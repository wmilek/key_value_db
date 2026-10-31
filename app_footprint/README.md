# `app_footprint` — what the storage stack costs in flash

A measuring app, not a demo. It links the stack one layer at a time on the
nRF5340-DK and charges every FLASH byte of each image to the component it came
from. Numbers are in [`RESULTS.md`](RESULTS.md); the raw data is
[`results/footprint.json`](results/footprint.json).

**ROM only, on purpose.** RAM is recorded too, but buffer sizing
(`CONFIG_BLOB_DB_SECTOR_BUF_SIZE`, stacks, the UBI heap) is not final, so the
RAM column is for reference and should not be quoted.

## How it works

`src/main.c` has no options of its own. Each block in it is gated on a
library's **own** Kconfig symbol, and calls every public function of that
layer, so the linker keeps what a real caller would link:

| Gate | What `main()` calls |
|---|---|
| `CONFIG_FLASH_MAP` (and no UBI, no blob_db) | `flash_area_open/erase/write/read` |
| `CONFIG_UBI` (and no blob_db) | PSA key import, `ubi_device_*`, `ubi_volume_*`, `ubi_leb_*`, `ubi_maintenance` — the calls blob_db's UBI backend makes |
| `CONFIG_BLOB_DB` | the whole `blob_db.h` API; `blob_db_iostats_*` / `blob_db_inspect_*` when those are on |
| `CONFIG_BLOB_ROOTREG` | the whole `rootreg.h` API |
| `CONFIG_BLOB_CONTAINER_KVHASH` | every `map_ops` entry of `kvhash_map_ops` |
| `CONFIG_BLOB_CONTAINER_LOGRING` | the whole `logring.h` API |
| `CONFIG_BLOBDB_KVDB` | the whole `kvdb.h` API |
| `CONFIG_BLOBDB_BLOBFS` | the path API of `blobfs.h` |

Only the topmost raw layer is driven directly: once blob_db is in, it is
blob_db that calls UBI and flash_area.

The builds are lists of Kconfig fragments:

- [`stages/`](stages) — cumulative. Stage *n* is `prj.conf` plus fragments
  1..*n*: logging, flash driver, UBI (+ PSA Crypto), blob_db, rootreg, kvhash,
  logring, kvdb, blobfs.
- [`variants/`](variants) — one toggle each, applied to the full stack
  (stages 1–8) or to `app_cbor_persondb`.
- `variants/persondb_minimal.*`, `persondb_single_thread.conf` and
  `persondb_ubi_minimal.conf` with `persondb_ubi_psa.h` are the exception:
  bundles that build the smallest `app_cbor_persondb` that still runs, the
  whole image (`RESULTS.md` §4) or the storage stack on UBI (§5).
  `footprint.py` does not build them.

`app_cbor_persondb` is built as shipped and with a few of the same fragments, as
the finished application.

`kvlist`, `kvtree`, `seq` and the shared intent helper are skeletons
(`NOT YET IMPLEMENTED`) and are not measured.

## Running it

Needs west, `ZEPHYR_BASE` and the Zephyr SDK (`arm-zephyr-eabi`). The numbers
in `RESULTS.md` were taken with SDK 1.0.1.

```
# one rung
west build -b nrf5340dk/nrf5340/cpuapp key_value_db/app_footprint \
  -- -DEXTRA_CONF_FILE="$PWD/key_value_db/app_footprint/stages/01_log.conf;$PWD/key_value_db/app_footprint/stages/02_flash.conf"

# the whole matrix (~30 builds, ~1 min each), attributed
python3 key_value_db/app_footprint/footprint.py
python3 key_value_db/app_footprint/footprint.py --only s04 p0   # rebuild some
python3 key_value_db/app_footprint/footprint.py --no-build      # re-analyse
```

Builds go to `<workspace>/build/footprint/<id>`, results to
`results/footprint.json`.

## How bytes are attributed

- **FLASH** is the linker's "FLASH used" for the region: code, read-only data
  and the initial values of RAM data (`text + data`). The script reads it from
  the build log and checks it against the ELF section table.
- Every input section the linker placed in a FLASH-loaded output section is
  charged to the object file named for it in `zephyr.map`, and each object to a
  component by its archive and source path. String literals and other
  symbol-less sections are attributed this way too, which `rom_report` leaves
  as "(hidden)".
- GNU ld lists a merged string pool (`.rodata.*.str1.1`) under the first file
  that contributed to it, with the size of the whole pool, and every other
  contributor with its pre-merge size at the same address. The script shares
  each pool out in proportion to each file's pre-merge strings, reading the
  first file's from its object.
- What the map does not name (linker-script data, alignment between output
  sections) goes to "linker". Each build's components sum to the linker total
  exactly.
