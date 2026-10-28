#!/usr/bin/env python3
# Copyright (c) 2026
# SPDX-License-Identifier: Apache-2.0
"""Build the footprint matrix for the nRF5340-DK and attribute every FLASH byte.

Each build is one west build of app_footprint or app_cbor_persondb with a list
of Kconfig fragments (stages/ cumulative, variants/ one toggle each). For each
image the script reads:

  - the ELF section table, for the FLASH and RAM totals (FLASH = every
    allocated section with contents, wherever it runs; RAM = everything placed
    in RAM), cross-checked against the linker's own "FLASH used" line;
  - zephyr.map, to charge every input section in FLASH to the object file it
    came from, and that object file to a component. String literals and other
    symbol-less sections are attributed too, which rom_report leaves as
    "(hidden)". Whatever the map does not name (linker-script data, the gap
    the linker leaves between output sections) is charged to "linker".

  python3 app_footprint/footprint.py                 # build all, write results
  python3 app_footprint/footprint.py --only s04 p0   # rebuild some
  python3 app_footprint/footprint.py --no-build      # re-analyse existing builds

Needs ZEPHYR_BASE, west and an arm-zephyr-eabi toolchain (Zephyr SDK) in the
environment. Results go to app_footprint/results/footprint.json.
"""

import argparse
import datetime
import json
import os
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
BOARD = "nrf5340dk/nrf5340/cpuapp"
FLASH_ORIGIN, FLASH_SIZE = 0x00000000, 0x00100000
RAM_ORIGIN, RAM_SIZE = 0x20000000, 0x00070000

ST = HERE / "stages"
VA = HERE / "variants"

# --- the matrix -------------------------------------------------------------

LADDER = [
    # id,    fragment (None = prj.conf only), title, what it adds
    ("s00", None, "Zephyr base",
     "Kernel, console UART, printk: an empty main() on the DK"),
    ("s01", "01_log.conf", "+ logging", "CONFIG_LOG=y"),
    ("s02", "02_flash.conf", "+ flash driver",
     "QSPI NOR driver and flash_map, driven by flash_area_* from main"),
    ("s03", "03_ubi.conf", "+ UBI",
     "zephyr-ubi and PSA Crypto (Mbed TLS), driven by ubi_* from main"),
    ("s04", "04_blob_db.conf", "+ blob_db",
     "blob_db (L1) on its UBI backend; main now calls blob_db only"),
    ("s05", "05_rootreg.conf", "+ rootreg", "rootreg (L1½)"),
    ("s06", "06_kvhash.conf", "+ kvhash", "kvhash Map container (L2)"),
    ("s07", "07_logring.conf", "+ logring", "logring log container (L2)"),
    ("s08", "08_kvdb.conf", "+ kvdb", "kvdb string-key stores (L3)"),
    ("s09", "09_blobfs.conf", "+ blobfs", "blobfs paths and files (L3)"),
]

REF_STACK = "s08"   # variants toggle one thing against the full stack


def ladder_confs(upto, drop=()):
    out = []
    for sid, frag, *_ in LADDER:
        if frag and frag not in drop:
            out.append(ST / frag)
        if sid == upto:
            break
    return out


def builds():
    b = []
    for sid, frag, title, what in LADDER:
        b.append(dict(id=sid, group="ladder", app="app_footprint", title=title,
                      what=what, confs=ladder_confs(sid),
                      ref=None if sid == "s00" else b[-1]["id"],
                      frag=[ST / frag] if frag else []))

    def var(vid, title, frags, ref=REF_STACK, drop=(), what=""):
        frags = [VA / f for f in frags]
        b.append(dict(id=vid, group="variant", app="app_footprint",
                      title=title, what=what,
                      confs=ladder_confs(REF_STACK, drop) + frags,
                      ref=ref, frag=frags))

    var("v01", "blob_db on flash_area (no UBI)", ["backend_flash_area.conf"],
        drop=("03_ubi.conf",),
        what="The raw-partition backend; UBI and PSA Crypto leave the image")
    var("v02", "UBI: no atomic replace", ["ubi_no_atomic_replace.conf"])
    var("v03", "UBI: verify on read", ["ubi_verify_on_read.conf"])
    var("v04", "UBI: self checks", ["ubi_self_checks.conf"])
    var("v05", "blob_db: large payloads", ["large_payloads.conf"])
    var("v06", "blob_db: slot index", ["slot_index.conf"])
    var("v07", "blob_db: I/O stats", ["iostats.conf"])
    var("v08", "blob_db: inspect API", ["inspect.conf"])
    var("v09", "blob_db: autoformat", ["autoformat.conf"])
    var("v10", "Logging off", ["log_off.conf"], drop=("01_log.conf",),
        what="Every LOG_* call and string compiled out, and the core gone")
    var("v11", "Logging: errors only", ["log_err.conf"])
    var("v12", "Stack logs at WRN", ["log_stack_wrn.conf"])
    var("v13", "Zephyr shell", ["shell.conf"])
    var("v14", "+ blob_db shell", ["shell.conf", "blob_db_shell.conf"],
        ref="v13")
    var("v15", "-O2 instead of -Os", ["speed_opt.conf"])

    def pdb(pid, title, frags, what=""):
        frags = [VA / f for f in frags]
        b.append(dict(id=pid, group="persondb", app="app_cbor_persondb",
                      title=title, what=what, confs=frags,
                      ref=None if pid == "p0" else "p0", frag=frags))

    pdb("p0", "app_cbor_persondb", [],
        what="As shipped: UBI, benchmark frontend, I/O stats, logging")
    pdb("p1", "on flash_area", ["persondb_flash_area.conf"])
    pdb("p2", "without I/O stats", ["persondb_no_iostats.conf"])
    pdb("p3", "logging off", ["log_off.conf"])
    pdb("p4", "shell frontend", ["persondb_shell.conf"],
        what="Zephyr shell, persondb + blob_db commands, full-screen TUI")
    pdb("p5", "shell frontend, no TUI", ["persondb_shell_no_tui.conf"])
    return b


# --- building ---------------------------------------------------------------

def run_build(bd, build_dir):
    confs = ";".join(str(c) for c in bd["confs"])
    cmd = ["west", "build", "-p", "always", "-b", BOARD, "-d", str(build_dir),
           str(REPO / bd["app"])]
    if confs:
        cmd += ["--", f"-DEXTRA_CONF_FILE={confs}"]
    log = build_dir.parent / f"{bd['id']}.log"
    build_dir.parent.mkdir(parents=True, exist_ok=True)
    with open(log, "w") as f:
        rc = subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT,
                            cwd=REPO.parent).returncode
    return rc, log


# --- analysis ---------------------------------------------------------------

COMPONENTS = [
    # key, label, group
    ("kernel", "Zephyr kernel", "zephyr"),
    ("platform", "Arch, SoC, HAL, board drivers", "zephyr"),
    ("libc", "C library, compiler runtime", "zephyr"),
    ("zlib", "Zephyr libs (printk, cbprintf, heap, utils)", "zephyr"),
    ("linker", "Vectors, ISR tables, linker data", "zephyr"),
    ("logging", "Logging subsystem", "logging"),
    ("shell", "Zephyr shell", "shell"),
    ("flash", "Flash driver (QSPI NOR) + flash_map", "flash"),
    ("crc_rand", "CRC, random, entropy", "support"),
    ("psa", "PSA Crypto (Mbed TLS)", "crypto"),
    ("ubi", "zephyr-ubi", "ubi"),
    ("blob_db", "blob_db", "stack"),
    ("blob_db_be", "blob_db store backend", "stack"),
    ("rootreg", "rootreg", "stack"),
    ("kvhash", "kvhash", "stack"),
    ("logring", "logring", "stack"),
    ("kvdb", "kvdb", "stack"),
    ("blobfs", "blobfs", "stack"),
    ("zcbor", "zcbor", "app"),
    ("app", "Application", "app"),
]


def classify(obj, member_dirs):
    """Return (component, file label) for one map object path."""
    if obj in ("*fill*", ""):
        return "linker", obj or "(linker)"
    m = re.match(r"(.*)\(([^)]*)\)$", obj)
    arch, member = (m.group(1), m.group(2)) if m else (obj, os.path.basename(obj))
    a = arch.replace("\\", "/")
    name = member.replace(".c.obj", ".c").replace(".obj", "")
    if a.endswith("/libzephyr.a") or a == "zephyr/libzephyr.a":
        a = "zephyr/" + member_dirs.get(member, "lib/" + member)
    rules = [
        (r"isr_tables|zephyr_final|offsets\.dir|empty_file", "linker"),
        (r"libblob_db\.a|/lib/blob_db/", "blob_db"),
        (r"librootreg\.a|/lib/rootreg/", "rootreg"),
        (r"kvhash", "kvhash"),
        (r"logring", "logring"),
        (r"libkvdb\.a|/lib/kvdb/", "kvdb"),
        (r"blobfs", "blobfs"),
        (r"modules/ubi/|libubi\.a|zephyr-ubi", "ubi"),
        (r"mbedtls|tfpsacrypto|tf-psa-crypto", "psa"),
        (r"zcbor", "zcbor"),
        (r"(^|/)app/|CMakeFiles/app\.dir", "app"),
        (r"libkernel\.a|^zephyr/kernel/", "kernel"),
        (r"drivers__flash|drivers/flash|flash_map|storage", "flash"),
        (r"subsys__crc|subsys/crc|lib/crc|crc\d*_sw|crc\d*\.c"
         r"|subsys__random|subsys/random|drivers__entropy|drivers/entropy",
         "crc_rand"),
        (r"subsys__logging|subsys/logging", "logging"),
        (r"subsys__shell|subsys/shell", "shell"),
        (r"/arch/|/soc/|hal_nordic|nrfx|cmsis|/boards/|drivers__|/drivers/",
         "platform"),
        (r"libc\.a|libgcc\.a|picolibc|libc__|lib/libc|c_lib_ext|libc/",
         "libc"),
    ]
    full = f"{a}({member})"
    if "blob_db_store" in member:
        return "blob_db_be", name
    for pat, comp in rules:
        if comp and re.search(pat, full):
            return comp, name
    return "zlib", name


def elf_sections(elf):
    out = subprocess.run([os.environ["SIZE_READELF"], "-SW", str(elf)],
                         capture_output=True, text=True, check=True).stdout
    secs = []
    for line in out.splitlines():
        m = re.match(r"\s*\[\s*\d+\]\s+(\S+)\s+(\S+)\s+([0-9a-f]+)\s+[0-9a-f]+"
                     r"\s+([0-9a-f]+)\s+[0-9a-f]+\s+(\S*)", line)
        if m:
            name, typ, addr, size, flg = m.groups()
            secs.append((name, typ, int(addr, 16), int(size, 16), flg))
    return secs


def member_index(build_dir):
    idx = {}
    root = build_dir / "zephyr" / "CMakeFiles" / "zephyr.dir"
    for p in root.rglob("*.obj"):
        idx.setdefault(p.name, str(p.parent.relative_to(root)))
    return idx


MERGE = re.compile(r"\.(str|cst)\d+(\.\d+)?$")


def object_section_sizes(path, cache):
    """{(member, section): size} for an object or archive, via readelf."""
    if path not in cache:
        sizes = {}
        try:
            out = subprocess.run([os.environ["SIZE_READELF"], "-SW", path],
                                 capture_output=True, text=True).stdout
        except FileNotFoundError:
            out = ""
        member = os.path.basename(path)
        for line in out.splitlines():
            m = re.match(r"File: .*\((.*)\)$", line)
            if m:
                member = m.group(1)
                continue
            m = re.match(r"\s*\[\s*\d+\]\s+(\S+)\s+\S+\s+[0-9a-f]+\s+[0-9a-f]+"
                         r"\s+([0-9a-f]+)", line)
            if m:
                sizes[(member, m.group(1))] = int(m.group(2), 16)
        cache[path] = sizes
    return cache[path]


def pre_merge_size(obj, sec, build_dir, cache):
    m = re.match(r"(.*)\(([^)]*)\)$", obj)
    path, member = (m.group(1), m.group(2)) if m else (obj, os.path.basename(obj))
    full = path if os.path.isabs(path) else str(build_dir / path)
    return object_section_sizes(full, cache).get((member, sec))


def parse_map(map_path, flash_secs, build_dir):
    """{object: FLASH bytes} from a GNU ld map.

    ld lists a merged string/constant pool (.rodata.*str1.1, .cst4, ...)
    under the first object that contributed to it, with the size of the
    whole pool, and every later contributor with its own pre-merge size at
    the same location. Each pool is shared out in proportion to pre-merge
    size; the first contributor's is read back from its object file.
    """
    text = Path(map_path).read_text().splitlines()
    try:
        start = next(i for i, l in enumerate(text)
                     if l.startswith("Linker script and memory map"))
    except StopIteration:
        start = 0
    out = None
    pending = None
    per_obj = {}
    pools = {}      # (out, kind) -> [(sec, size, obj)]

    def add(sec, size, obj):
        mm = MERGE.search(sec)
        if mm:
            pools.setdefault((out, mm.group(0)), []).append((sec, size, obj))
        else:
            per_obj[obj] = per_obj.get(obj, 0) + size

    for line in text[start:]:
        if not line.strip():
            continue
        if not line.startswith(" "):
            out = line.split()[0]
            pending = None
            continue
        if out not in flash_secs:
            continue
        s = line.strip()
        parts = s.split()
        if s.startswith("*fill*"):
            if len(parts) >= 3:
                per_obj["*fill*"] = per_obj.get("*fill*", 0) + int(parts[2], 16)
            continue
        if s.startswith("*") or s.startswith("KEEP") or s.startswith("SORT"):
            continue
        if pending is not None:
            sec, pending = pending, None
            if (len(parts) >= 3 and parts[0].startswith("0x")
                    and parts[1].startswith("0x")):
                add(sec, int(parts[1], 16), " ".join(parts[2:]))
            continue
        if parts[0].startswith("0x"):
            continue
        if len(parts) == 1:
            pending = parts[0]
            continue
        if (len(parts) >= 4 and parts[1].startswith("0x")
                and parts[2].startswith("0x")):
            add(parts[0], int(parts[2], 16), " ".join(parts[3:]))

    cache = {}
    for entries in pools.values():
        pool = entries[0][1]
        first = pre_merge_size(entries[0][2], entries[0][0], build_dir, cache)
        if first is None:
            first = pool
        pre = [first] + [e[1] for e in entries[1:]]
        total = sum(pre) or 1
        shares = [pool * p // total for p in pre]
        shares[0] += pool - sum(shares)
        for (sec, _, obj), share in zip(entries, shares):
            per_obj[obj] = per_obj.get(obj, 0) + share
    return per_obj


def linker_flash(log):
    m = re.search(r"^\s*FLASH:\s+(\d+)\s+B", Path(log).read_text(), re.M)
    return int(m.group(1)) if m else None


def read_config(build_dir):
    cfg = {}
    for line in (build_dir / "zephyr" / ".config").read_text().splitlines():
        m = re.match(r"(CONFIG_\w+)=(.*)", line)
        if m:
            cfg[m.group(1)] = m.group(2)
        m = re.match(r"# (CONFIG_\w+) is not set", line)
        if m:
            cfg[m.group(1)] = "n"
    return cfg


def analyse(bd, build_dir, log):
    elf = build_dir / "zephyr" / "zephyr.elf"
    secs = elf_sections(elf)
    flash_secs, flash, ram = set(), 0, 0
    for name, typ, addr, size, flg in secs:
        if "A" not in flg or size == 0:
            continue
        in_ram = RAM_ORIGIN <= addr < RAM_ORIGIN + RAM_SIZE
        if typ != "NOBITS":
            flash_secs.add(name)
            flash += size
        if in_ram:
            ram += size
    per_obj = parse_map(build_dir / "zephyr" / "zephyr.map", flash_secs, build_dir)
    idx = member_index(build_dir)
    comps, files = {}, {}
    for obj, size in per_obj.items():
        c, f = classify(obj, idx)
        comps[c] = comps.get(c, 0) + size
        files.setdefault(c, {})
        files[c][f] = files[c].get(f, 0) + size
    # The linker's region total also counts the alignment gaps between
    # output sections; charge those to "linker" too.
    lf = linker_flash(log)
    if lf:
        flash = max(flash, lf)
    rest = flash - sum(comps.values())
    comps["linker"] = comps.get("linker", 0) + rest
    files.setdefault("linker", {})
    files["linker"]["(unnamed in map)"] = (files["linker"].get("(unnamed in map)", 0)
                                           + rest)
    return dict(flash=flash, ram=ram, linker_flash=lf,
                components={k: v for k, v in comps.items() if v},
                files={c: dict(sorted(v.items(), key=lambda kv: -kv[1]))
                       for c, v in files.items()},
                config=read_config(build_dir))


def git_rev(path):
    try:
        return subprocess.run(["git", "-C", str(path), "rev-parse", "--short=12",
                               "HEAD"], capture_output=True, text=True,
                              check=True).stdout.strip()
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build-root", default=str(REPO.parent / "build" / "footprint"))
    ap.add_argument("--out", default=str(HERE / "results" / "footprint.json"))
    ap.add_argument("--only", nargs="*", help="build ids to (re)build")
    ap.add_argument("--no-build", action="store_true")
    args = ap.parse_args()

    sdk = os.environ.get("ZEPHYR_SDK_INSTALL_DIR", "")
    os.environ.setdefault("SIZE_READELF", str(
        Path(sdk) / "gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-readelf"))

    root = Path(args.build_root)
    out_path = Path(args.out)
    old = {}
    if out_path.exists():
        old = {b["id"]: b for b in json.loads(out_path.read_text())["builds"]}

    results = []
    for bd in builds():
        bdir = root / bd["id"]
        log = root / f"{bd['id']}.log"
        if not args.no_build and (not args.only or bd["id"] in args.only):
            print(f"[{bd['id']}] building {bd['app']} {bd['title']}", flush=True)
            rc, log = run_build(bd, bdir)
            if rc:
                print(f"[{bd['id']}] FAILED, see {log}", file=sys.stderr)
                results.append(dict(old.get(bd["id"], {}), id=bd["id"],
                                    failed=True))
                continue
        if not (bdir / "zephyr" / "zephyr.map").exists():
            if bd["id"] in old:
                results.append(old[bd["id"]])
            continue
        r = analyse(bd, bdir, log)
        r.update(id=bd["id"], group=bd["group"], app=bd["app"],
                 title=bd["title"], what=bd["what"], ref=bd["ref"],
                 fragments=[str(c.relative_to(REPO)) for c in bd["confs"]],
                 toggles={str(f.relative_to(REPO)): [
                     l for l in f.read_text().splitlines()
                     if l.startswith("CONFIG_")] for f in bd["frag"]})
        ok = "ok" if r["linker_flash"] == r["flash"] else "MISMATCH"
        print(f"[{bd['id']}] FLASH {r['flash']} (linker {r['linker_flash']}, "
              f"{ok})  RAM {r['ram']}", flush=True)
        results.append(r)

    # The Kconfig delta each build carries against its reference.
    by_id = {r["id"]: r for r in results}
    for r in results:
        ref = by_id.get(r.get("ref"))
        cfg = r.pop("config", None) or {}
        if ref and "config_all" in ref:
            rc = ref["config_all"]
        else:
            rc = None
        r["config_all"] = cfg
        if rc is not None:
            keys = sorted(set(cfg) | set(rc))
            r["kconfig_changed"] = [
                f"{k}={cfg.get(k, 'n')}" for k in keys
                if cfg.get(k, "n") != rc.get(k, "n")]
    for r in results:
        r.pop("config_all", None)

    meta = dict(board=BOARD, date=datetime.date.today().isoformat(),
                zephyr=git_rev(Path(os.environ.get("ZEPHYR_BASE", ""))),
                key_value_db=git_rev(REPO),
                zephyr_ubi=git_rev(REPO.parent / "modules/lib/zephyr-ubi"),
                sdk=Path(sdk).name if sdk else None,
                components=[dict(key=k, label=l, group=g) for k, l, g in COMPONENTS])
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(dict(meta=meta, builds=results), indent=1) + "\n")
    print(f"wrote {out_path}")


if __name__ == "__main__":
    main()
