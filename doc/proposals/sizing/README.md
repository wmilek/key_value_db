# Sizing harness for the large-payload proposal

**This is not project code and is not built by CMake.** It exists so the
code-size and RAM figures in
`doc/proposals/2026-08-09-large-payloads-cost.md` §1 can be re-derived
instead of taken on trust.

```
sh doc/proposals/sizing/measure.sh          # or: ... measure.sh arm-none-eabi-gcc
```

| File | What it is |
|---|---|
| `measure.sh` | compiles both objects at `-Os` and sums their sections |
| `blob_db_seg.c` | a **sketch** of the segmentation layer proposed in `2026-08-09-large-payloads.md` §6 — real logic (index load, segment fetch, pread, segmented write with master intent, orphan sweep), stubbed integration |
| `stub/zephyr/` | minimal stand-ins for `kernel.h`, `log.h`, `crc.h`, `util.h`, `toolchain.h` so `blob_db.c` compiles outside a Zephyr tree |

Caveats, repeated from the cost document:

- The figures in the document were taken with host `gcc` on x86-64, because
  no ARM cross-compiler was available. Pass `arm-none-eabi-gcc` as the first
  argument to get Cortex-M numbers, which are the ones that matter for a
  target build.
- `blob_db_seg.c` calls into helpers (`bdb_append`, `bdb_walk_bucket`, …) that
  would be exported from `blob_db.c` in a real integration. It links against
  nothing; only its object size is meaningful.
- The cost of *modifying* `blob_db.c` (flag dispatch in `get`/`update`/
  `delete`/`for_each_live_slot`, the scatter `append2`, the master
  `seg_owner` field) is **not** measured here — it is the `~300–500 B`
  estimate in the document's table.
