# Implementation plan — the master as a log, and format features

2026-11-01 · plan for `2026-10-28-master-log.md` as revised in #99 (cited
below as "the proposal")
· written against `main` at `e00702c`
· touches `lib/blob_db/`, `include/app/lib/blob_db_{inspect,iostats}.h`,
`tests/lib/blob_db/`, `app_perf_blob_db/`, `doc/impl/l1_bucketlog.md`

---

## 0. Scope

This plan covers phase 1 of the proposal: major 5, the log, rotation, mount,
geometry, recovery keyed on the scratch seal (D17), the foreign-slot rules
(D13), features set at format with `LARGE_PAYLOADS`, ceilings carried by
every record (D14), and the fuse field carried forward with no trip points.
Phases 2 and 3 (PR #63, PR #62) and the later fuse-trip phase appear in §7,
listed only by what each needs from phase 1.

Five points in the proposal need an amendment before code (§1). B1 and B2
are correctness problems: B1 can issue an id twice, and B2 can leave a
bucket that refuses every write after one power cut. B3–B5 are gaps the
proposal leaves open, and the code has to pick an answer for each. Every
amendment says which text in the proposal it changes. All five are now folded
into the proposal: B1 as D18, B2 as D13's compaction guard, B3 as D19, B4 as
D20, and B5 as §3.3's `erase_all` exception.

## 1. Amendments to the proposal

### B1 — Format writes both master sectors; any other sector that is not ok means doubt

**Problem.** §3.5 doubts when the other sector is "neither erased nor ok". It
does not doubt when the other sector is erased. That leaves the following
path on flash_area, and on UBI with `ATOMIC_REPLACE=n`:

1. Sector Y is active. Its header rots.
2. Mount: X is the only ok sector, so `hi = X`, it is usable, and
   `fell_back = no`. Y is corrupt, so mount doubts, scans, and raises
   `next_id`, in RAM only (§3.5 step 2).
3. Ending the doubt rotates into Y. Power is cut after Y's erase.
4. Next mount: X is ok, Y is erased. None of the doubt conditions hold, so
   mount takes X's ceiling. That ceiling is older than the ceilings Y carried,
   so ids bound under Y's ceilings are issued again.

§7 row 2 already says "the other sector erased or corrupt → doubt", which
contradicts §3.5. Tightening the rule to "erased beside a full active
sector" is not enough, because a sector closed by a failed write (§3.3) is
not full.

**Change.** Formats write both master sectors, so after a completed format
no master sector is erased again except inside a rotation:

- `blob_db_format()`, the virgin format at mount and an autoformat erase
  both master sectors. They write B with `rot_gen = 1`, then A with
  `rot_gen = 2`, each with a header and a record[0] for the same state.
- §3.5: `doubt = … or the other sector is not ok` (erased, corrupt or
  foreign). This is today's rule (`ca != MASTER_OK || cb != MASTER_OK`)
  carried over.
- Ending the doubt: "the other sector is not ok" is a reason to rotate,
  alongside `fell_back` and damage.
- A cut between the two format writes leaves A erased and B ok. Mount
  doubts, and the scan is cheap because format invalidates every bucket
  first. Mount then rotates into A. A cut inside B's write leaves no usable
  master and an erased A: `-EIO`, as a torn first master gives today
  (proposal test 3b).
- Cost: one sector write per format. Formats erase both master sectors
  already. Steady state is unchanged.

**Proposal text.** §3.5 (the doubt rule and how the doubt ends), §4.2 (format
writes both headers; the invariant is then "every ok sector carries every
bit" with no erased sector beside an ok one), §7 rows 1–2, and test 3a (add
"an erased other sector").

### B2 — The compaction guard covers only bytes beyond one slot span

**Problem.** §4.2 makes compaction refuse (`-EIO`) whenever the old bucket
holds programmed bytes past the point where its walk stopped. The argument
that nothing can follow a torn slot assumes bytes are programmed in order. A
QSPI NOR page program (256 B, the nRF5340-DK's MX25R) programs the whole page
at once, so a cut can leave programmed bits anywhere in the page, beyond a
header torn too badly for the walk to size it. Every later write to that
bucket compacts first (`confirm_cursor()`, #98), and every compaction
refuses. One power cut leaves the bucket refusing writes until a format,
which breaks P7.

**Change.** A single append writes at most
`span = slot_size_for(CONFIG_BLOB_DB_MAX_PAYLOAD_LEN)` bytes from where the
walk stopped. Compaction refuses only if a programmed byte lies at or beyond
`stop + span`. Programmed bytes inside that span are what one torn append
leaves, and they are dropped as today. With B3 in place, the guard exists to
catch data whose origin is unknown. The two known origins have their own
answers: a foreign slot (D13), and a different payload cap (B3).

**Proposal text.** The last two sentences of §4.2 and test 5a. Add: "a torn
append that programmed bytes anywhere in its span, out of order, still
compacts".

### B3 — The maximum payload length and `write_align` become geometry

**Problem.** The format depends on two values the header does not record:

- `slot_walk_at()` (`blob_db.c:911`) ends the log at
  `val_len > CONFIG_BLOB_DB_MAX_PAYLOAD_LEN`. A build with a smaller cap than
  the build that wrote the store therefore reads a sealed, CRC-intact slot as
  a torn tail. D13 does not catch it, because it requires a plausible
  `val_len`. Today the next compaction then drops every slot after it. Under
  the proposal, the B2 guard makes the bucket refuse writes.
- Slot padding (`slot_size_for()`) rounds up to `write_align`, but
  `rec_size` does not capture it, because 48 is a multiple of every
  `write_align` from 1 to 16. A store written with one `write_align` and
  walked with another parses slots at the wrong boundaries.

**Change.** Header bytes 56..59, reserved until now:

| Bytes | Field | Check at mount (every ok header, §4.4) |
|---|---|---|
| 56..57 | `max_payload_len` | must equal `CONFIG_BLOB_DB_MAX_PAYLOAD_LEN` |
| 58..59 | `write_align` | must equal the device's |

A mismatch is refused with `-ENOTSUP`, and nothing is written, the same as
for `rec_size`. Requiring equality, rather than "the store's cap is at most
the build's", keeps reading and writing symmetric. A build with a larger cap
would write slots that the store's original build could no longer walk.
Raising the cap therefore means a format, which is the same rule M8 applies
to every other geometry value.

**Proposal text.** The §3.1 table, M8, §4.4 (two more lines), and test 5b
(refusals).

### B4 — A store without a feature is written without it

**Problem.** §4.4 lets a `LARGE_PAYLOADS` build mount a store formatted
without the feature (`store.incompat ⊆ build.known`). D13 makes the walk
accept the build's flags. Nothing then stops that build from writing INDEXED
and SEGMENT slots into a store whose headers lack bit 0, which breaks the §4.2
invariant ("every ok sector carries every feature bit the store has"). A
build without the feature would then mount the store and hit foreign slots.

**Change.** The store's bits decide what a build writes. The Kconfig option
only decides what the build can read. A `LARGE_PAYLOADS` build on a store
without bit 0 behaves like a build without the feature: `update()` and
`write()` beyond `CONFIG_BLOB_DB_MAX_PAYLOAD_LEN` return `-EINVAL`, as they do
there today. Reads are unchanged, since the store holds no segments. An
upgrade (open item 7) is the only way to gain the feature. The rule applies
to every slot-flag feature (`DELTA` in phase 3).

Refusing the mount instead would be simpler, but it reverses the proposal's
principle that "the store says which features it uses, and a build follows".

**Proposal text.** §4.2 (a paragraph after "Set at format") and test 5 (an
LP build on a non-LP store writes no segment; large writes return
`-EINVAL`).

### B5 — `erase_all` is the one record that lowers the ceiling

**Problem.** D14 and §3.3 say that every record raises the ceilings with
`max(...)`, "so no record ever publishes a ceiling below one already on
flash". `blob_db_erase_all()` (`erase_all_impl()`) resets the id space and
commits `next_id_hint = 2`, which is exactly such a record. The proposal does
not mention it.

**Change.** `erase_all`'s record sets `next_id_hint = 2 + 1 + STEP`
directly, without the `max`. It is written only after every bucket is
invalidated and the root is bound again, as today. This is safe for the
reason today's comment gives: no id at or above 2 survives on flash except
the root. A cut before the record lands leaves the old, higher ceiling,
which is safe. A torn record means doubt, and the scan only raises the
ceiling. `seq_hint` is never lowered: nonces under one key must never repeat,
and `erase_all` does not change the key.

**Proposal text.** §3.3 (an exception after the `max` rule), D14, and test
2a (an `erase_all` followed by a cut at every byte of its record).

### Earlier amendments, after #99

The first version of this plan raised A1–A8 against the proposal before
#99. Where each ended up:

| | Outcome |
|---|---|
| A1 (torn rotation hides lost ceilings) | **still open; B1 replaces it**, with a fix that also covers closed sectors |
| A2 (failed append leaves a gap) | adopted: D3, §3.2a, §3.3 |
| A3 (`rot_gen = max + 1`) | dropped. With two sectors the rotation target is always the sector being replaced |
| A4 (check features on every ok header) | adopted: §4.4 |
| A5 (`rec_size` must match) | adopted: §3.1 |
| A6 (a doubtful mount ends its doubt) | adopted: §3.5 |
| A7 (the wedge record keeps `COMPACTING`) | dropped. D17 removes compaction state from the master |
| A8 (mark the feature on the `seg_owner` record) | replaced by D7 (set at format) |

## 2. PR sequence

| PR | Scope | Size (lib + tests) | Risk | Format |
|---|---|---|---|---|
| **1** | Refactor: one `master_commit()`, I/O wrappers | ~150 + 0 | low | unchanged |
| **2** | Recovery keyed on the seal, on today's format (D17, first half) | ~80 + ~150 | low | unchanged |
| **3** | Foreign slots and the compaction guard (D13 + B2) | ~150 + ~200 | medium | unchanged |
| **4** | Major 5 (all of §5) | ~700 + ~700 | **high** | **break**: 5; 1–4 refused |
| **5** | `l1_bucketlog.md`, `kvdb_book` tools, DK re-measure | docs | low | — |

PRs 2 and 3 are taken out of the format break on purpose. Neither depends on
the master layout, and each fixes something the current code gets wrong:

- PR 2 fixes a rotten newest master hiding `COMPACTING` (proposal §6.1, last
  paragraph).
- PR 3 fixes a store written with flags this build does not know being
  compacted down to its torn-tail reading.

Both still work on majors 1/2. PR 4 can then remove the compaction master
writes without changing the recovery logic in the same diff.

**Landing order against open PRs.** Land #89 (`blob_db_store_discard()` in
`format_whole_store()`), #86 (UBI backend only) and #87 (the slot walk, which
PR 3 changes) first; each conflict is then a few lines. #63 and #62 rebase
onto PR 4 as phases 2 and 3.

## 3. PR 1 — one commit path (no format change)

Eight sites each call `write_master(inactive, gen + 1, …)` and then flip
`active_master` and `master_gen` themselves: `persist_next_id_hint`,
`compact_commit` (×2), `recover_compaction`, `persist_seg_owner`,
`erase_all_impl`, `format_masters_fresh` and `format_whole_store`.

`lib/blob_db/blob_db.c`
- `static int master_commit(void)` writes the state held in `st`
  (`next_id_hint`, `seg_owner`, and for now the compaction state). On
  success it advances the A/B pointer. Each caller sets the fields it
  changes, calls it, and restores the old values on failure, as
  `persist_seg_owner()` already does.
- `static int master_format(void)` covers both format paths, so B1's
  second write lands in one place in PR 4.
- `io_read/io_write/io_erase/io_replace` replace the direct `blob_db_store_*`
  calls outside the master code. They do nothing yet; the fuse-trip phase
  hooks `IO_ERR` here.

Tests: unchanged and green on every scenario.

## 4. PR 2 and PR 3 — behaviour on today's format

### 4.1 PR 2: recovery keyed on the seal

- `mount_impl()` reads the scratch seal at every mount, after the master is
  adopted and before the doubt scan. `recover_compaction()` takes the
  bucket from the image's own header, which its CRC covers. It checks
  `bucket_id < n_buckets`, and checks that the bucket header is valid as
  today. It no longer needs the master's `compacting_bid`.
- The `if (m->state == COMPACTING)` branch goes. `compact_commit()` still
  writes `COMPACTING`/`CLEAN` around the window, so a build older than this
  PR still recovers a store that this build left mid-compaction. PR 4 removes
  those writes.
- `recover_compaction()` writes its `CLEAN` master only when the stored state
  is `COMPACTING`. A seal found beside a `CLEAN` master (the hole being
  closed) needs no master write.
- The check runs on every backend, the atomic UBI path included (§6.1:
  atomic replace is a build option the format does not record).
- Tests (§11.1a): convert `test_mid_compaction_crash_recovery` to a forged
  sealed scratch. Add "a sealed scratch beside a `CLEAN` master is restored",
  which fails on today's code. Add "a sealed scratch left by
  `ATOMIC_REPLACE=n` is restored by an atomic build", as a two-scenario case
  that shares one flash image, or by forging the seal on UBI. Run
  `power_cut.c` unchanged.

### 4.2 PR 3: foreign slots and the compaction guard

- `slot_walk_at()` and `scan_bucket_for()` tell **foreign** apart from the
  end of the log. A slot is foreign when it is sealed, has `val_len ≤ cap`,
  fits the sector, has a CRC that verifies, and carries a flag outside
  `BLOB_DB_SLOT_F_KNOWN`. The walk reports it through a new
  `bucket_scan.foreign`, beside `tail_torn`. The known flags stay fixed at
  compile time (D13).
- A foreign bucket refuses every mutation with `-ENOTSUP`
  (`update`/`write`/`delete` paths, before `confirm_cursor()`) and is never
  compacted. Every read of an id hashing to it returns `-EIO`.
  `iterate()`/`count()` return `-EIO` as soon as they reach it. The proposal
  does not say what they do; this plan picks `-EIO`, because a silently short
  count is the stale answer D13 exists to remove. The slot index forgets the
  bucket, as `scan_bucket()` already does on an unknown flag.
- Compaction guard (B2): `build_compacted_image()` returns the walk's stop
  offset. `compact_bucket()` reads `[stop + span, peb_size)` out of the
  resident `g_bbuf`, so no extra flash read, and refuses with `-EIO` if any
  byte there is not `0xff`.
- Tests (§11.5a, plus B2): a forged foreign slot in the middle of a bucket
  gives writes `-ENOTSUP`, reads `-EIO`, and no compaction or erase
  (iostats). A torn tail still compacts. A torn append with stray bits
  programmed anywhere in its span still compacts (B2). Programmed bytes
  beyond the span make compaction refuse.

## 5. PR 4 — major 5

### 5.1 Layout (`blob_db_internal.h`)

```c
#define BLOB_DB_FORMAT_MAJOR      5      /* every build; features replace 1/2 */
#define BLOB_DB_FORMAT_MINOR      0      /* fixed for major 5 (D15)            */

struct __packed blob_db_master_hdr {      /* 64 B unprotected */
	struct blob_db_compat_hdr compat;  /*  0..11 frozen                  */
	uint32_t rot_gen;                  /* 12..15                         */
	uint32_t peb_size;                 /* 16..19                         */
	uint16_t n_buckets;                /* 20..21                         */
	uint16_t rec_size;                 /* 22..23                         */
	uint32_t incompat;                 /* 24..27                         */
	uint32_t ro_compat;                /* 28..31                         */
	uint32_t compat;                   /* 32..35                         */
	uint8_t  suite;                    /* 36                             */
	uint8_t  reserved0[3];             /* 37..39                         */
	uint8_t  store_uuid[16];           /* 40..55                         */
	uint16_t max_payload_len;          /* 56..57  B3                     */
	uint16_t write_align;              /* 58..59  B3                     */
	uint32_t hdr_crc32;                /* 60..63 over [0, 60)            */
};

struct __packed blob_db_master_rec {      /* 48 B unprotected */
	uint32_t rec_no;                   /*  0..3  == slot index           */
	uint32_t reserved0;                /*  4..7  (D17: no compaction state) */
	uint64_t next_id_hint;             /*  8..15                         */
	uint64_t seq_hint;                 /* 16..23 0 until phase 2         */
	uint64_t seg_owner;                /* 24..31                         */
	uint32_t reserved1;                /* 32..35 features: header only   */
	uint32_t fuses;                    /* 36..39 FROZEN for major 5      */
	uint32_t reserved2;                /* 40..43                         */
	uint32_t crc32;                    /* 44..47 over LE32(rot_gen) ‖ [0,44) (D16) */
};
```

There is one `BUILD_ASSERT(offsetof(...))` per field. The assert on `fuses`
says that moving the field is a new major.

### 5.2 RAM (`st`)

| Today | Becomes |
|---|---|
| `active_master`, `master_gen` | `active_master`, `rot_gen` |
| — | `uint16_t log_next`, `uint16_t log_cap`, `bool log_failed` (§3.3) |
| — | `uint32_t incompat, ro_compat, compat` (the header's, copied at rotation), `uint32_t fuses` |

That is about 24 B, matching M9.

### 5.3 Commit, append, rotate (§3.3, §3.4, D14, B5)

```c
static int master_commit(void)
{
	struct blob_db_master_rec r;

	rec_fill(&r);             /* hint = max(st.hint, next + 1 + STEP), unless
	                           * st.ceiling_reset (B5); fuses = st.fuses */
	int rc = (st.log_failed || st.log_next == st.log_cap)
		 ? rotate(&r)
		 : log_append(&r);  /* log_next++ whatever the outcome;
		                     * on error log_failed = true */
	if (rc == 0) {
		st.next_id_hint = r.next_id_hint;   /* RAM after flash */
	}
	st.ceiling_reset = false;
	return rc;
}
```

- `rotate()` stages the header and record[0] in a buffer of 128 + R bytes,
  filled with `0xff`, at most 256 B on the stack. It writes them with
  `blob_db_store_replace(other)`. The header copies `st`'s feature sets
  verbatim and sets `rot_gen + 1`. A `rot_gen` that would wrap returns
  `-ENOSPC` (§3.0).
- `persist_next_id_hint()` keeps its trigger: commit only when
  `next_id ≥ next_id_hint`. Other commits carry the ceiling forward (D14). A
  run of segmented writes therefore needs no ceiling-only records.
- `compact_commit()` and `recover_compaction()` lose their master writes
  (D17, second half). The scratch-path window is erase scratch → image →
  seal → erase bucket → image → erase scratch. `st` loses `m_state` and
  `m_cbid`.
- `erase_all_impl()` sets `st.ceiling_reset` and `next_id = 2`, then commits
  (B5).

### 5.4 Format (B1)

```c
/* Both master sectors are already erased by the caller. Each write is a
 * header plus record[0] for the same state; the header's incompat holds the
 * build's slot-flag features (D7). */
static int master_format(void)
{
	int rc = write_hdr_and_rec0(BLOB_DB_MASTER_B_SECTOR, 1 /* rot_gen */);

	if (rc == 0) {
		rc = write_hdr_and_rec0(BLOB_DB_MASTER_A_SECTOR, 2);
	}
	return rc;   /* on success: active = A, log_next = 1 */
}
```

`format_masters_fresh()` (virgin) keeps its order: invalidate the buckets,
erase both masters, then `master_format()`. `format_whole_store()` erases
the store (or calls `blob_db_store_discard()` once #89 lands), then calls
`master_format()`. `fuses = 0` in both record[0]s (D12).

### 5.5 Mount (§3.5 with B1)

`read_master()` splits into `master_classify()` (the header window only) and
`log_load()`, which reads every slot of a sector (D3). Instead of 1 362
reads of 48 B, `log_load()` reads the sector into `g_bbuf` once. Mount owns
`g_bbuf` until the first bucket walk, and that is P3-neutral.
`log_load()` returns `end`, `newest`, and whether the slot below `end` failed
to verify.

```
classify A, B
FOREIGN on either                                       -> -ENOTSUP
any ok header: features (§4.4), suite, hdr_len, rec_size, peb_size, n_buckets,
               max_payload_len, write_align (B3)        -> -ENOTSUP if not acceptable
both ok, equal rot_gen                                  -> -EIO
no ok sector: both erased -> format (B1: both sectors); else no usable master
hi / lo, usable, fell_back                              (§3.5 unchanged)
doubt = tail_bad || fell_back || other not ok (B1) || found_damage
sealed scratch -> recover_compaction()                  (PR 2)
if doubt: scan, raise next_id in RAM
feature bit in one ok header only -> rotate (finish the upgrade)
if doubt and not ended by that rotation:
    other not ok || fell_back || found_damage -> rotate  (B1 adds "erased")
    else                                      -> append
```

Mount writes at most one rotation and one append, plus whatever recovery
writes to the scratch sector and the bucket.

### 5.6 Features (§4.2, D7, B4)

- `blob_db_internal.h`: the registry from §4.2 and
  `BLOB_DB_BUILD_{INCOMPAT,RO_COMPAT,REQUIRED,SUITE}`, with a
  `BUILD_ASSERT` that no two features own a slot flag.
- `master_format()` writes `BLOB_DB_BUILD_INCOMPAT`'s slot-flag features:
  `LARGE_PAYLOADS` when the build has it.
- B4: `static inline bool seg_enabled(void)` returns
  `IS_ENABLED(CONFIG_BLOB_DB_LARGE_PAYLOADS) && (st.incompat & FEAT_LP)`.
  Every write-side `#if defined(CONFIG_BLOB_DB_LARGE_PAYLOADS)` branch in
  `update_impl()` and `write_impl()` (about 8 sites) tests it, and otherwise
  falls through to the inline path and its `-EINVAL` above the cap. Read-side
  branches stay compile-time: a store without the bit holds no segments.
  `seg_geometry_init()` still runs, so a later upgrade needs no remount.
- No upgrade call in phase 1 (open item 7). Mount's "finish the upgrade"
  rotation is implemented and tested with forged headers, so a phase-1 build
  handles a store that a later build left halfway through an upgrade.

### 5.7 Fuse field (§4.5)

`rec_fill()` writes `st.fuses`. Mount sets `st.fuses` from the adopted record,
the lower sector's on a fallback. Formats write 0. Nothing trips, and no mask
of known bits exists.

### 5.8 iostats and inspect (public headers)

| Where | Change |
|---|---|
| `blob_db_iostats.h` | `master_writes` ("one block erase each") splits into `master_records` and `master_rotations` |
| `app_perf_blob_db/src/main.c:316` | the `why` line prints both. The identity becomes `erases = master_rotations + bucket_formats + k × compactions`, and k drops by the two master writes on the scratch path |
| `blob_db_inspect.h` | `master_gen` → `rot_gen`. Add the three feature sets, `suite`, `rec_size`, `max_payload_len`, `write_align`, `log_used`, `log_cap` and `fuses` (raw) |
| `blob_db_shell.c` | `blob_db info` prints the above, with no master state (D17) |

### 5.9 Tests

`tests/lib/blob_db/src/main.c`. Its forging helpers (line 455) stay private
copies of the layout, on purpose: `struct master_hdr_img` (64 B),
`struct master_rec_img` (48 B), offset asserts, `master_hdr_write_raw()`,
`master_rec_append_raw()` and `master_rec_corrupt_raw()`.

| Existing case | Change |
|---|---|
| `test_iostats_causes_account_for_erases` | its `master_writes >= 2 × compactions` becomes false by design (D17). Assert `master_rotations == 0` across compactions, and the new identity |
| `test_foreign_major_*`, `test_split_major_*`, `test_format_discards_*` | headers with major 6; the logic is unchanged |
| `test_future_minor_is_tolerated` | minor 1 on major 5 still mounts (D15: the minor is ignored), and the next rotation writes 0 |
| `test_corrupt_prefix_falls_back_*`, `test_both_masters_corrupt_*`, `test_corrupt_master_beside_blank_*` | both sectors are ok right after a format (B1), so no forced rotation is needed |
| `test_fallback_past_corrupt_master_raises_ceiling` | the newest record rots / the active header rots |
| `test_fallback_past_blank_master_raises_ceiling` | **B1's regression test**: the scenario in §1/B1, with the doubt-ending rotation cut after the erase. The next mount scans, and the id stays unique |
| `ubi.c: test_rolled_back_master_still_raises_ceiling` | keep. A stood-up older LEB copy is now a whole rotation older |

New cases cover the proposal's §11 (items 1–5b, 10) plus:

- **B3:** a header with another `max_payload_len` or `write_align` is
  refused. The partition is unchanged, byte for byte.
- **B4:** an LP build formats without the bit (forged header): large
  `update()`/`write()` return `-EINVAL`, and no INDEXED or SEGMENT slot is on
  flash.
- **B5:** `erase_all` and then a cut at every byte of its record. Each
  outcome is the old ceiling or 2 + 1 + STEP, never another value. After
  remount, no id equals a surviving one.
- **Golden images** (§11.5b): `tests/lib/blob_db/src/golden.c` holds the
  byte arrays written by hand from §3 and B3, and checks both parse and
  emit.

**Test-only Kconfig:** `CONFIG_BLOB_DB_TEST_MASTER_LOG_CAP` (0 means the
sector's capacity) caps `log_cap`. The new scenario `lib.blob_db.master_log_tiny`
(cap 3, flash_area and UBI with `ATOMIC_REPLACE=n`) runs the whole suite,
`power_cut.c` included, with a rotation every three commits. `power_cut.c`
gains an `alloc_id` that crosses a rotation, cut at every byte and erase
(§11.2). The scenario `lib.blob_db.fuses_all_set`, with
`CONFIG_BLOB_DB_TEST_FUSES_AT_FORMAT=0xffffffff`, runs the whole suite on a
store whose 32 bits are set (§11.10).

## 6. PR 5 — documentation and measurement

- `doc/impl/l1_bucketlog.md`: §1.5, §3.1, §5.1, §5.6, §5.6a, §6, §13.1 and
  §11 item 13, as the proposal's §10 lists, plus B1's format and B2's guard
  in §6.2.
- `doc/layers/l1_blob_db.md` §5: major 5 and the feature sets.
- `kvdb_book`: `tools/blob-db-bucket-log-explorer/REQUIREMENTS.md` and
  `tools/migration-paths/REQUIREMENTS.md`, as the proposal's §10 says.
- `app_perf_blob_db/RESULTS.md`: re-run `lg pwrite` and churn on the
  nRF5340-DK, and record mount time with the one-sector read.
- Mark the proposal **accepted**.

## 7. Later phases, as they touch phase 1

- **Fuse trips** (§4.5a, after open item 8): `fuse_trip()` at the PR 1
  wrappers, at `master_classify()`, `log_load()`, the foreign-slot detection
  of PR 3, mount's branches and `COMPACT_OR_WEDGE`. Then the mount, wedge
  and `blob_db_maintain()` persistence rules, and the `blob_db.h` docstring
  change that the proposal's §10 names.
- **PR #63 (phase 2):** header bytes 64..83 and record bytes 44..63 (R = 64),
  `suite`, and `PROTECTED` in `BUILD_REQUIRED`. The random 12-byte `cbase`
  base is used as base + offset (§6.2); `seq_reserve()` leaves compaction.
  B3's fields are under the header MAC, like the rest of the geometry.
- **PR #62 (phase 3):** slot flag bit 5, `incompat` bit 2 set at format, and
  B4's `seg_enabled()` pattern for `DELTA`.

## 8. Risks

| Risk | Mitigation |
|---|---|
| PR 4's size | PR 1 takes the call-site churn; PRs 2 and 3 take the behaviour changes. The cap-3 scenario runs the whole crash suite through rotations |
| An LP build becomes a non-LP writer on old dev stores (B4) | intended. Phase 1 refuses majors 1–4 anyway, so every dev store is formatted once (§12, "Landing phase 1"), and by an LP build if large payloads are wanted |
| `max_payload_len` equality (B3) makes raising the cap a format | the same rule as every other geometry value. The log message names the two values and `blob_db_format()` |
| The B2 span assumes a torn append never writes past its own span | an append programs only its own slot: `append_slot2()` writes `slot_size_for(val_len)` bytes, and `val_len ≤ cap` |
| Reading the master sector into `g_bbuf` at mount | `g_bbuf` is free until the first bucket read. `recover_compaction()` reloads it from scratch afterwards, as today |
