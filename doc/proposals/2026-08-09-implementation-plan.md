# Implementation plan — large payloads in `blob_db`

2026-08-09 · plan for `2026-08-09-large-payloads.md` + its cost addendum
· impact notes: `2026-08-09-kvhash-impact.md`, `2026-08-09-rootreg-kvdb-impact.md`

---

## 0. Assumptions

This plan is written against the recommended answer to every open decision.
Five are structural enough to name:

| | Taken as | Effect if reversed |
|---|---|---|
| **D10** layering | segmentation **inside** L1 | plan is void — a shared L1½ module is a different design |
| **D0** format compat | frozen prefix, breaking change, **PR 1** | later PRs each need their own compat story |
| **D9** ordering | streaming walk **before** segmentation | swap PR 2 and PR 3; ~80 lines of churn (see §2) |
| **D7** pwrite | yes, `blob_db_write()` | PR 4 drops; `blobfs` writes stay O(file) |
| **D3** segment ids | same `alloc_id` counter | separate namespace needs its own durability story |

The rest (D1 streaming writer, D4 `get` on large blobs, D5 payload CRC32,
D6 default inline length, D8 chunk defaults) affect one PR each and are noted
where they land.

---

## 1. PR sequence

| PR | Scope | Size | Risk | Delivers | Format break |
|---|---|---|---|---|---|
| **1** | Format compatibility + Stage-1 housekeeping | ~250 LOC | low | old code can detect/refuse future formats; R7 fix | **yes** |
| **2** | Streaming slot walk | ~500 LOC | **high** | −128 KB `.bss` on QSPI NOR, 8×→1× read | no |
| **3** | Segmentation | ~700 LOC | medium | payloads to ~505 KB | additive |
| **4** | Segmented pwrite | ~150 LOC | low | O(chunk) writes, not O(object) | no |
| **5** | Index cache | ~80 LOC | low | halves sequential read cost | no |

PRs 1 and 3 are the ones the original ask needs. PR 2 is independent of the
feature and is sequenced early on value, not necessity. PRs 4 and 5 matter only
once there is a filesystem client.

---

## 2. PR 1 — format compatibility + Stage-1 housekeeping

Combined because both reshape how a slot and a master are written, and both are
prerequisites for everything after.

**Changes**

`lib/blob_db/blob_db_internal.h`
- `BLOB_DB_FORMAT_MAJOR 1`, `BLOB_DB_FORMAT_MINOR 0`
- new `struct blob_db_compat_hdr` (12 B, `BUILD_ASSERT`) — frozen forever
- `blob_db_master_hdr` becomes prefix + body; `BUILD_ASSERT` total == 32

`lib/blob_db/blob_db.c`
- `write_master()` — fill prefix, `prefix_crc16` over the leading 10 B, keep
  `hdr_crc32` over the body
- `read_master()` — return a tri-state (`OK` / `ERASED` / `FOREIGN`) instead of
  a `bool valid`; this is the crux of the change
- new `region_is_erased(off, len)` — reads in ≤64 B chunks, no large buffer.
  Only the master header region is checked; `blob_db` never writes a master
  anywhere but offset 0, so an all-`0xff` header means an erased sector
- `blob_db_mount()` — the decision tree from proposal §4.1, replacing the
  current `if (!va && !vb) → format` at `:304`
- `append_slot()` — stage in `g_bbuf_new` instead of the
  `MAX_PAYLOAD + 46` stack frame (`:593`); add an `__ASSERT` busy guard, since
  the safety argument is "compaction has finished before any append"
- mount-time geometry check:
  `MAX_PAYLOAD_LEN <= (peb_size − 16)/2 − 14`, else `-ENOTSUP` naming both
  numbers — same shape as the existing `SECTOR_BUF_SIZE` check at `:274`

`lib/blob_db/Kconfig`
- `BLOB_DB_MAX_PAYLOAD_LEN` range `1 4096` → `1 65535` (D6: default stays 256)
- new `BLOB_DB_AUTOFORMAT_ON_CORRUPT`, default `y`

Docs: `impl/l1_bucketlog.md` §3.1 and §13.3/§13.4 (both close);
`layers/l1_blob_db.md` mount errors (its `-ENOTSUP` promise becomes true).

**Tests** — `tests/lib/blob_db/`, following the existing forge pattern at
`src/main.c:354-395`, which already injects a master via `flash_area_write`.

1. erased partition still formats (existing test 1 must pass unchanged)
2. unknown `format_major` → `-ENOTSUP`, **and the partition bytes are
   unmodified** (the assertion that matters — today it would be reformatted)
3. wrong magic → `-ENOTSUP`, unmodified
4. non-erased garbage, bad prefix CRC → `-ENOTSUP`, unmodified
5. A = major 1 gen 5, B = major 2 gen 6 → `-ENOTSUP`; must **not** fall back
   to A
6. valid prefix, bad body CRC on both × `AUTOFORMAT_ON_CORRUPT` y/n →
   formats / `-EIO` (needs a second testcase.yaml entry with the Kconfig
   flipped)
7. payload cap above the geometric bound → mount `-ENOTSUP`

**Done when** twister is green on `native_sim`, the UBI-backend build passes,
and both nRF5340 builds pass (they run with 64 KB sectors, so the geometry
check is exercised on a second geometry).

**Verify no existing config breaks:** `MAX_PAYLOAD_LEN` is 256 by default and
1024 in `app_perf_kvdb/prj.conf`; the new bound is 2026 on 4 KB sectors and
32 746 on 64 KB. Both clear.

---

## 3. PR 2 — streaming slot walk

**Gate: this one needs a short design note before code.** It changes documented
behaviour and one existing test, and the semantics are not obvious.

Today `read_bucket()` pulls a whole erase block into `g_bbuf` and
`slot_view_at()` CRC-verifies every slot as `walk_bucket()` steps over it.
Streaming means stepping by slot *header* (4 B gives `val_len`, hence the slot
size) and reading a payload only when it is actually wanted.

**The semantic question.** CRC verification currently doubles as end-of-log
detection: a slot that fails CRC terminates the bucket, and
`l1_bucketlog.md` §10 documents that "everything after it in that bucket is
unreachable until next compaction". Header-only walking cannot verify CRC while
skipping, so a corrupt slot would instead be *skipped*, leaving later slots
reachable. That is strictly better behaviour — but it is a change, and
`test_corrupted_slot_truncates_bucket` (`src/main.c:213`) asserts the old one.
The note must settle: skip-corrupt vs terminate, and what the write cursor does
when it walks past a corrupt slot.

**Scope.** Touches nearly every function in `blob_db.c`: `walk_bucket`,
`slot_view_at`, `for_each_live_slot`, `scan_bucket`, `get`, `delete`, `exists`,
`count`, `iterate`, `build_compacted_image`, `compact_commit`,
`recover_compaction`. Compaction becomes a streaming copy old→scratch→bucket
through a small buffer instead of two sector images.

**Payoff.** The resident buffer drops from `peb_size` to `MAX_PAYLOAD_LEN`-ish:
on the QSPI board `.bss` goes from ~128 KB to ~2 KB, and read amplification
from 8× to ~1×. On `native_sim` it is 8 KB → ~1 KB.

**Ordering note.** Doing this *before* PR 3 means segmentation's read path is
written once against the final internals. Doing it after costs ~80 lines of
rework in `index_load`/`seg_fetch`, but lets the segmentation suite stress the
refactor. If shipping large payloads is time-critical, swapping PR 2 and PR 3
is defensible; otherwise keep this order.

---

## 4. PR 3 — segmentation

**New file** `lib/blob_db/blob_db_seg.c`, productionized from
`doc/proposals/sizing/blob_db_seg.c` (that sketch is a sizing artefact, not a
starting point to copy verbatim — it stubs its integration).

`blob_db_internal.h` — `BLOB_DB_SLOT_F_INDEXED`/`_SEGMENT`,
`blob_db_index_hdr` (16 B), `blob_db_seg_hdr` (12 B); master body gains
`seg_owner` (u64), bumping `format_minor` and `hdr_len`, **not**
`format_major`.

`blob_db.c`
- export the helpers the new file needs (`bdb_*` accessors)
- `append_slot2()` — scatter variant taking `(hdr, payload)`, so a segment
  assembles into the staging buffer with no third buffer
- dispatch on `INDEXED` in `get`, on size in `update`, on `INDEXED` in `delete`
- `for_each_live_slot()` skips `SEGMENT` slots, so `count`/`iterate` keep
  reporting logical blobs
- `set_seg_owner()` + mount hook: run the sweep only when `seg_owner != 0`

`include/app/lib/blob_db.h` — `blob_db_size()`, `blob_db_read()` (D4: `get`
keeps returning `-ENOMEM`; D5: payload CRC32 included in the index).

`Kconfig` — `BLOB_DB_LARGE_PAYLOADS` (default n), `BLOB_DB_MAX_OBJECT_LEN`,
`BLOB_DB_SEGMENT_LEN` (D8: auto → 1024 on 4 KB sectors, 8192 on 64 KB),
`BLOB_DB_MAX_SEGMENTS`. `CMakeLists.txt` compiles the new file conditionally.

**Tests** — proposal §9, plus the two placement behaviours from the cost
addendum §2: segment `-ENOSPC` retries with a fresh id, and prepared buckets
skip the cursor read. Crash injection goes in
`tests/lib/blob_db_contract/`, which already has the harness
(`crash_and_recover()`, `src/main.c:68`); the new crash table is the five steps
of proposal §6.4, asserting after each that the payload is wholly old or wholly
new **and** that no unreferenced segment survives the sweep.

**The P4 test matters most:** a `LARGE_PAYLOADS=n` build must produce
byte-identical flash for the existing suite. That is the guarantee the three
impact notes rest on.

---

## 5. PR 4 — segmented pwrite · PR 5 — index cache

**PR 4** adds `blob_db_write(id, offset, buf, len)`: read the index, rewrite
only the segments the range touches, re-commit the index. Same commit point,
same sweep rule. Turns a 4-byte write into a 1-segment write instead of a
whole-object rewrite (33–67×).

**PR 5** caches the last-read index — owner id, the already-allocated
`g_idx_a` table, and an invalidate-on-mutation counter. ~16 B of new `.bss`,
halves sequential read cost. Do it last: it is an optimisation whose
correctness depends on invalidation being right everywhere else first.

---

## 6. Risks

| Risk | Mitigation |
|---|---|
| PR 2 is a core refactor touching every function | design note first; land alone; PRs 1 and 3 do not depend on its internals |
| PR 2 changes documented crash behaviour | settle it in the note, update `l1_bucketlog.md` §10 and the affected test in the same PR |
| Segmentation bugs are crash-path bugs — the hardest to catch | extend the existing contract suite rather than writing a new harness; the five-step crash table is mechanical |
| The sweep is O(N) sector reads | only runs when `seg_owner != 0`; assert in tests that a clean mount never triggers it |
| `MAX_SEGMENTS` mis-sized silently caps object size | `BUILD_ASSERT` that `MAX_OBJECT_LEN / segment_len <= MAX_SEGMENTS` |
| UBI backend diverges | CI already builds it; both backends go through the same PEB store interface, so no backend-specific work is expected — but PR 1 and PR 3 should each be smoke-tested on UBI before merge |

---

## 7. Out of scope

- **`map_ops` partial access** — required before `kvdb` can expose large
  values, and an L2 contract change (`2026-08-09-rootreg-kvdb-impact.md`).
- **`kvhash` directory via pread** — the cheap follow-up that removes its
  31-bucket cap and 256 B of `.bss`.
- **`blobfs`** — the consumer that motivated the filesystem framing.
- **Extent allocator** — contract D1's `BLOB_DB_ALLOC_EXTENT`, the endpoint if
  large objects become the primary workload rather than one client among
  several.
- **Streaming write API** (D1) — deferred until a caller cannot hold a whole
  object in RAM; `blob_db_write()` from PR 4 covers the filesystem case.
