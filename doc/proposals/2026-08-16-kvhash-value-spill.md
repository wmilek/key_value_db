# Design change proposal — value spill in `kvhash`

Status: **proposal / for review** · 2026-08-16
· Target implementation: `lib/containers/kvhash/kvhash.c`
· Target contract: `doc/layers/l2_containers.md` §4.3
· Evidence: `app_cbor_persondb/RESULTS.md` §5b, `FINDINGS.md` K1–K11
· Sizing harness: `doc/proposals/kvhash-spill-sizing.py`
· Governed by `doc/principles.md`

**The ask.** `kvhash` packs every value into a bucket blob shared with every
other key that hashes there. Should a large value instead live in its own
blob, with the bucket entry holding only its id?

**Short answer.** Yes, behind Kconfig, default off. The change is **local to
`kvhash`** — no `map_ops`, `blob_db`, L3 or application change — needs **no
additional RAM**, and keeps today's **single-write commit**, so visible-state
atomicity is untouched. It converts the `-ENOSPC` cliff (K2) from a sizing
exercise into a non-issue (`app_cbor_persondb`: 16 shards → 1, fullest bucket
66 % → 19 % of the ceiling) and cuts write amplification by ~45 %. It costs one
extra flash read per spilled `get` — about +25 % on that app's headline
`check` — which §6 shows is more than repaid if it lands together with derived
bucket ids (K1/K11). One question is genuinely open and is why this is a
proposal rather than a patch: the orphaned-blob residue it makes routine
(§5), which cannot be reclaimed without a hook the Map shape does not have
(§7).

---

## 1. What `kvhash` does today

A bucket is one blob holding a packed pair list, and the value is inline
(`kvhash.c:15`):

```
bucket blob = ( [u16 klen] [u16 vlen] [key bytes] [val bytes] )*
```

`kvhash_set` builds the whole bucket image in `bkt_buf` and writes it back
with one `blob_db_update` (`kvhash.c:291-314`). Three consequences follow, and
all three are load-bearing for this proposal.

| # | Property | Where | Consequence |
|---|---|---|---|
| **P1** | An entry costs `4 + klen + vlen` in the bucket | `kvhash.c:291` | large values crowd the bucket, not the store |
| **P2** | A bucket must fit one payload; overflow is `-ENOSPC` | `kvhash.c:293` | the K2 cliff — fires while the medium is nearly empty |
| **P3** | Any `set` rewrites the **entire** bucket | `kvhash.c:314` | write amplification = bucket size / value size |
| **P4** | The commit is a single `update`, so a `set` is atomic with no intent blob | `kvhash.c:314` | why `kvhash` has no recovery machinery today |

### 1.1 What that costs in practice

`app_cbor_persondb` is the measured case: 10 000 person records averaging
363 B, keyed `pXXXXXXXX`, over 16 map shards of 511 buckets
(`DESIGN.md` §6.1). Re-running its own enumeration
(`doc/proposals/kvhash-spill-sizing.py`, which imports the app's record model
rather than restating it):

```
          maps  occupied  max bkt  % of cap  over
today       16      5699     2711       66%     0
```

- **1.75 persons share a bucket blob on average, up to 7** — so a read of one
  record fetches 660 B on average to return 363 B, and a write rewrites all
  of it.
- **Sixteen shards exist only because of P2.** The first sizing attempt used
  eight and hit `-ENOSPC` at person 9 232; the count is now obtained by
  enumerating the whole population offline, because there is no per-bucket
  occupancy query (K10) and the bucket count cannot change after create (K3).

## 2. The change

A value longer than `CONFIG_BLOB_CONTAINER_KVHASH_SPILL_MAX` is written to its
own blob; the bucket entry holds that blob's id instead of the bytes.

### 2.1 Entry format

The spilled entry reuses the existing shape with a flag in the high bit of
`klen` (keys are then bounded at 32 767 bytes, far above anything the stack
can carry in one payload):

```
inline entry   [u16 klen           ] [u16 vlen] [key bytes] [val bytes]
spilled entry  [u16 klen | 0x8000  ] [u16 vlen] [key bytes] [u64 val_id]
                                      ^^^^^^^^ logical value length, so
                                               `get` reports it without
                                               reading the value blob
```

Keeping `vlen` as the *logical* length is deliberate: a size probe
(`get(key, NULL, 0, &len)`) and the `-ENOMEM` short-buffer path both stay at
today's cost, one bucket read, with no dereference.

### 2.2 Read path

```
dir_load(root)                     unchanged
blob_db_get(bucket_id)             unchanged
*out_len = vlen                    from the entry, before any dereference
if vlen > out_sz:  return -ENOMEM  short-circuit — value blob never read
if entry is spilled:
        blob_db_get(val_id, out, out_sz, NULL)        <-- the added read
else:
        memcpy(out, &bkt_buf[val_off], vlen)          unchanged
```

The value is read **straight into the caller's buffer** — it is never staged
in `bkt_buf`.

Note the ordering is load-bearing, not incidental. `map_ops.get` requires that
`-ENOMEM` report the true length (`shape_map.h:73`), whereas the `blob_db`
contract specifies `out_len` only as the length *written* and does not promise
it on `-ENOMEM` (`blob_db.h:156-160`). Taking the length from the entry's
`vlen` and answering the short-buffer case before dereferencing means `kvhash`
never relies on that unspecified behaviour — and a size probe stays at one
bucket read, as it is today.

### 2.3 Write path

```
1. if new value spills:  vid = alloc_id(); blob_db_update(vid, val, vlen)
2. rewrite the bucket entry to reference vid;  blob_db_update(bucket_id, ...)   <-- COMMIT
3. if the superseded value was spilled:  blob_db_delete(old_vid)
```

Step 2 is the same single commit write as today (P4). Step 1 writes the
value **from the caller's pointer**, so it never transits `bkt_buf`.

A rebind where both old and new values spill can skip steps 1 and 3 entirely
and `blob_db_update(vid, ...)` in place — the id is exclusively owned by this
entry, so the bucket does not change at all. That is the case that removes
write amplification (§4.2), and it mirrors the model container's §3.4
in-place fast path.

### 2.4 Delete path

`del` tombstones the value blob after committing the bucket rewrite — same
order as step 2 then 3.

### 2.5 RAM: unchanged

`kvhash` keeps exactly its two `MAX_PAYLOAD` scratch buffers
(`kvhash.c:54-55`). The spilled value is never staged in either of them, so
`.bss` is unchanged and pressure on `bkt_buf` *falls*. P3 ("no per-entry RAM")
is unaffected.

## 3. Performance — reads

Using the cost rule RESULTS.md §5b establishes (read cost ≈ map gets × ~7 ms)
and K11's "a map get is two `blob_db` calls", their measurements divide to
**~3.65–3.89 ms per blob op** (`check` 14.605/4, `byid` 7.770/2). Applying
that to changed op counts — **derived, not measured**:

| operation | today | + spill | + spill & derived bucket ids (§6) |
|---|--:|--:|--:|
| `card_owner` miss (5 B value, inline) | 7.165 ms · 2 ops | ~7 ms · 2 ops | **~3.7 ms · 1 op** |
| `card_owner` hit (5 B value, inline) | ~6.8–7.0 ms · 2 ops | ~7 ms · 2 ops | **~3.7 ms · 1 op** |
| `person_get` (363 B value, spills) | 7.770 ms · 2 ops | **~11 ms · 3 ops** | ~7.4 ms · 2 ops |
| `check` = `card_owner` + `person_get` | 14.605 ms · 4 ops | **~18.3 ms · 5 ops** | **~11 ms · 3 ops** |

Two things this table is meant to make obvious:

- **The threshold does the work.** persondb's credential index stores a 5-byte
  CBOR uint, so it never spills and never pays. Only the 363 B records do.
- **Spill alone costs the headline read ~25 %.** That is the honest price.

Not in the table, and pushing the middle column further the wrong way: the
same bytes split across more blobs (5 699 → 10 511, §4.3) means more, smaller
slots per 64 KB erase block, so `blob_db`'s slot-header walk lengthens on
*every* get. The direction is certain; the magnitude is not, and a board run
is the only way to settle it (§10).

## 4. Performance — writes and capacity

### 4.1 Write amplification

Today a `set` rewrites the whole bucket: **660 B written on average to change
a 363 B record.** With spill and the in-place fast path (§2.3), the write is
the 363 B value blob and the bucket is untouched.

### 4.2 Compaction, which is where the tail lives

RESULTS.md §5b measures `persondb_person_put` at **83.920 ms into a settled
bucket and 807.7 ms when the write grows a bucket into compaction** — a 9.6×
spread on the same function in the same run, with erase at ~1.09 s. The gap
is erase, not extra work.

Compaction frequency is set by how fast a bucket's append-log fills with
superseded slots. Cutting the bytes appended per update from 660 B to 363 B
(−45 %) roughly halves how often that 807.7 ms case is reached. It does not
make `put` bounded — the caller still cannot predict which case it gets
(B3, K10) — but it makes the bad case rarer.

### 4.3 Capacity — the strongest argument

A persondb bucket entry goes from `4 + 9 + 363` = 376 B to `4 + 9 + 8` = 21 B.
Enumerated, not modelled:

```
          maps  occupied  max bkt  % of cap  over
today       16      5699     2711       66%     0
spill       16      5699      147        4%     0
spill        4      2031      315        8%     0
spill        2      1022      462       11%     0
spill        1       511      777       19%     0
```

**One map replaces sixteen, with the fullest bucket at 19 % of the ceiling.**
The sizing exercise that K2/K3/K9/K10 forced on the application — a script
that reimplements `fnv1a`, the key format and the CBOR sizing rules to pick a
shard count offline — largely stops being necessary for record-shaped maps.

The flash cost of that, counted symmetrically (packed bytes plus one slot
header for every blob that exists, on both sides):

| | blobs | total |
|---|--:|--:|
| today, 16 maps | 5 699 | 3.66 MiB |
| spill, 1 map | 10 511 | 3.80 MiB |
| | | **+144 KiB (+3.8 %)** |

Per person that is +8 B for the reference and +14 B for the value blob's own
slot header. Cheap for what §4.3 buys, and it is the whole cost — the record
bytes themselves do not grow.

## 5. Crash behaviour, and the residue question

The commit stays a single bucket `update`, so **visible state is always
complete-old or complete-new** and no intent blob is required:

| Crash point | On flash | Visible state |
|---|---|---|
| after §2.3 step 1 | new value blob written, bucket still references the old | complete **old** — new blob unreferenced |
| during step 2 | bucket slot torn; CRC fails, end-of-log | complete **old** |
| after step 2 | bucket references the new value, old blob still present | complete **new** — old blob unreferenced |
| after step 3 | clean | complete new |

So no torn read, no dangling reference, no partial value. What a crash *can*
leave is **one unreferenced value blob**.

This is not a new class of defect. `kvhash` already leaks exactly this way and
says so (`kvhash.c:319-322`):

> *"Publish the new bucket into the directory. A crash between the bucket
> write above and this update leaves an unreferenced blob (reclaimed by a
> later format), never a corrupt map."*

What changes is the **frequency**: today the window is open only when a bucket
is created for the first time; with spill it is open on every write of a
spilled value. P7 rates "no permanent leak" a **must**, and "reclaimed by a
later format" does not satisfy it. Whether widening an already-accepted
violation is tolerable is decision **D3** below — it is the one thing this
proposal cannot settle on technical grounds alone.

## 6. Interaction with derived bucket ids (K1/K11)

FINDINGS K11 calls derived bucket ids "the strongest single conclusion in
this register": with `bucket_id = base_id + hash % n_buckets` the directory
read disappears, per-map state becomes 16 bytes, and a map get drops from two
`blob_db` calls to one.

The two changes are independent but strongly complementary, and the right
column of §3 is the point: **spill alone regresses `check` by ~25 %; spill
plus derived bucket ids improves it by ~25 %**, while still delivering the
write and capacity wins. If both are wanted, they should land together, and
if only one can, this proposal is the weaker of the two.

## 7. The one part that is not local: no recovery hook

Everything above lives in `lib/containers/kvhash/`. Closing §5's residue does
not, and the obstruction is worth stating precisely because it is a finding
about the *shape*, not about `kvhash`:

- `map_ops` is `create · get · set · del` (`shape_map.h:50`). There is no
  `open`. `l2_containers.md` §3 collapses it deliberately — "resolve root,
  then call these" — which was free while every mutation was a single write.
  Spill is the first thing that makes the missing hook cost something: there
  is nowhere for a container to run bounded, idempotent recovery.
- Checking an intent lazily inside `dir_load` would keep it local, since that
  read already happens on every operation — except the directory is **exactly
  full** at maximum buckets: `DIR_HDR_LEN + 511 × 8 = 4096`
  (`kvhash.c:46-47`). Making room means 510 buckets, which perturbs
  `MAX_BUCKETS` and therefore every capacity number an application derived
  from it (K9).

Both routes out are cheap; neither is `kvhash`'s to choose alone. Hence D3/D4.

## 8. Options considered

| | **A — do nothing** | **B — value spill** *(recommended)* | **C — overflow chaining** | **D — restructure the app** |
|---|---|---|---|---|
| Fixes the K2 cliff | ✗ | ✓ (19 % of ceiling at 1 map) | partly — moves it to chain length | ✓ for that one app |
| Write amplification | bucket-sized | **value-sized** | bucket-sized | value-sized |
| Read cost | baseline | +1 op when spilled | +1 op per chain hop | +1 op |
| Local to `kvhash` | — | **✓** | ✓ | ✗ — every app repeats it |
| New crash residue | none | one orphan blob per spilled write | none (single commit) | same, at app level |
| Benefits other clients | — | **all Map users** | all Map users | no |
| Effort | 0 | ~2–3 days | ~2 days | ~1 week per app |

**Why not C.** Overflow chaining (the `kvlist` §4.2 mechanism) keeps every
mutation a single commit and adds no residue, which is genuinely attractive.
But it leaves write amplification untouched — a chained bucket is still
rewritten whole — and it makes reads *worse* in the same case spill does,
without the capacity collapse §4.3 delivers. It is the better answer if D3
resolves against accepting the residue.

**Why not D.** `app_cbor_persondb` could store one blob per person itself and
keep maps as indexes. It gets the same capacity win, but every future
application repeats the work, and the packing decision is L2's to make
(`l2_containers.md` §1). Moving the mechanism down is the same argument the
large-payload proposal made for segmentation: write it once, where the
atomicity primitives already are.

## 9. Kconfig

```
config BLOB_CONTAINER_KVHASH_SPILL
	bool "Spill large values to their own blob"
	depends on BLOB_CONTAINER_KVHASH
	default n              # P4: off = today's code and today's flash bytes

config BLOB_CONTAINER_KVHASH_SPILL_MAX
	int "Inline value limit in bytes (above this, a value gets its own blob)"
	depends on BLOB_CONTAINER_KVHASH_SPILL
	default 64
	# BUILD_ASSERT: >= 8, so a spilled reference is never larger than the
	# inline value it replaces.
```

With `SPILL=n` the module compiles to today's code and a store is
byte-identical on flash. With it on, a value at or below `SPILL_MAX` is stored
exactly as today — the flag bit is simply not set — so a small-value map
(persondb's credential index) is indistinguishable from an unspilled build.

## 10. Test plan

Extends `tests/lib/containers/kvhash/` (to be created) and the existing
`tests/lib/kvdb/` conformance suite, which already runs per backend.

1. Boundary matrix at `SPILL_MAX`−1, `SPILL_MAX`, `SPILL_MAX`+1 (first
   spilled), and one payload.
2. Round-trip of a spilled value; `get` with `out_sz` short returns `-ENOMEM`
   with the true `vlen` **without** reading the value blob (assert op count).
3. Transitions: inline→spilled, spilled→inline, spilled→spilled (asserting the
   in-place fast path leaves the bucket blob unchanged — `blob_db` op count).
4. `del` of a spilled entry tombstones the value blob; a later `get` is
   `-ENOENT`, and no live slot for that id survives.
5. Crash injection at each of §2.3's three steps using the
   `tests/support/flash_fault` shim: remount, assert the value is fully old or
   fully new, and count unreferenced blobs against whatever D3 decides is
   allowed.
6. Capacity: fill a single-map store past today's `-ENOSPC` point and assert
   it now succeeds; then fill to the new ceiling and assert a clean `-ENOSPC`
   with the map still consistent.
7. `SPILL=n` build produces byte-identical on-flash images for the existing
   suite — the P4 "costs nothing when unused" check.
8. Re-run `app_cbor_persondb` on the nRF5340-DK with `SPILL=y` and one map, and
   publish the `check` / `person_get` / `put` figures against RESULTS.md §5b.
   §3's read numbers are derived; this is what would make them measured, and
   §3's un-quantified slot-walk effect is the reason it matters.

## 11. Compatibility — and a prerequisite bug

Spill changes `kvhash`'s bucket interpretation, so a store written by one
build must not be misread by the other. **It currently would be.**

`dir_load` validates the magic and the bucket count and then **never reads the
version field** (`kvhash.c:129-141`), although `kvhash_create` faithfully
writes it (`kvhash.c:201`) and `l2_containers.md` §4.3 describes it as
reserving "room to evolve the layout". Today the byte is written and ignored.

So, as a prerequisite and independent of this proposal:

- `dir_load` must reject `version > KVHASH_VERSION` with `-ENOTSUP`;
- spill bumps `KVHASH_VERSION` to 2 **only when a bucket actually contains a
  spilled entry**, so a `SPILL=y` build that has never spilled still produces
  a v1 store that older firmware reads correctly.

This mirrors the "refuse, don't reformat" property the large-payload proposal
§4.1 argues for at L1: an unrecognized format should be detected, not
misparsed.

## 12. Decisions needed from review

- **D1 — Adopt spill at all?** Recommendation: **yes, but only together with
  derived bucket ids (K1/K11)**. Alone it trades a 25 % read regression for
  capacity and write wins; together the read improves too (§6). Landing spill
  first would regress the one number the app publishes.
- **D2 — Default `SPILL_MAX`.** Recommendation: **64**, matching
  `CONTAINER_KVLIST_INLINE_MAX`'s default so the two Map providers agree on
  what "small" means. Anything from 32 to 256 is defensible; the value only
  needs to sit below the smallest record an application wants out of its
  buckets.
- **D3 — Is the orphan-blob residue acceptable?** The blocking question (§5).
  Options: (a) accept it as a widening of the documented `kvhash.c:320` case,
  with a `LOG_WRN` and a `format`-time sweep; (b) add an intent record and a
  recovery hook, which needs D4; (c) reject spill in favour of overflow
  chaining (§8-C), which has no residue. Recommendation: **(b) if D4 is
  affordable, else (c)** — (a) knowingly widens a P7 "must" violation, and
  this proposal should not be the reason that becomes routine.
- **D4 — Give the Map shape an `open`.** Adding `open(root)` to `map_ops`
  gives every container the recovery hook §7 shows is missing, at the cost of
  one more op vector entry and an `open` call in `kvdb`/`persondb`. It is a
  shape change, so it is not this proposal's to make. Recommendation: **yes,
  as its own change** — `kvtree` will need it for the same reason the moment
  it does a multi-node split.
- **D5 — Flag bit placement.** High bit of `klen` (proposed, caps keys at
  32 767) versus a per-entry flags byte (+1 B on every entry, including inline
  ones). Recommendation: **the `klen` bit** — no cost to entries that do not
  spill, and the cap is far above any key a single payload can hold anyway.

## 13. Summary

`kvhash` packs values into shared bucket blobs, and that single decision
produces the `-ENOSPC` cliff, the offline sizing exercise, and write
amplification proportional to how many neighbours a key has. Spilling values
above a threshold into their own blobs removes all three: one map replaces
sixteen for the measured workload, the fullest bucket falls from 66 % to 19 %
of the ceiling, and a write costs the value rather than the bucket — for
+3.8 % flash and no additional RAM.

The change is local to `kvhash`, needs no `map_ops` or `blob_db` change, and
keeps the single-write commit that makes today's `set` atomic. Its two real
costs are one extra read per spilled `get` — which derived bucket ids more
than repay — and an unreferenced value blob after a crash, which is the same
residue `kvhash` already accepts but far more often. That last point, and the
missing `open` hook that would let it be reclaimed, are what §12 asks review
to settle.
