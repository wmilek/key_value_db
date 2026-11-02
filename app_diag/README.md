# app_diag — kvdb diagnostics shell

An application that does nothing but expose a shell over the storage stack.
The same source builds for the nRF5340-DK, where it is the on-device
diagnostics firmware, and for `native_sim`, where it is a host tool working on
a partition image. The plan it is the first step of is
[`doc/proposals/2026-11-02-kvdb-tools.md`](../doc/proposals/2026-11-02-kvdb-tools.md).

## What it contains

The app registers no commands of its own. It enables the groups the libraries
and Zephyr provide.

| Group | Source | Needs a mount | Writes |
|---|---|---|---|
| `blob_db info` · `stats` · `buckets` · `bucket` · `id` · `dump` | `lib/blob_db/blob_db_shell.c` | yes | no |
| `blob_db export [id [off len]]` | `lib/blob_db/blob_db_shell_xfer.c` | yes | no |
| `blob_db maintain [budget]` | `lib/blob_db/blob_db_shell.c` | yes | with a budget: erases |
| `blob_db iostats [reset]` | same (`CONFIG_BLOB_DB_IOSTATS`) | no | no |
| `blob_db mount` · `unmount` | same (`CONFIG_BLOB_DB_SHELL_ADMIN`) | no | `mount` may: see below |
| `blob_db format yes` | same (`CONFIG_BLOB_DB_SHELL_ADMIN`) | no | erases the partition; leaves it mounted |
| `blob_db import <line>` | `blob_db_shell_xfer.c` (`CONFIG_BLOB_DB_SHELL_ADMIN`) | yes, freshly formatted | binds objects |
| `flash read` · `write` · `erase` · … | Zephyr `CONFIG_FLASH_SHELL` | no | `write`, `erase`, `copy`, `load`, `test` |
| `kernel …`, `device list` | Zephyr kernel and device shells | no | no |
| `kernel reboot cold` · `warm` | Zephyr kernel shell (`CONFIG_REBOOT`) | no | no; the store sees a power cut between operations |

## No automatic mount

`blob_db_mount()` is not read-only. It finishes an interrupted compaction from
the scratch seal, formats a partition whose master sectors both read as
erased, and on UBI attaches (or formats) the volume. Any of those can destroy
the state a diagnostic session was opened to look at, so by default nothing
mounts:

- `main()` mounts only with `CONFIG_APP_DIAG_MOUNT_AT_BOOT=y` (default `n`).
  Otherwise the store is mounted when `blob_db mount` (or
  `blob_db format yes`) is typed.
- `CONFIG_BLOB_DB_AUTOFORMAT_ON_CORRUPT=n` and `CONFIG_BLOB_DB_MAINT_WORK=n`
  are pinned in `prj.conf`.
- blobfs is not built, so the file system it registers cannot be a path to a
  mount either.

Until then the `blob_db` inspection commands answer `blob_db not mounted`,
and the partition is reached only through the `flash` commands. On
`native_sim` a session without a mount leaves the image byte-identical.

`blob_db mount` writes nothing when it refuses. `-ENOTSUP` means a format or
geometry this build does not support. `-EIO` means a flash error, or no usable
master on a partition that is not blank. `blob_db format yes` discards such a
store; without the word `yes` it does nothing.

## Logging

The build compiles in the stack's debug messages, which a product build leaves
out, and shows them from boot: `blob_db` (including the store backend) and UBI
at DBG. Mount's classification of the two masters (`master A=ok B=erased`,
`A=foreign`) and the reason for a refusal are logged by the library. The
shell's reply lists what each refusal can mean and the remedy for each, and
the log line above it says which one happened.

Logging is immediate, so a message prints before the shell's reply to the
command that caused it, and a burst is not dropped. `log enable inf blob_db`
(or `wrn`, or another module) turns a module down at run time.

## Geometry and format

An image is readable only by a build that matches the firmware that wrote it:

- **Geometry:** the storage partition is 8 MB with 64 KB erase blocks and
  4 B write alignment, the nRF5340-DK's MX25R64 partition (the same overlay
  as `app_perf_blob_db`).
- **Backend:** UBI, with the built-in key. A store sealed with
  `CONFIG_BLOB_DB_UBI_KEY_APP` cannot be attached without that key.
- **Format options:** `BLOB_DB_LARGE_PAYLOADS` and `BLOB_DB_MAX_PAYLOAD_LEN`
  must match.

Pass any of these as `-D` options to match another firmware.

## Host tool (native_sim)

```bash
west build -b native_sim -d build/diag key_value_db/app_diag
```

On `native_sim` the store has a simulated flash device of its own. The
partition starts at offset 0 of that device and fills it, so the device's
backing file *is* a partition image. The flash simulator names its options
after the device label:

```bash
./build/diag/zephyr/zephyr.exe --kvdb=storage.bin
```

- Always pass `--kvdb`. Without it the store uses `flash1.bin` in the current
  directory.
- A file that does not exist is created and erased (8 MB of `0xff`).
- `--kvdb_in_ram` works on a blank in-RAM device and writes no file.

The shell is on the terminal the tool was started from, so commands can also
be piped in. `-stop_at=<seconds>` ends the run. `kernel reboot cold` re-runs
the tool with the same arguments, so the image named by `--kvdb` carries
across it. Each reboot starts the run, and `-stop_at`, from zero:

```bash
printf 'blob_db info\nblob_db stats\n' | \
  ./build/diag/zephyr/zephyr.exe --kvdb=storage.bin -stop_at=2
```

To inspect the store, mount it first:

```bash
printf 'blob_db mount\nblob_db info\nblob_db stats\n' | \
  ./build/diag/zephyr/zephyr.exe --kvdb=storage.bin -stop_at=2
```

Make a copy of the image first if it must stay as it was: the mount may
write to it. To get a fresh empty store in a new file, run
`blob_db format yes`.

In the shell, the store's flash device is `flash-controller@80000000`:

```
kvdb:~$ flash read flash-controller@80000000 0 32
00000000: 55 42 49 23 01 00 00 00  00 00 00 00 00 00 00 02 |UBI#.... ........|
```

## Export and import

`blob_db export` prints every live object as checksummed text lines
(`%BDB1 ...`), and `blob_db import` takes them back into a freshly formatted
store, one line per command, keeping every id and `next_id`. The format, the
replies and the limits are in
[`doc/tools/blob_db_export.md`](../doc/tools/blob_db_export.md). The host side
is `scripts/blob_db_export.py`.

Each line carries its own CRC, so log output printed between or inside the
lines does no harm: `verify` drops a damaged line and names the
`blob_db export <id> <off> <len>` command that prints its data again.

```bash
printf 'blob_db mount\nblob_db export\n' | \
  ./build/diag/zephyr/zephyr.exe --kvdb=storage.bin -stop_at=10 > dump.log
python3 key_value_db/scripts/blob_db_export.py verify dump.log
python3 key_value_db/scripts/blob_db_export.py extract dump.log -o store/
```

Restoring into a new image on the host is a pipe. `-no-rt` runs the
simulation as fast as the host can, so a large dump does not take its
simulated time in real time:

```bash
python3 key_value_db/scripts/blob_db_export.py commands --format dump.log | \
  ./build/diag/zephyr/zephyr.exe --kvdb=restored.bin -no-rt -stop_at=600
```

The same works from `store/`, after editing it, or with a directory made by
hand (a `manifest.json` as `extract` writes it). Since a dump holds objects,
not sectors, the restoring build may differ from the one that exported:
another geometry, the other backend, a later format.

The host build stages objects of up to 128 KB
(`CONFIG_BLOB_DB_SHELL_IMPORT_BUF_SIZE`); a device build, one slot's payload
by default. On a device, `import` lines have to be sent one at a time, each
after the previous reply: the shell has no flow control on its input.

## On the device (nRF5340-DK)

```bash
west build -b nrf5340dk/nrf5340/cpuapp -d build/diag_dk key_value_db/app_diag
```

The shell runs on the board's console UART. The storage partition is the first
8 MB of the external MX25R64. A raw dump of that range opens unchanged with
`--kvdb=` on the host build.

## Tests

`sample.yaml` runs these scenarios on `native_sim` under twister:

- `app.diag`: boots to the shell, unmounted.
- `app.diag.mount`: mounts at boot, formatting a blank image.
- `app.diag.admin`: twister's shell harness types the commands in
  `test_shell.yml`. Format is refused without `yes`; then mount, a second
  mount, unmount, format.
- `app.diag.reboot`: `pytest/test_reboot.py` formats, reboots, and checks the
  app comes back unmounted and mounts the same store without formatting.
- `app.diag.export`: `pytest/test_export.py` imports a dump made by
  `scripts/blob_db_export.py` line by line (a damaged line, a resent one, a
  header to a store no longer empty), exports the store, and compares every
  object and `next_id`. One bucket holds 41 ids, more than export gathers in
  one pass.
- `app.diag.export.large`: the same with `CONFIG_BLOB_DB_LARGE_PAYLOADS=y`,
  so one object is segmented.
