# littlefs vs blobfs harness

**This is not project code and is not built by CI.** It exists so the figures in
`doc/comparisons/2026-10-11-littlefs-vs-blobfs.md` can be re-derived instead of
taken on trust.

One application, `src/main.c`, runs the same `fs_*` workload against whichever
file system the variant fragment enables. On `native_sim` every flash operation
is counted by the flash simulator — below both stacks, so neither is measured
by its own instrumentation — and printed per phase as an `io` line that
`app_perf_l0/tools/l0_timing.py predict` reads. On a board the build exists for
its image size.

| Variant (`variants/*.conf`) | What is in the image |
|---|---|
| `base` | kernel, flash driver, the harness; no storage stack |
| `lfs` | littlefs on its own partition, Zephyr defaults |
| `lfs_tuned` | `lfs` with `CONFIG_FS_LITTLEFS_CACHE_SIZE=512`, so small files are stored inline |
| `lfs_settings` | `lfs` + Zephyr `settings` on a file in it (littlefs as the only store) |
| `kvdb_ubi` | blob_db on UBI + rootreg + kvhash + kvdb (the multi-purpose stack, no files) |
| `blobfs_ubi` | `kvdb_ubi` + blobfs + its VFS driver |
| `blobfs_ubi_ar` | `blobfs_ubi` with `CONFIG_BLOB_DB_UBI_ATOMIC_REPLACE=y` |
| `blobfs_fa` | `blobfs_ubi` on the raw partition instead of UBI |
| `blobfs_ubi_lp` | `blobfs_ubi` with `CONFIG_BLOB_DB_LARGE_PAYLOADS=y` |
| `lfs_kvdb_ubi` | `kvdb_ubi` for key/value + littlefs on a second partition for files |

Both board files split the DK's 8 MB MX25R64 into two 4 MB partitions,
`storage` (blob_db) and `lfs` (littlefs), so each stack gets the same space.
`native_sim` is given the MX25R64's 64 KB erase blocks and 4 B write alignment,
as `app_perf_l0/geometry/mx25r64.overlay` does.

## Running

From the workspace top directory. littlefs is not in this repository's west
allowlist, so clone it once (the revision Zephyr's `west.yml` pins) and pass
it as an extra module:

```shell
git clone https://github.com/zephyrproject-rtos/littlefs modules/fs/littlefs

H=key_value_db/doc/comparisons/fs_compare
LFS=-DEXTRA_ZEPHYR_MODULES=$PWD/modules/fs/littlefs

# image size, per variant
west build -p always -b nrf5340dk/nrf5340/cpuapp -d build/fsc_lfs $H -- \
    $LFS -DEXTRA_CONF_FILE=variants/lfs.conf
west build -d build/fsc_lfs -t rom_report      # and ram_report

# flash operations: run twice on one flash file — the first run formats and
# runs the workload, the second is a cold boot onto the populated store
west build -p always -b native_sim -d build/fsc_sim_lfs $H -- \
    $LFS -DEXTRA_CONF_FILE=variants/lfs.conf
rm -f lfs.bin
build/fsc_sim_lfs/zephyr/zephyr.exe --flash=lfs.bin --no-rt --stop_at=1000 | tee run1.log
build/fsc_sim_lfs/zephyr/zephyr.exe --flash=lfs.bin --no-rt --stop_at=1000 | tee run2.log

python3 key_value_db/app_perf_l0/tools/l0_timing.py predict \
    -m key_value_db/app_perf_l0/models/mx25r64_nrf5340dk_direct.json run1.log
```

`--no-rt` matters: `native_sim` otherwise paces itself to wall-clock and the
run never reaches `--stop_at` in reasonable time.

## Caveats

- The figures in the comparison were cross-built with Debian's
  `arm-none-eabi-gcc` 13.2 and `CONFIG_MINIMAL_LIBC=y`, not the Zephyr SDK.
  Zephyr `main` calls `__rbit()`, which GCC 13's `arm_acle.h` lacks; the build
  force-included a three-line inline-asm definition of it. Absolute sizes will
  differ slightly under the SDK's GCC and picolibc; the differences between
  variants are what the document uses.
- The prediction is flash time only. CPU time between flash calls (CRC,
  memcpy, littlefs's metadata parsing, blob_db's slot walk) is not modelled.
- The UBI variants trigger the timing tool's "partition mismatch" warning: it
  compares the model's 8 MB against the UBI volume blob_db reports (3 728 256 B of
  the 4 MB partition). The model is per operation, so the prediction stands.
