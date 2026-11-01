# Implementation plan — the master as a log, and format features

2026-11-01 · plan for `2026-10-28-master-log.md` (cited below as "the proposal")
· written against `main` at `d5461a0`
· touches `lib/blob_db/`, `include/app/lib/blob_db_{inspect,iostats}.h`,
`tests/lib/blob_db/`, `app_perf_blob_db/`, `doc/impl/l1_bucketlog.md`

---

## 0. Scope

Phase 1 of the proposal is planned here in full: major 5, the log, rotation,
the mount search and doubt rule, geometry, the feature sets with
`LARGE_PAYLOADS`, and the fuse word. Phases 2 (PR #63) and 3 (PR #62) are
rebases onto it, and §8 lists only what each one has to change.

Reading the code against the proposal turned up eight points where the
proposal is incomplete or, in two cases, unsafe as written. §1 lists them
with the fix this plan uses. The two unsafe ones are A1 and A2: as written,
each lets an id be issued twice. Both fixes are small, and each should go
back into the proposal before PR 2 lands.

## 1. Changes to the proposal

| # | Proposal says | Problem | This plan |
|---|---|---|---|
| **A1** | §3.5: doubt when "the other sector is neither erased nor ok" | **A torn rotation hides lost ceilings.** Suppose sector Y is active and its header rots. Mount falls back to X, which is full and has an older ceiling. Doubt (Y is corrupt), so mount scans and raises the ceiling. Persisting the new ceiling needs a rotation into Y. If power is cut after Y's erase, the next mount sees X ok and Y erased, finds no doubt, and takes X's old ceiling. Ids above it that are already on flash get issued again. | Also doubt when **the other sector is erased and the active one is full**. Rotation only ever leaves a full sector behind, so the non-active ok sector is always full. A full sector beside an erased one is therefore exactly what a rotation torn after its erase looks like. Right after a format the other sector is erased too, but the active one is not full, so this costs nothing in the common case. |
| **A2** | §3.3: "`cursor += R` also on failure" | **A failed append can leave a hole.** If `flash_area_write()` fails before programming anything (a driver timeout, `-EIO` from UBI), the slot stays erased. The next update writes the slot after it. The binary search (D3) assumes written slots form a prefix, so it can stop at the hole and adopt the record before it. That record verifies, so there is no doubt, and an acknowledged ceiling is lost. | **After a failed append, the sector is closed.** A RAM flag makes every later update rotate. In the same call, the failed append is followed by one rotation attempt, so the update still commits if the other sector works. In the active sector the written slots stay a strict prefix, and the binary search stays correct. It costs one erase, and only on a write failure. |
| A3 | §3.4: new header gets `rot_gen + 1` | After a fallback the active sector is the *older* one, so +1 can equal the rot_gen of the sector it is about to replace. | `rot_gen = max(rot_gen of every ok header) + 1`. |
| A4 | §4.4: check `store.incompat` = header ∪ newest record | If the newer sector carries a bit and mount falls back to the older one (A3's case), the store may already hold slots using that bit. | Check the union of **both ok headers** and the adopted record. |
| A5 | §3.1: `rec_size` stored | Nothing says what a reader does with a value other than its own. | Refuse with `-ENOTSUP` unless `rec_size` equals the build's R. It is a geometry field, like `peb_size`. |
| A6 | §3.5: scan on doubt; persist only if the scan out-ran the ceiling | Under today's A/B scheme, the next master write repairs a corrupt other sector. With the log, that sector can stay corrupt for up to 1 362 updates, and every mount until then scans every bucket. | A doubt mount always commits one record before it returns. If the doubt came from the other sector (corrupt, or a header with no verifying record) or from A1, the commit **rotates**. Otherwise it **appends**. Either way the next mount finds no doubt. |
| A7 | §4.5 Wedge: "one best-effort record append, with the state unchanged" | In the code the wedge happens inside the atomic window, where the stored state is `COMPACTING(bid)`. | Stating it outright: the wedge record repeats `COMPACTING(bid)`. A `CLEAN` record there would skip recovery. |
| A8 | §4.2: a record that marks the feature precedes the first segmented write | Every INDEXED or SEGMENT slot is already preceded by `persist_seg_owner(id ≠ 0)` (`blob_db.c` 2503, 2616, 2914, 3030), which commits a record. | The marking rides on that record, so marking `LARGE_PAYLOADS` costs nothing extra. Phase 3 needs a standalone `feature_mark()` for `DELTA`. |

## 2. PR sequence

| PR | Scope | Size (lib + tests) | Risk | Format |
|---|---|---|---|---|
| **1** | Refactor: one `master_commit()`, I/O wrappers | ~150 + 0 | low | unchanged (majors 1/2) |
| **2** | Major 5: header, log, rotation, mount search, doubt, geometry; fuse word carried; iostats split | ~650 + ~600 | **high** | **break** (5; 1–4 refused) |
| **3** | Feature sets and registry; `LARGE_PAYLOADS` marked on first use; slot flags from the store | ~200 + ~200 | medium | additive within 5 |
| **4** | Fuses: trip points, persistence rules, inspect and shell | ~300 + ~400 | low–medium | none (field from PR 2) |
| **5** | `l1_bucketlog.md` rewrite; DK re-measure in `RESULTS.md` | docs | low | — |

PR 2 must carry the fuse word, even though PR 4 is what trips fuses. The
proposal requires every build that can write a major-5 record to carry the
word forward (§4.5, first rule). A PR 2 build that wrote bytes 36..39 as
zeros would be exactly the build that destroys fuses.

PRs 2–4 should merge back to back, with nothing released between them. Then
the temporary measure in PR 2 (§4, last row) never reaches a board.

**Landing order against open PRs.** #89 changes `format_whole_store()` (one
line, `blob_db_store_discard()`), #86 changes only the UBI backend and
`ubi.c`, and #87 rewrites the slot walk where PR 3 and PR 4 hook in. Land all
three first; each conflict is then a few lines. #63 and #62 rebase onto this
work as phases 2 and 3 (§8).

## 3. PR 1 — one commit path (no format change)

Today eight call sites each run `write_master(inactive, gen + 1, …)` and then
flip `active_master` and `master_gen` themselves: `persist_next_id_hint`,
`compact_commit` (×2), `recover_compaction`, `persist_seg_owner`,
`erase_all_impl`, `format_masters_fresh` and `format_whole_store`. PR 2 has to
change every one of them. A refactor first keeps PR 2's diff to the format
itself.

`lib/blob_db/blob_db.c`
- `st` gains `uint8_t m_state; uint16_t m_cbid;`. Together with the existing
  `next_id_hint` and `seg_owner`, that is the whole mutable state a record
  will carry.
- `static int master_commit(void)` writes the state in `st` and, on success,
  advances the A/B pointer. Each caller sets the fields it changes, calls it,
  and restores the old values on failure, as `persist_seg_owner()` does today.
- `static int master_format(uint64_t next_id_hint)` covers the two format
  paths (sector A written, sector B erased, generation 1).
- I/O wrappers `io_read/io_write/io_erase/io_replace` replace the 33 direct
  `blob_db_store_*` calls outside the master code. They do nothing yet. PR 4
  makes them trip `IO_ERR`, and the master code keeps calling the store
  directly, so a failed record trips `MASTER_WRITE_FAIL` only (proposal row
  8 vs 9).

Tests: unchanged and green on every scenario in `testcase.yaml`. Doing
nothing visible is the point of this PR.

## 4. PR 2 — major 5

### 4.1 Layout (`blob_db_internal.h`)

```c
#define BLOB_DB_FORMAT_MAJOR      5      /* both builds: features replace 1/2 */
#define BLOB_DB_FORMAT_MINOR      0
#define BLOB_DB_MASTER_LOG_OFF    BLOB_DB_MASTER_HDR_MAX   /* 128 */

struct __packed blob_db_master_hdr {      /* 64 B; written once per erase */
	struct blob_db_compat_hdr compat;  /*  0..11 frozen                 */
	uint32_t rot_gen;                  /* 12..15                        */
	uint32_t peb_size;                 /* 16..19                        */
	uint16_t n_buckets;                /* 20..21                        */
	uint16_t rec_size;                 /* 22..23 R, write_align-rounded */
	uint32_t incompat;                 /* 24..27                        */
	uint32_t ro_compat;                /* 28..31                        */
	uint32_t compat;                   /* 32..35                        */
	uint8_t  suite;                    /* 36    0 = unprotected         */
	uint8_t  reserved0[3];             /* 37..39                        */
	uint8_t  store_uuid[16];           /* 40..55 0 unless protected     */
	uint32_t reserved1;                /* 56..59                        */
	uint32_t hdr_crc32;                /* 60..63 over [0, 60)           */
};

struct __packed blob_db_master_rec {      /* 48 B; one per update */
	uint32_t rec_no;                   /*  0..3  == slot index          */
	uint8_t  state;                    /*  4                            */
	uint8_t  reserved0;                /*  5                            */
	uint16_t compacting_bid;           /*  6..7                         */
	uint64_t next_id_hint;             /*  8..15                        */
	uint64_t seq_hint;                 /* 16..23 0 until phase 2        */
	uint64_t seg_owner;                /* 24..31                        */
	uint32_t incompat;                 /* 32..35                        */
	uint32_t fuses;                    /* 36..39 FROZEN for major 5     */
	uint32_t reserved1;                /* 40..43                        */
	uint32_t crc32;                    /* 44..47 over [0, 44)           */
};
```

The proposal's offsets are fixed with `BUILD_ASSERT(offsetof(...))`, one per
field. The assert on `fuses` carries a comment that moving it is a new major.

### 4.2 RAM (`st`)

| Today | Becomes |
|---|---|
| `active_master`, `master_gen` | `active_master`, `rot_gen` |
| — | `uint16_t log_next` (next free slot), `uint16_t log_cap` ((peb − 128) / R), `bool log_closed` (A2) |
| — | `uint32_t incompat`, `uint32_t fuses`, `uint32_t fuses_new` |

That is 16 B, within M9. `fuses_new` holds bits tripped but not yet on
flash; "pending" means `fuses_new != 0`.

### 4.3 Commit, append, rotate

```c
static int master_commit(void)                 /* from PR 1, now a log */
{
	if (!st.log_closed && st.log_next < st.log_cap) {
		int rc = log_append();
		if (rc == 0) {
			goto committed;
		}
		st.log_closed = true;                  /* A2: never append past a hole */
		fuse_trip(BLOB_DB_FUSE_MASTER_WRITE_FAIL);
	}
	int rc = rotate();                             /* full, closed, or append failed */
	if (rc < 0) {
		fuse_trip(BLOB_DB_FUSE_MASTER_WRITE_FAIL);
		return rc;
	}
committed:
	st.fuses |= st.fuses_new;
	st.fuses_new = 0;
	return 0;
}
```

- `log_append()` fills a record from `st` with
  `fuses = st.fuses | st.fuses_new`, the CRC, and padding to R with 0xff. It
  writes it with `blob_db_store_write()` at `128 + log_next × R` and
  increments `log_next` whatever the outcome.
- `rotate()` stages the header and record[0] in one 0xff-filled buffer of
  128 + R bytes, at most 256 B on the stack, twice today's 128 B. It writes
  them with `blob_db_store_replace(other)`. On success it sets
  `active_master = other`, `rot_gen = max + 1` (A3), `log_next = 1` and
  `log_closed = false`. The header's `incompat` is `st.incompat`, which by
  then already holds every bit the old header and records carried.
- `write_align > 128` is refused at open, as `write_master()` refuses it
  today.

### 4.4 Mount

`read_master()` splits in two:

- `master_classify(slot, &hdr, &cls)` reads only [0, 128). The
  ERASED/CORRUPT/FOREIGN/OK rules stay as they are, with these additions:
  major ≠ 5 is FOREIGN (D10); `peb_size`, `n_buckets` or `rec_size` (A5)
  different from the device's is FOREIGN; feature bits are checked in PR 3.
- `log_load(slot, &rec, &tail_bad)`:
  1. Binary search for the first slot whose R bytes all read 0xff, in
     [1, log_cap]. Record[0] is written together with the header, so slot 0
     is never the end. That is about log₂(cap) reads of R bytes each.
  2. Walk back from that slot to the first record whose CRC verifies and
     whose `rec_no` equals its slot index. `tail_bad` says whether the slot
     just before the end failed.
  3. Return `-ENOENT` if no record verifies.

`mount_impl()` is restructured around them:

```
classify A, B
FOREIGN on either                       -> -ENOTSUP, write nothing      (as today)
neither OK                              -> virgin / autoformat / -EIO   (as today)
order the OK sectors by rot_gen, newest first
for each: log_load(); stop at the first that yields a record
    a sector passed over                -> doubt, fallback
none yields a record                    -> as "neither OK"
doubt |= tail_bad
       | other is CORRUPT
       | other is ERASED and active is full        (A1)
       | found_damage on either master             (as today)
adopt the record into st; log_next = end; incompat = ∪ both headers ∪ record (A4)
if COMPACTING: recover_compaction()      # ends with master_commit(CLEAN)
if doubt: scan buckets, raise the ceiling, then
          force_rotate if the doubt came from the other sector or A1, else append  (A6)
```

`recover_compaction()` and the doubt step can each commit once, so a mount
writes at most two records. With `force_rotate`, the doubt commit is the
rotation and the recovery commit is an append, so a mount still erases at
most one sector.

### 4.5 Call sites

The code paths themselves do not change; each now writes one record where it
used to write a whole sector (proposal §5.1, §6). `erase_all_impl()` commits
`next_id_hint = 2` like any other update. A full-copy record makes lowering
the ceiling as safe as it is today. `format_whole_store()` and
`format_masters_fresh()` call `master_format()`, which erases both master
sectors and writes the header plus record[0] to A with `fuses = 0`. PR 4 adds
an argument for `AUTOFORMATTED`.

**Temporary, until PR 3:** a `LARGE_PAYLOADS` build sets `incompat` bit 0 at
format. This does the job of today's major 2: a build without the feature
refuses the store. PR 3 replaces it with marking on first use.

### 4.6 iostats

`master_writes` is documented today as "one block erase each". It splits:

| Field | Counts |
|---|---|
| `master_records` | every record programmed, record[0] included |
| `master_rotations` | header writes (each erases a master sector) |

The identity in `test_iostats_causes_account_for_erases` and in
`app_perf_blob_db`'s `why` lines becomes
`erases == master_rotations + bucket_formats + k × compactions`.
`app_perf_blob_db/src` prints both fields.

### 4.7 Tests

`tests/lib/blob_db/src/main.c`. The raw forging helpers stay a private copy
of the layout, on purpose (the comment at line 455 explains why):

- `struct master_hdr_img` (64 B) and `struct master_rec_img` (48 B), with
  offset asserts;
- `master_hdr_read_raw()` and `master_hdr_write_raw()` (erase the sector,
  then write the header and a record[0]);
- `master_rec_append_raw(slot, rec)` (finds the end the way mount does, then
  writes) and `master_rec_corrupt_raw(slot, i)`.

| Existing case | Change |
|---|---|
| `test_mid_compaction_crash_recovery` | append a `COMPACTING(42)` record instead of writing a higher-gen B. Then assert that recovery appended a `CLEAN` record directly after it, in the same sector, with no new erase |
| `test_foreign_major_*`, `test_split_major_*`, `test_format_discards_*`, `test_future_minor_*` | forge headers with major 6 or minor 1; the logic is unchanged |
| `test_corrupt_prefix_falls_back_to_the_good_master`, `test_both_masters_corrupt_*`, `test_corrupt_master_beside_blank_*` | need two ok sectors, so force a rotation first (helper below) |
| `test_fallback_past_corrupt_master_raises_ceiling` | becomes two cases: the newest record rots (doubt from `tail_bad`), and the newest header rots (fallback) |
| `test_fallback_past_blank_master_raises_ceiling` | becomes **A1's regression test**: full X, rotation torn after Y's erase, mount must scan |
| `ubi.c: test_rolled_back_master_still_raises_ceiling` | keep; UBI stands an older LEB copy back up only across a replace, which is now a rotation |

New cases. Each item of proposal §11 maps to a case here or in PRs 3–4.

1. `force_rotation()` helper: allocate ids until `blob_db_inspect` reports a
   new `rot_gen` (82 records × 256 ids on native_sim's 4 KB sectors, which
   takes milliseconds).
2. **Log round trip:** iostats show 0 master erases for `lg pwrite`-style
   segmented writes and for compaction on the atomic path (§11.1).
3. **Log end:** a torn last record; a rotten record in the middle; rotten
   last record → scan; corrupt other sector → scan, then rotate (A6), and the
   second mount does not scan (§11.3).
4. **Rotation:** fill, rotate, remount. The state survives, `rot_gen` is
   monotonic (A3), and a fallback followed by a rotation never reuses a
   rot_gen (§11.4).
5. **A2's regression test:** a flash-simulator callback fails one write
   without programming a byte. The next commit rotates, and the remount sees
   the newest ceiling.
6. **Geometry:** forge a header with another `n_buckets`, `peb_size` and
   `rec_size`. Each is refused with `-ENOTSUP`, and the partition is
   byte-for-byte unchanged (§11.7, unprotected half).

`tests/lib/blob_db/src/power_cut.c` is already offset-agnostic. It gains a
case that cuts at every byte and every erase of an `alloc_id` that persists a
ceiling across a rotation. After each cut it remounts and asserts that the
next id is above every id handed out before the cut, including
allocated-but-unbound ones (§11.2).

**Test-only Kconfig.** `CONFIG_BLOB_DB_TEST_MASTER_LOG_CAP` (int, 0 means
the sector's capacity) caps `log_cap`. A new scenario
`lib.blob_db.master_log_tiny`, with cap 3, on flash_area and on UBI with
`ATOMIC_REPLACE=n`, then runs the *whole* suite, `power_cut.c` included, with
a rotation every three commits. That puts every existing crash and recovery
case through rotation without writing them twice.

## 5. PR 3 — features

`blob_db_internal.h`
```c
/* incompat bits. One table; a slot flag has exactly one owner. */
#define BLOB_DB_FEAT_LARGE_PAYLOADS  BIT(0)   /* owns slot flags 2, 3 */
#define BLOB_DB_FEAT_PROTECTED       BIT(1)   /* phase 2: flag 4      */
#define BLOB_DB_FEAT_DELTA           BIT(2)   /* phase 3: flag 5      */

#define BLOB_DB_FEAT_FLAGS_LARGE_PAYLOADS  (BLOB_DB_SLOT_F_INDEXED | BLOB_DB_SLOT_F_SEGMENT)
#define BLOB_DB_SLOT_F_BASE                (BLOB_DB_SLOT_F_SEALED | BLOB_DB_SLOT_F_TOMBSTONE)

#define BLOB_DB_BUILD_INCOMPAT   (IF_ENABLED(CONFIG_BLOB_DB_LARGE_PAYLOADS, (BLOB_DB_FEAT_LARGE_PAYLOADS |)) 0)
#define BLOB_DB_BUILD_RO_COMPAT  0
#define BLOB_DB_BUILD_REQUIRED   0                         /* phase 2: PROTECTED */
#define BLOB_DB_BUILD_SUITE      0
BUILD_ASSERT((BLOB_DB_FEAT_FLAGS_LARGE_PAYLOADS & BLOB_DB_SLOT_F_BASE) == 0, ...);
/* one assert per pair of features as they are added */
```

`blob_db.c`
- `features_acceptable()` runs the five checks of proposal §4.4. It is called
  from `master_classify()` on header bits and again in mount on the union
  (A4), always before the first write.
- `st.slot_known = BASE | (st.incompat & LARGE_PAYLOADS ? FLAGS_LP : 0)`,
  recomputed whenever `st.incompat` changes. It replaces
  `BLOB_DB_SLOT_F_KNOWN` in `slot_walk_at()`, `scan_bucket()`,
  `scan_bucket_for()` and `blob_db_core_slot_at()`. The compile-time mask is
  removed.
- `persist_seg_owner(id ≠ 0)` ORs in `LARGE_PAYLOADS` before it commits
  (A8). It must commit even when `seg_owner == id` already holds and the bit
  is missing; that case cannot happen today, but the code says so rather than
  relying on it.
- PR 2's temporary bit at format is removed.

Tests (§11.5 and §11.9):
- A store formatted by an LP build and never segmented mounts in a non-LP
  build. After one large update it is refused with `-ENOTSUP`, with nothing
  written. This needs both builds, so it is a two-image test: one scenario
  writes `storage.bin` with `flash_simulator` file backing, and a second
  scenario mounts that image. Alternatively, forge `incompat` on flash_area
  in one build, which is what main.c's style suggests and is cheaper. This
  plan takes the forging route, plus one cross-build check in CI if the
  maintainers want it.
- Unknown `incompat`, unknown `ro_compat`, and a bit present only in the
  newer sector's header with mount falling back to the older one (A4): each
  refused, nothing written.
- A cut at every byte of the marking record leaves the bit absent and no
  INDEXED or SEGMENT slot on flash (proposal §7 row 4). This reuses the
  `BLOB_DB_CUT_AFTER_OWNER_SET` hook.

## 6. PR 4 — fuses

`blob_db_internal.h`: `BLOB_DB_FUSE_*` for bits 0..15 as in proposal §4.5,
`BLOB_DB_FUSE_DEBUG(n)` as `BIT(24 + (n))`, and a `BUILD_ASSERT` that the
assigned bits are distinct, by summing popcounts.

`blob_db.c`
```c
static void fuse_trip(uint32_t bit)
{
	if (!(st.fuses & bit) && !(st.fuses_new & bit)) {
		st.fuses_new |= bit;
		blob_db_maint_kick();          /* pending work for the helper */
	}
}
```

Trip points, against today's code:

| Fuse | Where |
|---|---|
| `SLOT_ROT` / `SLOT_TORN` | `slot_walk_at()` with `intact == false`, split on whether an erased header follows; `slot_verify()` returning `-EBADMSG`; `cursor_past_torn_tail()` → `SLOT_TORN` |
| `BUCKET_HDR_ROT` | `bucket_hdr_valid()` / `scan_bucket()` / `read_bucket_hdr()`: magic matches but the CRC or the bid does not. Not an erased or invalidated (zeroed magic) header |
| `MASTER_HDR_ROT` | `master_classify()` → CORRUPT, on either sector |
| `MASTER_REC_ROT` / `_TORN` | `log_load()`: a failing record older than the newest / the newest written |
| `STORE_DAMAGE` | mount, when `blob_db_store_open_found_damage()` is true for either master |
| `IO_ERR` | the PR 1 wrappers |
| `MASTER_WRITE_FAIL` | `master_commit()` (§4.3) |
| `WEDGED` | `COMPACT_OR_WEDGE`, followed by one `(void)master_commit()` with the state left at `COMPACTING(bid)` (A7) |
| `MASTER_DOUBT`, `ROTATION_FALLBACK`, `COMPACT_RECOVERED`, `SEG_SWEEP` | the corresponding branches of `mount_impl()` |
| `AUTOFORMATTED` | `master_format(…, fuses)`: mount's autoformat branch passes the bit, `blob_db_format()` passes 0 |

Persisting (proposal §4.5): mount commits once before returning if
`fuses_new != 0` and it has not committed already (A6's commit counts).
`blob_db_maintain()` treats pending fuses as one step of work: it reports
`more` and commits one record. Nothing else changes: other paths fold
`fuses_new` into the next record they write anyway. Reads never write.

Inspect and shell:
- `struct blob_db_inspect_info` (public) loses `master_gen` and gains
  `rot_gen`, `log_used`, `log_cap`, `incompat`, `ro_compat`, `compat`,
  `suite`, `fuses` (the word, raw) and `fuses_pending`.
  `struct blob_db_core_state` gains the same.
- `blob_db_shell.c` `info` prints the word with names for bits 0..15 and
  `debug(n)` for 24..31. The names table lives in the shell only.

Tests (§11.10):
- **One case per trip point** by fault injection on flash_area: flip a bit
  in a slot in the middle of a bucket, cut the tail, rot a bucket header, and
  so on. Each fuse survives a remount, a rotation (cap 3 scenario) and a
  compaction.
- **Unknown bits pass through:** forge a record with
  `fuses = 0xffff0000`, then drive updates, rotations, compaction,
  `COMPACTING` recovery, a wedge (`blob_db_test_wedge()`) and a fallback. The
  word read back still has every planted bit. `blob_db_format()` clears it.
  An autoformat leaves only `AUTOFORMATTED`.
- **Write-free reads:** a `get` that trips `SLOT_ROT` changes no
  write or erase counter in iostats. `blob_db_maintain()` then writes one
  record, and a second call writes none.
- **Mount:** a mount that trips commits once. A refused mount writes nothing,
  checked byte for byte on the partition.
- **Not interpreted:** `CONFIG_BLOB_DB_TEST_FUSES_AT_FORMAT` (hex, default
  0) is the word `master_format()` writes. Scenario
  `lib.blob_db.fuses_all_set` sets it to `0xffffffff` and runs the whole
  suite. One case in the default scenario runs a fixed workload on two
  stores, fuses 0 and fuses all set (forged), and asserts identical iostats.

## 7. PR 5 — documentation and measurement

- `doc/impl/l1_bucketlog.md`: §3.1 (new layout), §5.1 (mount, with the A1
  and A6 rules), §5.6 and §5.6a (records instead of master writes), §6 (crash
  tables: add rows for a record append and for a rotation), §13.1 (ceiling
  persistence). §5.7a covers the new inspect fields and the fuse printout.
- `doc/layers/l1_blob_db.md` §5: major 5 and the feature sets replace the
  major-per-combination text.
- `app_perf_blob_db/RESULTS.md`: re-run `lg pwrite` and churn on the
  nRF5340-DK and compare with proposal §9. The claims to confirm are 0 master
  erases per `lg pwrite` op and 24 erases for churn on flash_area. Mount time
  is new: about 11 reads of 48 B on 64 KB sectors.
- Mark the proposal **accepted**, with A1–A8 folded into its §3.3–§3.5 and
  §4.4.

Docs that describe a PR's behaviour should land in that PR. PR 5 is for what
spans PRs and for the board run.

## 8. Phases 2 and 3 (rebases)

**PR #63 (protection) onto major 5**
- The master MAC and tail move to header bytes 64..83 (`hdr_len = 84`) and
  record bytes 44..63 (`R = 64`). The record MAC binds `LE32(rot_gen)`.
  `log_load()` checks in this order: erased, CRC, MAC.
- `suite`, plus `PROTECTED` in `BLOB_DB_BUILD_REQUIRED`, `incompat` bit 1 set
  at format, and `COMPACTED` moved to slot flag bit 4 under that feature.
- `cbase` becomes 8 random bytes per compaction (`psa_generate_random`) with
  the top bit set, and the mount salt's top bit is cleared.
  `seq_reserve()` calls leave `compact_commit()`.
- `MAC_FAIL` trips where the CRC verifies and the MAC does not.
- Geometry is under the MAC, which closes PR #63 §9 item 4 on UBI. Add the
  forged `leb_count` case there.

**PR #62 (`DELTA`) onto major 5:** slot flag bit 5, `incompat` bit 2, and a
standalone `feature_mark(BLOB_DB_FEAT_DELTA)` before the first delta slot.
That function commits one record unless the bit is already in `st.incompat`.
Its `BUILD_ASSERT` pairs it against `LARGE_PAYLOADS` and `PROTECTED`.

## 9. Risks

| Risk | Mitigation |
|---|---|
| A slot that reads all 0xff after an interrupted program, on flash with ECC or with weakly programmed cells. The binary search takes it as the end; the next append then programs over it, and the write may fail or read back unstable | On flash with ECC the append fails, and A2 rotates. On NOR, a slot whose programmed bytes were all 0xff is erased in fact. In the remaining case of weak bits, a CRC failure on the next mount means doubt and a scan. No ceiling is lost: the torn record was never acknowledged |
| UBI write ordering (`ubi_leb_write_at`: "writes to one LEB go in rising order, each at or past the end of the previous one") | Appends only move forward, a failed slot is never rewritten (A2), and rotation is `ubi_leb_change()`. The `lib.blob_db.ubi*` scenarios run with `UBI_SELF_CHECKS=y` |
| Mount cost on very slow reads | log₂(1 362) ≈ 11 reads of 48 B, against two today. Measured in PR 5 |
| PR 2's size | PR 1 takes the call-site churn. The cap-3 scenario reuses the whole existing crash suite. Review §4.4 against A1–A6 line by line |
| Development boards holding majors 1–4 | Refused with `-ENOTSUP`, nothing written (D10). The log message names `blob_db_format()`, as it does today |

## 10. Open questions for the maintainer

1. **A1–A8.** Agree, then fold them into the proposal before PR 2.
2. **Cross-build feature test (PR 3):** forge-only, or also a two-image CI
   check?
3. **Should inspection trip fuses?** `blob_db_core_slot_at()` shares
   `slot_walk_at()`, so a `blob_db bucket` shell command that meets rot
   would trip `SLOT_ROT`. This plan says yes, because it is evidence like any
   other, and reads stay write-free either way.
