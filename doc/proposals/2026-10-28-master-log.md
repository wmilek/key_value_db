# Design change proposal — the master as a log, and format features

Status: **accepted** · 2026-10-28 · phase 1 implemented in #101–#104,
documented in `doc/impl/l1_bucketlog.md` (implementation plan:
`2026-11-01-master-log-implementation-plan.md`); phases 2, 3 and the fuse trips
are open
· Target implementation design: `doc/impl/l1_bucketlog.md` (§3.1 master sector, §5.1 mount, §5.6 compaction, §5.6a segmented writes, §6 crash tables)
· Also changes: PR #63 (`blob_db_protect`: master and bucket header formats, compaction nonces) and PR #62 (`BLOB_DB_DELTA`: its slot flag)
· Evidence: `app_perf_blob_db/RESULTS.md` (`why` lines: master writes per phase)
· Governed by `doc/principles.md`

**The ask.** Nothing runs this stack in production yet, so the master format
can still change. Two things are wrong with it:

- **It is written too often, and every write is an erase.** A master write
  is a whole-sector replace. A large-payload partial write costs two of them,
  which is nearly all of its 1.0–1.1 s on the nRF5340-DK. Compaction on the
  scratch path costs two more. PR #63's AEAD mode adds about one per
  compaction on 64 KB blocks, including on the UBI atomic-replace path, which
  writes no master today.
- **The format is not flexible.** Each feature combination takes a major
  number: 1 plain, 2 large payloads, 3 protected, 4 protected with large
  payloads. Adding delta slots (PR #62) would double that. Slot flags have no
  owner: PR #62 gives bit 4 to `DELTA` and PR #63 gives bit 4 to
  `COMPACTED`. Geometry is not recorded at all, so on UBI the bucket count
  rests on the volume table alone.

**Short answer.** Each master sector becomes a header written once per erase,
followed by an append-only log of fixed-size records. Each record is a full
copy of the mutable state. An update programs one record into erased space
and erases nothing, and a sector is erased once per 62 to 1 362 updates (§3.2). The
header records the geometry and three feature sets (`incompat`, `ro_compat`,
`compat`) in place of the major-number combinations. Protection is a `suite`
the build requires, not a feature the store chooses. Compaction no longer
touches the master on any path. On UBI with atomic replace it never did. On
the scratch path, recovery is keyed on the scratch sector's seal instead of a
`COMPACTING` record. In AEAD mode, its nonces stop coming from the master
counter. Each record also carries 32 **fuses**:
bits that record damage and recovery as post-mortem evidence. The field and
its rules are format from phase 1. Every record carries the word forward, OR-only,
and nothing but a format clears it. Tripping fuses is behaviour, and it can
come later (§4.5a).

---

## 1. Requirements

| # | Requirement | Consequence |
|---|---|---|
| M1 | A master update erases nothing in the common case | an append-only log of records inside the master sector (§3) |
| M2 | Compaction never writes the master, on any path | recovery keyed on the scratch seal, with no compaction state in the master (D17); AEAD compaction nonces drawn at random, not reserved from the master counter (§6) |
| M3 | A large-payload write costs no erase beyond its slots | the sweep window is two log records (§5) |
| M4 | Crash safety as today | the same two commit primitives: a slot append and a master update; a torn record reads as no update (§7) |
| M5 | Every write programs erased bytes only | works on NOR, on flash with ECC and on UBI; no rewrite of programmed bits (§8) |
| M6 | A store says which features it uses; a build refuses what it does not understand | `incompat` / `ro_compat` / `compat` in the header (§4) |
| M7 | A protected build accepts only a store protected exactly as it expects | `suite` must equal the build's; the store never chooses protection (§4.3) |
| M8 | The geometry is part of the format | `peb_size`, `n_buckets`, `write_align` and the payload cap in the header, checked at mount (§3.1, D19) |
| M9 | A few bytes of steady-state RAM, fixed at build time (P3) | the log cursor as a record index (2 B), the active sector's `rot_gen` (4 B), the header's feature sets (12 B), a failed-write flag, and the fuse word (4 B) |
| M10 | Evidence of damage and recovery survives until a format, and no other path clears it | 32 OR-only fuses in every record (§4.5) |

## 2. Decisions

| # | Decision | Alternatives rejected |
|---|---|---|
| D1 | A header plus fixed-size records, each a **full copy** of the mutable state | delta records replayed in order (mount logic, and one rotten record would break the chain) |
| D2 | Keep two master sectors; a full sector rotates into the other | one sector (a torn erase would leave no master) |
| D3 | The format guarantees that a sector's record slots are a written prefix followed by erased slots (§3.2a): a failed record write is followed by a rotation, never by a record in the next slot. Mount finds the end by a binary search on "fully erased" and reads only the slots it needs (amended after the nRF52840-DK A/B on #104: reading every slot of a 64 KB master cost +35–43 ms per mount) | reading every slot (what PR 4 first shipped: it tolerates a store on which the invariant was broken, but no writer breaks it, and it costs a sector read per mount); leaving gaps to the reader (a store written without the invariant could never be binary-searched, by any later build) |
| D4 | Large payloads keep the mount-time sweep, as two log records | reclaiming orphans in compaction (§5.2, kept as open item 1) |
| D5 | AEAD compacted slots take a random 95-bit nonce base per compaction, and their nonce is the base plus the slot's offset; one named nonce bit separates them from appended slots (§6) | reserving `peb_size` values from the master counter (PR #63 §3.4); a 63-bit random prefix with the offset beside it (about 2⁻¹⁷ over 10⁷ compactions, beyond the 2⁻³² budget) |
| D6 | Features in three sets, ext4-style; a slot flag belongs to exactly one feature | a major number per combination (doubles with every feature) |
| D7 | A feature that adds a slot flag is set **at format**, as ext4 sets `incompat` features at `mkfs`; an existing store gains one only by an explicit upgrade that puts it in every master header before any slot uses it (§4.2) | marking on first use (until the next rotation the bit lives only in the active sector's records, so a fallback to the other sector loses it while slots already use it) |
| D8 | Protection is a `suite` byte plus an `incompat` bit, and the build requires it | an ordinary feature the store declares (a forged unprotected master would be obeyed: a downgrade) |
| D9 | No writes over programmed bits | clearing bits in place (§8: STM32F3, nRF word-write limits, ECC flash; and an attacker can clear bits too) |
| D10 | Major 5 for the new layout; majors 1–4 are refused, never migrated | migration code for stores that exist only on development boards |
| D11 | Fuses: a 32-bit OR-only word in every record that blob_db carries forward and never interprets, except to skip persisting a bit already set | bits cleared in place (§8); counters (more bytes than a post-mortem needs); fuses in the header (written once per erase, so a trip would need a rotation) |
| D12 | A format clears the fuses | carrying them over (a format is the intervention that ends the store's history, P9; carrying them needs a readable old master, which an autoformat by definition lacks) |
| D13 | The slot walk accepts the flags the **build** knows. A sealed slot whose CRC verifies but which carries a flag outside them makes its bucket refuse writes; it is never compacted. Compaction also refuses when programmed bytes lie beyond one slot span past where its walk stopped | taking the accepted flags from the store's features (a lost bit would turn live slots into a "torn tail" that the next compaction drops); treating such a slot as torn (the same loss, from a single slot); refusing on any programmed byte past the stop (a page program torn out of order leaves stray bits inside the torn slot's span, and the bucket would refuse writes for good) |
| D14 | Every record raises the id and nonce ceilings to the current value plus a step: `hint = max(hint, next + 1 + STEP)`. A ceiling-only record is written only when allocation reaches the ceiling and no other record has carried it forward. `erase_all`'s record is the one exception: it resets the id ceiling (§3.3) | copying the persisted ceiling unchanged (allocation then needs a ceiling-only record every step, even right after another record was written) |
| D15 | `format_minor` is 0 for all of major 5; every extension is a feature bit (§3.6) | raising the minor (a rotation by an older build would write its own lower minor and erase the record of the newer one) |
| D16 | A record's CRC covers `LE32(rot_gen)` too, in every suite, so a record verifies only under its own header | binding it in the protected MAC only (a stale unprotected record would verify) |
| D17 | Compaction keeps no state in the master. Every mount checks the scratch seal and restores the bucket that a sealed image names (§6.1) | `COMPACTING` and `CLEAN` records around each scratch-path compaction (a rotten `COMPACTING` record hides an interrupted compaction and loses the bucket's data; two records per compaction) |
| D18 | A format writes **both** master sectors, so an erased master sector beside an ok one only ever means a torn rotation, and mount doubts on it (§3.4a) | writing one sector and leaving the other erased (an erased sector then means either "never rotated" or "a rotation torn after its erase", and a doubt-ending rotation torn there loses the ceiling the scan raised: §3.5) |
| D19 | The payload cap and `write_align` are geometry: recorded in the header, and a mount refuses a store whose values differ from the build's and the device's (§3.1) | leaving them to Kconfig and the driver (a smaller cap reads a sealed, intact slot as a torn tail; another `write_align` pads slots differently, and `rec_size` does not show it) |
| D20 | A slot-flag feature is written only into a store whose headers carry it. A build with the feature, on a store without it, writes as a build without it (§4.2) | refusing the mount (the store says which features it uses, and a build follows); writing the flag anyway (the headers would no longer carry every feature the store uses) |

## 3. Master sector layout (major 5)

```
offset 0      header            written once, right after the erase
offset 128    record[0]         written with the header
offset 128+R  record[1] ...     appended, one per update
              erased to the end of the sector
```

128 is `BLOB_DB_MASTER_HDR_MAX`, the window every reader already stages, so a
reader of any age reads the frozen prefix from the same place.

### 3.0 Encoding

- **Integers** are little-endian, as today. The one exception is the AEAD
  compaction nonce, which reads `cbase` as a big-endian number (§6).
- **Checksums.** `crc32` is CRC32-IEEE (`crc32_ieee()`), and `prefix_crc16`
  is CRC16-CCITT seeded with `0xffff` (`crc16_ccitt()`), both as today.
- **Filler.** The header window's bytes from `hdr_len` to 128, and a record's
  padding from its layout size (48, or 64 protected) to R, are written as
  `0xff`. No checksum covers them, and a reader ignores their value.
- **Reserved** fields are written as 0, and a reader ignores their value.
  §3.6 says what a later field in them may rely on.
- **Limits.** `n_buckets` is 16 bits, so a store has at
  most 65 535 buckets. A mount on a larger partition refuses it with
  `-ENOTSUP`, and writes nothing. `peb_size` is 32 bits, and `rot_gen` and
  `rec_no` are 32 bits. Even at one rotation per update, `rot_gen` does not
  wrap within any device's erase budget, and a writer that would wrap it
  refuses with `-ENOSPC`. Ids and ceilings are 64 bits. `max_payload_len` and
  `write_align` are 16 bits each, as a slot's `val_len` already is.

### 3.1 Header

Nothing in it changes until the sector is erased again.

| Bytes | Field | Notes |
|---|---|---|
| 0..11 | frozen prefix | `magic` `BDMS`, `format_major = 5`, `format_minor = 0` (D15), `hdr_len` (64, or 84 protected), `reserved`, `prefix_crc16`; layout unchanged since major 1 |
| 12..15 | `rot_gen` | +1 per rotation; the sector with the higher valid value is active |
| 16..19 | `peb_size` | must equal the device's |
| 20..21 | `n_buckets` | must equal the device's |
| 22..23 | `rec_size` | R: 48, or 64 protected, rounded up to `write_align`; must equal what the build computes (§3.1 below) |
| 24..27 | `incompat` | a reader must understand every bit (§4) |
| 28..31 | `ro_compat` | a writer must understand every bit |
| 32..35 | `compat` | anyone may ignore an unknown bit |
| 36 | `suite` | 0 unprotected; otherwise mode and tag length, as PR #63's master byte 19 |
| 37..39 | reserved, 0 | |
| 40..55 | `store_uuid` | drawn at format when protected (PR #63 §3.1); 0 otherwise |
| 56..57 | `max_payload_len` | `CONFIG_BLOB_DB_MAX_PAYLOAD_LEN` of the build that formatted the store; must equal the build's (D19) |
| 58..59 | `write_align` | the device's write unit, which pads every slot; must equal the device's (D19) |
| 60..63 | `hdr_crc32` | over [0, 60) |
| 64..79 | `mac` | protected only: AES-CMAC-128(K_meta, [0, 64)) |
| 80..83 | `tail_crc32` | protected only: over [0, 80) |

A protected header whose CRCs verify but whose MAC does not classifies as
**corrupt**, as rot does: mount falls back to the other sector, with doubt.
Refusing the whole store instead would protect nothing, because anyone who
can write the flash can force the same fallback by breaking the CRC. Keeping
an attacker from rolling the store back is the job of PR #63's rollback
anchor, not of the classification. Once fuses trip, `MAC_FAIL` records it
(§4.5a).

**Every ok header must agree with the build and the device**, not only the
active one. Each sector that classifies as ok must carry the device's
`peb_size`, `n_buckets` and `write_align`, the build's `suite` and
`max_payload_len`, the `hdr_len` of that suite, and the `rec_size` the build
computes for that suite and the device's `write_align`. Otherwise mount
refuses the store with `-ENOTSUP` and writes nothing.

The last two are geometry for the same reason as the rest (D19). The slot
walk ends the log at a `val_len` above the build's cap, so a build with a
smaller cap would read a sealed, intact slot as a torn tail, and the bucket's
next compaction would drop it and every slot after it. Every slot is padded
to `write_align` (`slot_size_for()`), so another value walks the bucket at
the wrong boundaries. `rec_size` does not show it, because 48 is a multiple
of every `write_align` up to 16. Equality, not "at most", keeps both
directions safe: a build with a larger cap would write slots the store's own
build can no longer walk. Raising the cap is a format, as changing any other
geometry is. The CRC and MAC sit at fixed offsets in the record (§3.2), whatever R
is. R only adds filler, so a store whose `rec_size` differs from the build's
was written by another build or for another device. `write_align` must be a
power of two no larger than 128, because records start at offset 128. A
larger one is refused with `-ENOTSUP`, as today's `write_master()` refuses it.

**Equal `rot_gen`.** No writer gives two ok sectors the same `rot_gen`,
because a rotation always writes the active value plus one. Two ok sectors
with equal values cannot be ordered, and either could hold the newer state.
Mount treats that as a store with no usable master: `-EIO`, nothing written,
and `blob_db_format()`, or an autoformat that opts out of P9, discards it.

A device whose geometry differs from the header's is refused with `-ENOTSUP`
and nothing is written. In a protected store this closes the gap PR #63 left
on UBI, where the bucket count came from the volume table and only UBI's MAC
protected it. In an unprotected store it catches a build configured for
another partition.

### 3.2 Record

| Bytes | Field | Notes |
|---|---|---|
| 0..3 | `rec_no` | equals the record's position in this sector, from 0 |
| 4..7 | reserved, 0 | compaction keeps no state here (D17) |
| 8..15 | `next_id_hint` | the id ceiling (`l1_bucketlog.md` §13.1), raised by every record (§3.3) |
| 16..23 | `seq_hint` | the AEAD nonce ceiling (PR #63 §3.2), raised by every record (§3.3); 0 otherwise |
| 24..31 | `seg_owner` | the segmented write in flight; 0 = none |
| 32..35 | reserved, 0 | features live in the header only (§4.2) |
| 36..39 | `fuses` | the previous record's fuses, plus any tripped since; opaque, OR-only (§4.5) |
| 40..43 | reserved, 0 | |
| 44..47 | `crc32` | unprotected: over `LE32(rot_gen) ‖ [0, 44)` (D16); R = 48 |

A protected record puts a MAC where the unprotected CRC was:

| Bytes | Field |
|---|---|
| 44..59 | `mac`: AES-CMAC-128(K_meta, LE32(rot_gen) ‖ [0, 44)) |
| 60..63 | `crc32` over `LE32(rot_gen) ‖ [0, 60)` (D16); R = 64 |

`rot_gen` is the header's. With it in the CRC, and with `rec_no` equal to
the record's position, a record verifies only in its own slot under its own
header. A record left from an earlier rotation fails as damage, in either
suite, and the protected MAC adds the same binding against tampering. Check
order, as everywhere in PR #63: erased, then CRC (damage), then MAC
(tampering). A protected record whose CRC verifies but whose MAC does not
counts as not verifying, for the same reason a header's does (§3.1).


Records per sector: (sector − 128) / R.

| Sector | Unprotected (48 B) | Protected (64 B) |
|---|--:|--:|
| 4 KB | 82 | 62 |
| 64 KB | 1 362 | 1 022 |

### 3.2a The log invariant

In every master sector, the record slots are a **written prefix followed by
erased slots**. For some `end`, every slot below `end` is not fully erased (a
record, a torn record, or a failed write), and every slot from `end` to the
end of the sector reads fully erased. Every writer keeps this from phase 1,
because a later reader meets stores that today's builds wrote:

- Slots are written in order, and only at `end`.
- After a record write returns an error, the slot may still read erased. So
  the next master update rotates (§3.3) instead of writing the next slot,
  which would leave an erased slot below a written one.
- A slot is erased exactly when all R bytes read `0xff`. A written record
  never does, because `rec_no` is below the record count and so never
  `0xffffffff`. Padding up to `write_align` is written as `0xff`, but a
  record's first bytes never are.

A reader may therefore find `end` by a binary search on "fully erased", in
about log₂(records) reads (7 on 4 KB, 11 on 64 KB), and mount does (D3). The
search is a reader change only: no format change, and every store written by
any major-5 build already keeps the invariant.
A power cut cannot break the invariant: it tears at most the slot at `end`,
and nothing has been written after it.

### 3.3 Update

```
# cursor: the index of the next record slot; n_rec = (peb_size - 128) / R
master_update(state):
    if failed or cursor == n_rec: rotate(state); return
    rc = write record(rec_no = cursor, state) at 128 + cursor * R
    cursor += 1
    if rc < 0: failed = true      # the slot may still read erased: keep §3.2a
```

The write programs only erased bytes, moving forward through the sector:
`blob_db_store_write()`, which on UBI is `ubi_leb_write_at()`, as slot appends
already are.

**Every record raises the ceilings (D14).** Whatever a record is written for
(an id-ceiling step, `seg_owner`, a fuse, mount, or a
rotation's record[0]), it carries

```
next_id_hint = max(next_id_hint, next_id + 1 + BLOB_DB_ID_HINT_STEP)
seq_hint     = max(seq_hint,     seq     + 1 + BLOB_DB_SEQ_STEP)      # AEAD only
```

and RAM takes the new values only once the record has landed, as
`persist_next_id_hint()` does today. The rule "persist first, then use" is
unchanged:
`alloc_id()` still appends a record before it hands out an id at or above the
ceiling. That record is just rarely needed, because any other record written
within the last step already carried the ceiling forward. The `max` keeps
every ceiling monotonic, so no record ever publishes a ceiling below one
already on flash. That is the hazard `compact_commit()` guards against by
copying the ceiling instead of `next_id`. The ids a crash skips stay bounded
by `STEP + 1`, because a record never puts the ceiling further ahead than
that. A record whose write fails raises nothing.

**`erase_all` resets the id ceiling.** `blob_db_erase_all()` empties the id
space, so its record is the one that lowers the ceiling:
`next_id_hint = 2 + 1 + STEP`, written directly instead of through the `max`.
It is safe for the reason it is today. The record is written only after every
bucket has been invalidated and the root bound again, so no id at or above 2
is left on flash but the root's. A cut before the record lands leaves the
older, higher ceiling, which is safe. A torn record is doubt, and the scan only
raises the ceiling. `seq_hint` is never lowered: `erase_all` keeps the key, and
a nonce under one key must never repeat. Every later record follows the `max`
rule again.

### 3.4 Rotation

```
rotate(state):
    other = !active
    replace(other, header(rot_gen + 1, features = header) ‖ record[0](state))
    active = other; cursor = 1; failed = false
```

`blob_db_store_replace()` is an erase and a write on `flash_area`, and
`ubi_leb_change()` on UBI with atomic replace. UBI checks its data checksum
over the replaced length only, so the appends that follow keep it valid,
exactly as slot appends after a bucket compaction do today. Rotation is
today's A/B master write, made once per sector's worth of updates.

### 3.4a Format (D18)

```
format(state):                            # blob_db_format(), the virgin format, an autoformat
    invalidate or erase the buckets       # as today, before any master is written
    replace(B, header(rot_gen = 1, features = the build's) ‖ record[0](state))
    replace(A, header(rot_gen = 2, features = the build's) ‖ record[0](state))
    active = A; cursor = 1; failed = false; fuses = 0
```

Both sectors carry a header and a record for the same state, so after a
format no master sector is erased except inside a rotation. An erased sector
beside an ok one is then always a rotation torn after its erase, and mount
doubts on it (§3.5). Leaving the second sector erased, as today's format
does, would make that state ambiguous: "never rotated yet" and "a rotation
torn after its erase" would look the same. A doubt-ending rotation (§3.5) torn
there would then leave a mount with no doubt and an old ceiling.

A cut between the two writes leaves B ok and A erased. That is doubt, and the
scan is cheap, because format has emptied the buckets first. A cut inside B's
write leaves no usable master beside an erased A, which is refused with `-EIO`,
as a torn first master is today. The cost is one sector write per format,
whose sectors are erased anyway.

### 3.5 Mount

```
read [0, 128) of both sectors; classify each as erased, corrupt, foreign or ok (§3.1 of l1_bucketlog.md)
foreign on either sector                               -> -ENOTSUP, write nothing
features, suite, rec_size or geometry of any ok sector not acceptable (§3.1, §4.4) -> -ENOTSUP, write nothing
both ok with equal rot_gen                             -> -EIO, write nothing (§3.1)
no ok sector:                                          # as today (l1_bucketlog.md §3.1)
    both erased                                        -> format
    otherwise                                          -> no usable master (below)

for an ok sector x:                                    # a binary search on "fully erased" (D3, §3.2a)
    end(x)    = one past the last record slot that is not fully erased
    newest(x) = the newest record below end(x) that verifies, if any
x is usable when newest(x) exists

hi = the ok sector with the higher rot_gen; lo = the other, if ok
if hi is usable:     active = hi; fell_back = no
elif lo is usable:   active = lo; fell_back = yes      # a torn rotation, or every record of hi rotten
else:                no usable master                  # -EIO, write nothing; or erase all with
                                                       # AUTOFORMAT_ON_CORRUPT, as today

cursor = end(active); state = newest(active); fuses = state.fuses   # §4.5
doubt  = the record just below cursor does not verify
      or fell_back
      or the other sector is not ok                    # erased included: a torn rotation (D18)
      or the store reports damage on a master sector   # as today, for UBI
if the scratch sector carries a valid seal:            # every mount, any backend (D17, §6.1)
    restore the bucket its image names; erase scratch
if doubt:
    trip MASTER_DOUBT                                    # RAM only; once fuses trip (§4.5a)
    scan the buckets and raise the id ceiling            # RAM only, as today (§5.1)
if one ok sector has a feature bit the other ok sector lacks:
    rotate                                               # finish an upgrade (§4.2)
if doubt still holds:                                    # end it, after the scan
    if the other sector is not ok,
       or fell_back, or the store reported damage:
        rotate                                           # rewrites the other sector
    else:
        append a record                                  # the last record now verifies
if mount tripped a new fuse and appended no record: append one   # once fuses trip (§4.5a)
```

"Usable" separates a sector whose header is ok from one mount can take its
state from. A sector with an ok header and no record that verifies (a rotation
torn after its header landed, or every record rotten) is not usable. Mount
then takes everything from the other sector, `cursor`, `state` and `fuses`
included, and doubts. Its ending write is a rotation into the unusable sector.
Neither sector usable is a store with no usable master, handled as today.
That includes a first format torn after its header landed, with the other
sector still erased: today a torn first master is refused with `-EIO` the
same way, because only two erased sectors format by themselves. The feature,
suite and geometry checks above run on every ok header, usable or not, so a
rotation torn by a newer build still makes an older build refuse the store.
The lower sector is read only when the higher one is not usable.

"Doubt" carries today's rule over: a ceiling that might have been
acknowledged and then lost makes mount scan. A torn last record was never
acknowledged, but bit rot in the last committed record looks the same, so
both trigger the scan. A rotten record further back is harmless, because a
later full-state record replaces it.

**An erased other sector is doubt (D18).** Today's rule scans whenever one
master is not ok, and the log keeps it. Doubting only on a corrupt other
sector would lose a ceiling. Take a store whose active sector Y rots in its
header. Mount takes the other sector X, doubts because Y is corrupt, scans,
and ends the doubt by rotating into Y. If power is cut after Y's erase, the
next mount finds X ok and Y erased. Without this rule nothing would make it
doubt, and it would take X's ceiling, which is older than the ceilings Y
carried, and issue the ids bound under them a second time. Since a format
writes both sectors (§3.4a), an erased sector beside an ok one never arises
in normal use. The rule costs a scan only after a torn rotation.

**Ending the doubt.** A mount that doubts also ends the doubt before it
returns, so the next mount does not scan again. Only the doubt conditions
decide this, never the fuse word. Once fuses trip (§4.5a), a mount that finds
`MASTER_DOUBT` already set ends the doubt all the same. The order is fixed:

1. Detect the doubt from the master state. Once fuses trip, also trip
   `MASTER_DOUBT` in RAM.
2. Scan the buckets and raise `next_id` in RAM. Nothing is written yet.
3. Write once, carrying the raised ceiling (D14) and the pending fuses.
   - If the other sector is not ok (erased, or corrupt), if mount fell back
     after a torn rotation, or if the store reported damage on a master
     sector, **rotate**: one erase, which rewrites the other sector.
   - If only the last record failed to verify, **append a record**: no erase.
   - A write that mount makes anyway after the scan counts as this write when
     it ends the doubt (an upgrade's rotation).

The scan comes before the write because the write is what tells the next
mount the ceiling is safe. Written first, a cut before the scan finished would
leave a clean-looking master whose ceiling was never checked. A cut during
step 3 leaves a torn record or a torn rotation, and both are doubt again: the
next mount scans and writes again. The scan writes nothing, so repeating it
is harmless.

Without this, the doubt would last until the next rotation, because records
are appended to the active sector and nothing else rewrites the other one.
That can be never: on UBI with atomic replace, a workload that only rewrites
existing ids appends no records at all. Rotating over a corrupt other sector
discards it, as today's next master write does. Once fuses trip, the
evidence is kept in a fuse (§4.5a). If UBI reports damage on the active sector again at the next attach,
that mount doubts again. That costs a scan, never a wrong ceiling.

Cost: two header reads as today, plus the binary search for `end` in the
higher sector (7 record reads on 4 KB, 11 on 64 KB) and one read per record
walked back from it, once per mount. The lower sector is searched too when
mount falls back. Mount checks only the records it reads: the newest that
verifies, and any that fail above it. A rotten record below the newest is
not seen at mount, so `MASTER_REC_ROT` (§4.5a, provisional) needs a full log
read off the mount path, such as maintenance, when fuses land. (PR 4 first
read every slot; on a 64 KB master that was +35–43 ms per mount on an
nRF52840-DK, D3.)

### 3.6 Extending the format

Major 5 can grow without a new major, under these rules:

- **No version number inside the major (D15).** `format_minor` stays 0. A
  writer writes 0, and a reader ignores it. A build learns what a store needs
  from its feature sets (§4.1), which every writer copies verbatim at
  rotation. A minor number is not copied that way: a rotation by an older
  build would write its own lower minor, and the record of the newer one
  would be gone.
- **A new field takes reserved bytes,** in the header or in the record, and
  comes with a feature bit. Older builds write reserved bytes as 0 in every
  record and in every header they write at rotation. So the bit depends on
  what that zeroing does:
  - If 0 means "absent", and an older writer zeroing the field loses only a
    hint, the bit is `compat`. Its field must read 0 as absent, because the
    bit survives an older writer and the field does not.
  - If the field must survive every writer, the bit is `ro_compat`, so an
    older build refuses to write the store (§4.1).
  - If an older reader would misread the store without the field, the bit is
    `incompat`.
- **A new meaning for existing bytes,** or a new slot flag,
  is `incompat`.
- **The record layout and R are fixed per suite** for all of major 5. A
  record that needs more than its reserved bytes is a new major.
- **The fuse word** follows its own rules (§4.5). Every writer carries it, so
  it needs no feature bit.

## 4. Features and fuses

### 4.1 The three sets

| Set | A build that meets an unknown bit | Example |
|---|---|---|
| `incompat` | refuses the store, `-ENOTSUP`, writes nothing | a new slot flag; a new suite |
| `ro_compat` | may read, must not write | a structure every writer has to maintain |
| `compat` | ignores it | a hint a reader can do without |

blob_db has no read-only mount, so until it has one an unknown `ro_compat`
bit is refused like an `incompat` bit.

A Kconfig option that changes no byte on flash is not a feature. Most of
PR #62's options (`SKIP_IDENTICAL`, `READ_WINDOW`, `WALK_CHUNK`, ...) are
write or read policy, and stay out of these sets.

### 4.2 Registry

One table in `blob_db_internal.h` owns every bit, so two features cannot take
the same slot flag:

| `incompat` bit | Feature | Slot flags it owns | Set |
|---|---|---|---|
| 0 | `LARGE_PAYLOADS` | bit 2 `INDEXED`, bit 3 `SEGMENT` | at format, or by an upgrade |
| 1 | `PROTECTED` | bit 4 `COMPACTED` (AEAD suites) | at format only (§4.3) |
| 2 | `DELTA` (PR #62) | bit 5 `DELTA` (moved from bit 4) | at format, or by an upgrade |

`SEALED` (bit 0) and `TOMBSTONE` (bit 1) belong to every store. Bits 6 and 7
are free, and `slot_hdr.reserved` stays kept for a key epoch (PR #63 §11.4).
A `BUILD_ASSERT` checks that no two features own the same bit.

**Set at format, as ext4 does at `mkfs`.** `blob_db_format()`, the virgin
format at mount, and an autoformat write the build's slot-flag features into
the header. ext4 lets its driver set some `ro_compat` and `compat` bits on first
use (`large_file`, `dir_nlink`), where losing one cannot make a reader misread
data. It never does so for `incompat`, and blob_db does not either. A bit set
on first use would live only in the active sector's records until the next
rotation. During that time, a rotten active header, or UBI standing up an older
copy of that block, makes mount fall back to the other sector, which has never
seen the bit, while slots already use the feature. The cost is ext4's: a build
without the feature refuses a store formatted with it, even if no slot uses
it. Refusing is the safe direction.

**A build writes only the features the store has (D20).** The other direction
is not a refusal. A build with a slot-flag feature accepts a store formatted
without it, because the store uses nothing the build cannot read (§4.4), but
it writes that store as a build without the feature. It decides from the
store's `incompat`, not from its own Kconfig. A `LARGE_PAYLOADS` build on a
store without bit 0 writes no INDEXED or SEGMENT slot, and `blob_db_update()`
and `blob_db_write()` beyond `CONFIG_BLOB_DB_MAX_PAYLOAD_LEN` return
`-EINVAL`, as they do in a build without the feature. Reads are unchanged:
such a store holds no segment. Writing the flags anyway would break the
invariant below. A build without the feature would then mount the store, as
its headers allow, and meet slots it cannot read (D13). The store gains the
feature only by an upgrade. The same holds for `DELTA` (phase 3).

**The invariant.** Every master sector that classifies as ok carries every
feature bit the store has. Format meets it: both sectors carry the bits
(§3.4a). Rotation keeps it, because the new header
copies the old header's feature sets verbatim, bits the build does not know
included. Features live only in the header; records carry none. Bits are never
cleared, except by a format.

**Upgrade.** An existing store gains a feature only by an explicit call (open
item 7), never as a side effect of a write. The upgrade rotates twice, so both
sectors' headers carry the new bit. That is two erases, paid once. Until both
rotations have landed, the store does not use the feature. A mount that finds
one ok sector with a bit the other ok sector lacks finishes the upgrade with
one more rotation before it returns. The feature check (§4.4) takes the union
of every ok header, so a build that does not know the bit refuses the store
even halfway through the upgrade.

**The slot walk accepts the build's flags (D13).** The flags a walk accepts are
`SEALED | TOMBSTONE | the slot flags of every feature the build has`, fixed at
compile time as today. The store's features decide only whether mount accepts
the store (§4.4). The registry gives every flag one owner for good, so reading
a slot with the build's meaning of a flag is never a misread. A walk that stops
at a sealed slot with a plausible `val_len`, an intact CRC and a flag the build
does not know has found a slot written by another build, not a torn append. Its
bucket refuses writes with `-ENOTSUP` and is never compacted. Reads of any id
in that bucket return `-EIO`, because a newer slot for that id may lie beyond
the one the walk cannot read. That replaces a silently stale value. Once fuses
trip, `FOREIGN_SLOT` records it (§4.5a). Like everything else on the store, the bucket is
kept until a format (P9).

**The compaction guard (D13).** Compaction drops everything past the point
where its walk stopped, so it must not drop slots the walk could not read. One
torn append can leave programmed bytes past that point, but only inside its
own span. An append programs one slot, at most
`span = slot_size_for(CONFIG_BLOB_DB_MAX_PAYLOAD_LEN)` bytes from where the
walk stops. Within that span, it may program them in any order: a QSPI NOR
page program (256 B) programs the whole page at once, so a cut can leave bits
anywhere in the page, past a header torn too badly to size. Every append that
fails forces a compaction before the next one (`confirm_cursor()`), so no
second append lies beyond a torn one. Compaction therefore refuses (`-EIO`)
only when a programmed byte lies at or beyond `stop + span`. Bytes inside the
span are what a torn append leaves, and they are dropped as today. A guard
that refused on any byte past the stop would turn one power cut inside a page
program into a bucket that refuses writes until a format. With the cap and
`write_align` recorded as geometry (D19) and foreign slots detected above,
the guard is a backstop for data whose origin is unknown.

### 4.3 Protection

Protection differs from the features above in who decides. The store says
which features it uses, and a build follows. Protection is a requirement of
the build: a build that obeyed the store could be handed a forged unprotected
master (an unprotected master has no MAC to fail) and would serve forged
plaintext as data. PR #63 rules this out with "a build mounts only its own
suite" (D8 there), enforced through the major number. Here:

- `suite` records the mode and tag length. It is one value, not bits,
  because the modes exclude each other. A future cipher is a new value.
- `incompat` bit 1 `PROTECTED` is set too, so a build that knows nothing about
  protection refuses the store.
- Each build has a **required** set: `PROTECTED` in a protected build, empty
  otherwise. Its suite must equal the store's.
- The whole header, feature sets included, is under the MAC in a protected
  store, so nobody without the key can clear a bit. An unprotected store
  promises no integrity, and nothing here changes that.
- Protection is fixed at format. It changes every layout (bucket header,
  slot overhead, record size), and plaintext slots in a protected store are
  exactly the downgrade it exists to stop.

### 4.4 The mount check

```
store.incompat  ⊆ build.known            # else -ENOTSUP
store.ro_compat ⊆ build.known            # else -ENOTSUP (no read-only mount yet)
build.required  ⊆ store.incompat         # else -ENOTSUP: a protected build meets an unprotected store
store.suite    == build.suite            # else -ENOTSUP; in every ok header
store geometry == device geometry        # else -ENOTSUP; in every ok header
store.rec_size == build's R              # else -ENOTSUP; in every ok header
store.max_payload_len == build's cap     # else -ENOTSUP; in every ok header (D19)
store.write_align == device's            # else -ENOTSUP; in every ok header (D19)
```

The feature sets are the union over every master sector that classifies as
ok. Suite, geometry (the payload cap and `write_align` included), `hdr_len`
and `rec_size` must hold in each of them
(§3.1). Records carry no features, so the check runs on the headers alone, before
any record is read. Every refusal writes nothing.

### 4.5 Fuses: the format (phase 1)

Fuses are not features. A feature tells a build how to read the store; a fuse
only records that something happened to it: a CRC failure, a master update
that failed, a recovery at mount. They are post-mortem evidence for a store
returned from the field.

The **field** is part of the major-5 format from phase 1, and its rules are
fixed now. **Tripping** fuses, meaning which bits mean what, when they trip
and how a trip is persisted, is behaviour. It can come in a later phase
(§4.5a) without a format change. A phase-1 build trips nothing, but it keeps
every rule below, so the bits a later build sets survive it.

- **The field.** Record bytes 36..39, an opaque 32-bit word, for as long as
  the major is 5. A layout that moved it would be a new major, which older
  builds refuse.
- **Carried forward, OR-only.** Every record a build writes carries
  `previous | trips`, where `previous` is the word of the newest record that
  verified at mount, or of the record this session wrote last. A build that
  trips nothing writes `previous` unchanged. This covers every record:
  updates, rotation's record[0], and the
  write that ends a doubt. No build clears a bit, and no build rebuilds the
  word from the bits it knows: no mask of known bits exists anywhere. So a fuse
  defined after a build was released passes through that build unchanged.
- **No build can destroy a fuse it does not know.** The field is in major 5
  from phase 1, so every build that can write a major-5 record carries the
  word. A build that wrote those bytes as reserved zeros would clear every
  fuse. Older builds (majors 1–4) refuse a major-5 store and write nothing
  (D10).
- **Only a format clears them** (D12). `blob_db_format()`, the virgin format
  and an autoformat write record[0] with `fuses = 0`. Once trips exist, an
  autoformat may set a bit saying so (§4.5a). If evidence is wanted, read the
  fuses before a format.
- **Never a feature.** No read result, refusal, recovery or layout decision
  may depend on a fuse bit. A bit that changed how a store is read or written
  would be a format feature and would belong in `incompat` (§4.1). The only
  use of the stored word a later phase may make is to skip persisting a trip
  whose bit is already set (§4.5a).
- **Bit numbers are stable.** Bits 0..23 are for defined fuses: once assigned,
  a bit keeps its meaning forever, so a post-mortem can decode a store
  without knowing which build wrote it. Bits 24..31 are never assigned. They
  are for ad-hoc debugging (§4.5a). Phase 1 assigns no bit; the table in
  §4.5a is provisional until its phase.

**Trust.** In a protected store the record, `fuses` included, is under the
MAC, so nobody without the key can clear a bit. Replaying an older master
sector removes bits, but it also rolls back the ceilings, so it is the
rollback that PR #63's rollback anchor addresses, not a new hole. An
unprotected store promises no integrity, and its fuses are evidence only
against accidents.

### 4.5a Fuses: tripping (a later phase, provisional)

This section sketches the behaviour. It is not part of the format, and it can
change before its phase without touching any byte on flash.

- **Persisting.** A trip whose bit is already in the word is not new and
  leaves nothing pending, so a fault that every mount finds again is written
  once, not once per mount. A new trip ORs the bit into RAM and leaves it
  pending until a record carries it. Reads stay write-free: a CRC failure
  found by a read is written with the next record the store appends anyway.
  Pending fuses also count as maintenance work, so `blob_db_maintain()`
  appends one record for them, with no erase. Three cases write at once,
  because the evidence would otherwise be lost to a reboot:
  - **Mount** appends one record before returning when it tripped a new fuse.
    A refusal (`-ENOTSUP`, no usable master) writes nothing.
  - **Wedge.** Before refusing further writes, a wedged store makes one
    best-effort record append with the state unchanged. It never rotates:
    when the sector is full, or a record write has already failed (§3.3),
    it skips the append, because a rotation is an erase.
  - **A failed master write** is carried by the next record: record[0] of
    the rotation that follows a failed append (§3.3).

  A trip is lost only if power is cut before any record carries it, and since
  no code depends on fuses, that costs only that piece of evidence.
- **Exposed, not interpreted.** No call in `<app/lib/blob_db.h>` reads or sets
  fuses. With `CONFIG_BLOB_DB_INSPECT`, the inspection module returns the raw
  word and whether new fuses are still pending, and the shell prints it with
  bit names.
- **Registry.** The bit numbers live in `blob_db_internal.h`, and a
  `BUILD_ASSERT` checks that no two fuses share a bit. The names exist only
  for printing.
- **Debug fuses.** A developer puts `fuse_trip(BLOB_DB_FUSE_DEBUG(n))` where
  something is suspected, runs the build, and reads the fuses back later. The
  registry gives bits 24..31 no names and no trip points, so a merged build
  never trips them.
- **Torn versus rot** (open item 8). The provisional bits below separate a
  power cut from damage by a slot's position, and that does not hold. A slot
  whose header landed but whose payload tore stops being the last slot after
  the next append. A torn tail with unsealed flags fails no CRC at all. A
  torn rotation leaves a corrupt header. The bits must be settled before this
  phase, either by making the distinction real or by naming the bits for what
  was observed.

Provisional bits:

| Bit | Fuse | Trips when |
|---|---|---|
| 0 | `SLOT_ROT` | a slot fails its CRC and is not the last slot of its bucket |
| 1 | `SLOT_TORN` | the last slot of a bucket fails its CRC (a power cut, or rot there) |
| 2 | `BUCKET_HDR_ROT` | a bucket header fails its CRC |
| 3 | `MASTER_HDR_ROT` | a master sector's header classifies as corrupt, on either sector |
| 4 | `MASTER_REC_ROT` | a record before the newest written one fails its CRC |
| 5 | `MASTER_REC_TORN` | the newest written record fails its CRC |
| 6 | `STORE_DAMAGE` | the backend reports damage it found on open (UBI: `blob_db_store_open_found_damage()`) |
| 7 | `MAC_FAIL` | protected: the CRC verifies but the MAC does not (tampering, not damage) |
| 8 | `IO_ERR` | a store read, write or erase returns an error, outside a master update |
| 9 | `MASTER_WRITE_FAIL` | a record append or a rotation returns an error |
| 10 | `WEDGED` | the store wedges inside compaction's atomic window |
| 11 | `MASTER_DOUBT` | mount scans the buckets for the id ceiling (§3.5) |
| 12 | `ROTATION_FALLBACK` | the active sector holds no record that verifies; mount uses the other |
| 13 | `COMPACT_RECOVERED` | mount finds a sealed scratch and restores a bucket |
| 14 | `SEG_SWEEP` | mount finds `seg_owner` set and sweeps (an interrupted large write) |
| 15 | `AUTOFORMATTED` | set in record[0] by an autoformat |
| 16 | `FOREIGN_SLOT` | a walk stops at a sealed, CRC-intact slot carrying a flag the build does not know (§4.2) |
| 17..23 | reserved | for later fuses; copied like every other bit |
| 24..31 | debug, undefined | never assigned; free for ad-hoc debugging; copied like every other bit |

## 5. Large payloads

### 5.1 The window as two records (decision)

§5.6a of `l1_bucketlog.md` is unchanged except for steps 1 and 5: "master:
`seg_owner = id`" and "master: `seg_owner = 0`" each become a record append.
The mount sweep, its single rule (an unlisted segment is garbage) and the
`blob_db_test_cut` cut points stay as they are. The window costs two
programs of R bytes instead of two sector erases.

### 5.2 Reclaiming orphans in compaction (not proposed now)

The marker exists only to tell mount when to sweep. Compaction could apply the
same rule as it rebuilds a bucket, and the marker could then live in RAM.
That saves the two record programs. It also brings three problems:

1. A segment append that fills its bucket compacts it, in the middle of the
   write that owns the new, still unlisted segments. Compaction would have to
   skip the owner currently being written.
2. Checking a segment means reading its owner's index. In plaintext that is
   one 8-byte read at `seg_id[seq]`, since the segment header carries `seq`.
   In AEAD the index is encrypted, so opening it needs a `MAX_PAYLOAD_LEN`
   buffer while compaction holds both sector buffers (P3).
3. Orphans from a crash would take space until their bucket next compacts.
   P7 allows that, since the recovery is bounded, but it is space the sweep
   reclaims at the next mount today.

§5.1 already removes the erases, so this is kept as open item 1.

## 6. Compaction

| Path | Today | Proposed |
|---|---|---|
| UBI with atomic replace (the default) | no master write | no master write |
| scratch path (`flash_area`; UBI with `ATOMIC_REPLACE=n`) | 2 sector replaces (COMPACTING, CLEAN) | no master write |
| recovery at mount | 1 sector replace | no master write; a 16 B seal read at every mount |
| AEAD, either path (PR #63) | a master write when the `peb_size` reservation passes the ceiling: nearly every compaction on 64 KB blocks | none: random prefix (below) |

### 6.1 Recovery keyed on the seal (D17)

The scratch path keeps `l1_bucketlog.md` §5.6's steps except the two master
writes: erase scratch, write the image, seal it, erase the bucket, write the
image back, erase scratch. The seal is written after the image and erased with
the scratch sector, so **a valid seal exists only inside the window where the
bucket may be erased or half written, and the sealed image is the bucket's
complete new content**:

- Between the seal and the bucket's erase, the bucket is intact, and restoring
  writes the same image it was about to get.
- After the bucket's erase, restoring is the only way back.
- After the bucket is rewritten and before scratch is erased, restoring
  writes the same bytes again. Compaction runs inside one call, so nothing
  was appended in between, and a wedged store refuses writes until remount.
- A tear anywhere in the image or the seal fails the seal's CRCs, and an
  unsealed scratch means the bucket was never touched.

So every mount reads the seal (16 B), whatever the master says and whatever
the backend. A valid seal restores the bucket named by the image's own bucket
header, which its CRC covers, and erases scratch, as `recover_compaction()`
does today. Nothing else is needed: the master has no compaction state to
lose. The check runs on UBI with atomic replace too, because atomic replace is
a build option the format does not record. A store compacted by a build with
`ATOMIC_REPLACE=n` can carry a sealed scratch into an atomic build. An unsealed
scratch with content is left as it is, and the next compaction erases it
first. Recovery runs before the doubt scan, so the scan sees the restored
bucket.

This closes a hole that exists today. A rotten newest master that hid
`COMPACTING` made mount skip recovery, and the bucket lost every slot past
the cut.

### 6.2 AEAD compacted slots

**AEAD compacted slots** (PR #63 §3.2, §3.4). Each compaction draws 12 random
bytes into the bucket header's 12-byte `cbase` field (bytes 12..23, which
PR #63 split into `cbase_seq` and `cbase_salt`), and sets bit 7 of its byte 0.
Both nonce forms are 12 bytes, `N[0..11]`, and bit 7 of `N[0]` tells them
apart:

```
appended slot    N = mount_salt(4) ‖ LE64(seq)               N[0] bit 7 = 0
compacted slot   N = BE96(2⁹⁵ | ((c + offset) mod 2⁹⁵))      N[0] bit 7 = 1
                 where c = BE96⁻¹(cbase), offset = the slot's byte offset in the bucket
```

- **One bit, named in the nonce.** `mount_salt` is drawn with bit 7 of its
  byte 0 cleared, which leaves it 31 random bits. `cbase` is read as a
  big-endian 96-bit integer, so its top bit is that same `N[0]` bit 7, and the
  addition runs in the low 95 bits and never carries into it. No appended
  nonce can equal a compacted one, whatever `seq` reaches. A bit named as the
  "top bit" of two fields read as little-endian integers would sit in nonce
  byte 3 for one form and byte 7 for the other, and byte 7 of an appended
  nonce is bit 31 of `seq`.
- **Collisions.** A compaction uses the nonce range `[c, c + peb_size)`. Two
  compactions repeat a nonce only if their ranges overlap, that is
  `|c₁ − c₂| < peb_size` mod 2⁹⁵: about `2 · peb_size / 2⁹⁵` per pair, 2⁻⁷⁸ on
  64 KB and 2⁻⁸² on 4 KB. Over n compactions under one key that is about
  `n² · peb_size / 2⁹⁵`. At 10⁷ compactions, more than a device is expected
  to make, that is 2⁻³²·⁵ on 64 KB and 2⁻³⁶·⁵ on 4 KB, within the usual total
  budget of 2⁻³² for random nonces. Under CCM, a repeated nonce would leak
  the XOR of two plaintexts and weaken their tags, so the budget is not
  optional.
- **A never-compacted bucket** carries `cbase = 0`. Its bit 7 is clear, so it
  is never a valid base, and a `COMPACTED` slot under such a header fails to
  open: tampering, not a misread.
- **Entropy failure.** Compaction draws `cbase` from `psa_generate_random()`
  before it writes anything. If that fails, compaction returns the error,
  writes nothing and leaves the bucket as it was, and the write that needed
  the compaction returns the error too. There is no fallback to a fixed,
  derived or earlier prefix. Recovery at mount restores a sealed scratch image
  that already carries its `cbase`, so it needs no entropy.
- Entropy is needed once per compaction, not per write. PR #63 rejected
  random nonces because they would have needed entropy per write.
- `seq_hint` then covers appended slots only. Every record raises it (D14),
  and an append needs a record only when `seq` reaches the ceiling.
- All 12 bytes of `cbase` are random, and none is left reserved: this
  section's earlier 8-byte prefix had left 4. The key epoch keeps
  `slot_hdr.reserved` (§4.2).

## 7. Crash analysis (P7)

| Crash during | Mount sees | Outcome |
|---|---|---|
| a record append | the last record torn (CRC fails) or still erased | the previous record stands. The update was not acknowledged, so no ceiling or feature it carried was used. Doubt, so the bucket scan raises the id ceiling as today |
| a rotation, before the header lands | the other sector erased or corrupt; the full sector ok | the full sector's newest record stands; doubt (§3.5: an erased other sector is doubt, D18), scan. The doubt-ending write rotates again |
| a format, between its two sector writes (§3.4a) | B ok, A erased | B's record stands; doubt, a scan of buckets the format has already emptied, then a rotation into A |
| a format, inside B's write | no usable master, A erased | `-EIO`, nothing written, as a torn first master today; `blob_db_format()` or an autoformat starts again |
| `erase_all`'s record (§3.3) | the old record, or a torn one | the old, higher ceiling stands, or doubt and a scan; never a ceiling below an id on flash |
| a rotation, after the header, during record[0] | the new sector's header ok, no record verifies | that sector is unusable; fall back to the other; doubt, scan |
| the write that ends a doubt (§3.5) | a torn record, or a torn rotation | doubt again: the next mount scans and writes again. The ceiling stays safe, because the write that would have ended the doubt never landed |
| an upgrade's first rotation | as a rotation (rows above); if it landed, one ok sector has the new bit and the other lacks it | the store does not use the feature yet. A build that knows the bit finishes the upgrade with the second rotation. One that does not know it refuses, because §4.4 takes the union of the ok headers |
| an upgrade's second rotation | as a rotation | the same; both headers carry the bit once it lands |
| a record that carries a new fuse (once fuses trip, §4.5a) | the bit absent | only that piece of evidence is lost: no code depends on a fuse. A mount-time trip trips again at the next mount, since what it found is still on flash |
| `seg_owner` set / clear record | the old value | as `BLOB_DB_CUT_AFTER_OWNER_SET` / `_AFTER_RELEASE` today: an unset owner means no segment was written yet, a still-set owner means the sweep runs again (it is idempotent) |
| a scratch-path compaction, at any step | a sealed scratch, or none | sealed: the bucket is restored from the image (§6.1). Unsealed: the bucket was never touched. The master is not involved |
| a `seg_owner` record that rots after a cut inside the window | the previous record, `seg_owner` of that or 0, doubt | **accepted residual**, as today: no sweep runs, so the cut write's orphan segments stay as live slots (open item 9) |

A record write that fails without a power cut may leave its slot programmed,
partly programmed, or still erased. The cursor moves past it, and the next
update rotates instead of writing the next slot, so no erased slot ever lies
below a written one (§3.2a). A wedged store makes no next update, so its
failed best-effort append is at most the last slot, like a torn one. A failed
rotation leaves the active sector as it was; the next update tries it again.

## 8. Writing over programmed bits

Flash can clear bits (1 → 0) without an erase, and it is tempting to use that
for a counter or a "done" flag cleared in place. This design does not, and
every write programs erased bytes only:

- **STM32F3** (`nucleo_f302r8` among this repo's boards) accepts a write to a
  programmed half-word only when the new value is `0x0000`.
- **nRF internal flash** limits how many times one word may be written
  between erases (n_WRITE in the product specification).
- **Flash with ECC** accepts no second write at all. zephyr-ubi's
  `UBI_ERASE_INVALIDATES_HEADERS` needs exactly that and falls back without
  it.
- **An attacker can clear bits too.** In a protected store a bit cleared after
  the MAC was computed is not authenticated, so it could only ever mean
  "trust less", never assert state.

The log gets the same saving, fewer erases, from erased space alone. Clearing
in place could come later as an optional backend capability, never as
something the format depends on.

## 9. Cost (estimates from the measured counts)

Baseline from `app_perf_blob_db/RESULTS.md` (nRF5340-DK, high-performance
QSPI, about 0.53 s per 64 KB erase):

| Workload | Today | Proposed |
|---|---|---|
| `lg pwrite` (32 ops) | 64 master writes = 64 erases, 1.02–1.10 s/op | 64 record programs, 0 erases (one rotation per ~680 ops); the erases were nearly all of the per-op time |
| churn on `flash_area`, 1 000 updates | 40 erases = 16 master + 8 compactions × 3 | 24 erases, no master write |
| churn on UBI, atomic replace | 8 erases, 0 master | unchanged |
| AEAD compaction on 64 KB blocks (PR #63) | about +1 erase per compaction (not measured) | 0 |
| id ceiling | 1 erase per 256 new ids | at most 1 record program per 256 new ids, and none when another record carried the ceiling forward within the step (D14) |
| mount | 2 header reads | 2 header reads + a binary search of the active log (7 record reads on 4 KB, 11 on 64 KB) + 1 to verify the newest |
| RAM | — | about 24 B: the cursor as a record index (2 B; at most 1 362 records on 64 KB, 2 728 on 128 KB), the active sector's `rot_gen` (4 B), the header's three feature sets (12 B), copied verbatim at rotation, a failed-write flag, and the fuse word (4 B); once fuses trip, a pending flag |
| a failed record write | — | the next update rotates: 1 erase, on a path that should not happen |
| a doubtful mount | a scan at every mount until the next master write | one scan, then 1 record, or 1 rotation (1 erase) when the other sector is bad; the next mount does not scan |
| a fuse (once fuses trip, §4.5a) | — | 1 record program the first time it trips, at mount or with the next record or `blob_db_maintain()`; 0 erases, except a rotation when the sector is full |
| flash wear on the master sectors | 1 erase per update | 1 erase per 62–1 362 updates |
| a format | 1 master sector written | both master sectors written (D18): one more sector write, no more erases |

## 10. Code and documentation impact

| Where | Change |
|---|---|
| `lib/blob_db/blob_db_internal.h` | major-5 header and record structs; the feature and slot-flag registry; the fuse registry |
| `lib/blob_db/blob_db.c` | `write_master()` becomes `master_update()` and `rotate()`; mount's master classification, log search, feature check and doubt rule; features written at format, the upgrade's two rotations and finishing a half-done upgrade at mount; every format writes both master sectors, and an erased other sector is doubt (D18); the payload cap and `write_align` written at format and checked at mount (D19); segmented writes gated on the store's `LARGE_PAYLOADS` bit, not on Kconfig (D20); a sealed, intact slot with an unknown flag refuses its bucket instead of ending its log as torn, and compaction refuses to drop programmed bytes beyond one slot span past its walk (D13); `persist_seg_owner()` and `persist_next_id_hint()` append records, every record raises both ceilings, and `erase_all`'s record resets the id ceiling (D14); compaction writes no master, and `recover_compaction()` runs on a valid scratch seal at every mount, taking the bucket from the image (D17); every record carries the fuse word forward and a format writes 0 (phase 1); later, `fuse_trip()` at each trip point, the mount and wedge appends, and pending fuses as `blob_db_maintain()` work (§4.5a) |
| PR #63 | master MAC and tail move into the header and record layouts above; `cbase` becomes a 12-byte random base, and the compacted nonce becomes base + offset (§6); `seq_reserve()` is no longer called by compaction |
| PR #62 | `DELTA` moves to slot flag bit 5 and is set at format |
| `include/app/lib/blob_db_inspect.h` (public) | `struct blob_db_inspect_info`: `master_gen` becomes `rot_gen`; `format_minor` stays, always 0 (D15); it gains the three feature sets, `suite`, `rec_size`, the log's fill (records used and per sector) and the raw fuse word |
| `lib/blob_db/blob_db_inspect.c`, `blob_db_shell.c` | fill the fields above; `blob_db info` prints them and no longer prints a master state (D17); once fuses trip, whether new fuses are pending and the bit names (§4.5a); `struct blob_db_core_state` gains the same fields |
| `include/app/lib/blob_db.h` (public) | phase 1: no change. `blob_db_maintain()` keeps its contract (erases done ahead, at most one per step, nothing read back changes). The fuse phase changes it: a step may append a master record for pending fuses, and that append may rotate (§4.5a), so the docstring must say so then |
| `tests/lib/blob_db/src/main.c` | the tests that forge master images by hand (`write_master`, `COMPACTING`, `format_major`, `master_gen`) become major-5 header and record images (test 5b), and the forged-`COMPACTING` crash tests become forged sealed-scratch tests (test 1a) |
| `doc/impl/l1_bucketlog.md` | §3.1, §5.1, §5.6, §5.6a, §6 and §13.1 rewritten for the log; §1.5's cost table loses compaction's master writes; §11 item 13 becomes a sealed-scratch recovery test |
| `kvdb_book` (other repo) | `tools/blob-db-bucket-log-explorer/REQUIREMENTS.md`: the master view (`gen`, `CLEAN` / `COMPACTING(b)`) becomes header and log (`rot_gen`, features, suite, records), and the compaction sequence loses its two master steps. `tools/migration-paths/REQUIREMENTS.md`: no minor inside a major (D15), and protection is a `suite` chosen at format, not a major of its own |

## 11. Test plan (P8; `native_sim`, both backends)

1. **Log round trips**: every suite passes on major 5; iostats show 0 master
   erases for `lg pwrite`, and no master write at all for compaction on
   either path.
1a. **Seal recovery** (D17): a cut at every step of a scratch-path compaction
    leaves either the old bucket or the new one, with the master untouched.
    A sealed scratch restores the bucket named in the image, whatever the
    newest record says, including when that record is rotten. An atomic-
    replace build mounting a store left with a sealed scratch by an
    `ATOMIC_REPLACE=n` build restores it too. An unsealed scratch leaves the
    bucket as it was.
2. **Power cut** at every byte of a record append and of a rotation (header
   and record[0]): mount sees the old state or the new one, never another, and
   an id or nonce value is never issued twice.
2a. **Ceilings ride along** (D14): a run of segmented writes, which append
    `seg_owner` records anyway, allocates many ids and writes no ceiling-only
    record (iostats). Every record's ceiling is at least the one before it.
    After a cut at any point, mount's `next_id` is at most `STEP + 1` above the
    last id handed out, and never at or below it. **`erase_all`**: a cut at
    every byte of its record leaves the old ceiling or `2 + 1 + STEP`, never
    another value, and after the remount no id handed out equals one still on
    flash. `seq_hint` is unchanged by it.
3. **The log end**: a torn last record, an erased-but-interrupted slot, and a
   rotten record in the middle give the newest committed state. **The
   invariant** (§3.2a): a record write that fails having programmed nothing
   is followed by a rotation, not by a record in the next slot. After every
   test in the suite, every sector's record slots are a written prefix
   followed by erased slots. A full read of every slot, kept in the test
   suite as a reference reader, agrees with mount's binary search at every
   fill level, and mount reads less than a sector. A rotten last
   record, and a corrupt other sector, make mount scan.
3a. **Ending the doubt**: for each doubt condition (a corrupt other sector,
    an erased other sector, a rotten last record, a fallback after a torn
    rotation, UBI damage on a master sector), mount scans once and writes once: a rotation or a
    record, as §3.5 says. The next ten mounts do not scan (iostats). The same
    holds once fuses trip, with `MASTER_DOUBT` already set. On UBI
    with atomic replace, a workload that only rewrites existing ids still
    stops scanning after one mount. A cut at every point of the scan and of
    that write leaves the next mount scanning again, with a ceiling above
    every id on flash. **The torn doubt-ending rotation** (D18): rot the active
    sector's header after ids were bound under its ceilings, mount, and cut
    the doubt-ending rotation right after its erase. The next mount doubts on
    the erased sector, scans, and hands out no id that is on flash.
3c. **Format writes both sectors** (D18): after `blob_db_format()`, the
    virgin format and an autoformat, both master sectors are ok with
    `rot_gen` 1 and 2 and the same record[0], and the next mount does not
    scan. A cut at every byte of the format leaves either a store that mounts
    (with a scan when only B landed) or `-EIO` with nothing written.
3b. **Usable sectors**: a header-only sector (a rotation torn after its
    header) above a usable one makes mount take `cursor`, `state` and `fuses`
    from the lower sector, doubt, and rotate into the header-only one. The
    same with every record of the higher sector rotten. Two header-only
    sectors, and one header-only sector beside an erased one (a first format
    torn after its header), return `-EIO` and write nothing. With
    `AUTOFORMAT_ON_CORRUPT` they are erased. A header-only sector from a newer
    build, carrying an unknown `incompat` bit, refuses the store.
4. **Rotation**: fill a sector, rotate, remount; the new header carries the
   old header's feature sets verbatim, bits the build does not know included.
5. **Features**: an unknown `incompat` or `ro_compat` bit refuses with
   `-ENOTSUP` and writes nothing, whichever ok sector carries it. A store
   formatted with large payloads is refused by a build without them, even if
   no segmented write ever happened. The same for `DELTA`. **Fallback keeps
   features**: in a store with segmented objects, corrupt the active sector's
   header. Mount falls back to the other sector and still has
   `LARGE_PAYLOADS`, and every object reads back intact. On UBI, also when the
   active block's older copy is stood up. **Upgrade**: a cut before, between
   and after its two rotations. Mount either finishes it or leaves the store
   as it was, no segmented write happens until both headers carry the bit, and
   a build without the feature refuses the store from the first rotation on.
   **Writing only the store's features** (D20): a `LARGE_PAYLOADS` build on a
   store formatted without the bit returns `-EINVAL` for `update()` and
   `write()` beyond the cap, and no INDEXED or SEGMENT slot reaches flash. A
   build without the feature then mounts the store and reads every object.
5a. **Foreign slot** (D13): forge a sealed slot with an intact CRC and an
    unknown flag into the middle of a bucket. Writes to that bucket return
    `-ENOTSUP`, reads `-EIO`, and nothing is compacted or erased. Once fuses
    trip, `FOREIGN_SLOT` is set. A torn slot at a bucket's tail still compacts as
    today, and so does a torn append whose span holds stray programmed bits
    anywhere, out of order (a page program cut midway). A compaction whose
    walk stops more than one slot span before programmed bytes refuses.
5b. **Format rules** (§3.0, §3.1, §3.2, §3.6):
    - **Golden images.** The suite keeps byte images of a major-5 header and
      record, one per suite, written out by hand from §3. The build must
      parse them, and must write exactly those bytes for the same state, so
      no build can drift from the format.
    - **Refusals.** A `rec_size` other than the build's is refused, and so is
      an ok other sector whose suite, geometry or `hdr_len` differs. So is a
      `max_payload_len` other than the build's cap, smaller or larger, and a
      `write_align` other than the device's (D19). A
      `write_align` above 128 is refused. Two ok sectors with equal `rot_gen` return `-EIO`.
      None of these writes anything.
    - **Binding and reserved bytes.** A record copied from an earlier
      rotation into a new sector fails its CRC. Non-zero reserved bytes and
      filler in a planted record still mount, the next record writes them as
      0 and `0xff`, and `format_minor` is written as 0. A protected header with
      a broken MAC falls back with doubt.
6. **Protection**: a protected build refuses an unprotected store, another
   suite, a cleared `PROTECTED` bit, and a forged record (MAC). A record
   copied from an earlier rotation or moved within the sector is rejected.
7. **Geometry**: a store mounted with another `n_buckets` or `peb_size` is
   refused. On UBI, a forged volume-table `leb_count` under the builtin UBI
   key is refused in a protected build (PR #63 §9 item 4, UBI half).
8. **AEAD nonces**: PR #63's recorder sees no nonce repeat across remounts and
   repeated compactions, and compaction writes no master on either path.
   Every recorded nonce has `N[0]` bit 7 clear for an appended slot and set for
   a compacted one, including with `seq` started above 2³¹. A forced `cbase`
   near 2⁹⁵ − 1 wraps within the low 95 bits and keeps the bit set. A
   `COMPACTED` slot under a header with `cbase = 0` fails to open. An injected
   `psa_generate_random()` failure makes compaction, and the write that
   needed it, return the error, with the bucket's bytes unchanged.
9. **Registry**: a `BUILD_ASSERT` that no two features own a slot flag.
10. **Fuse word** (phase 1, format): plant every one of the 32 bits. They
    pass through updates, rotations, the
    write that ends a doubt, a fallback to an earlier record and a fallback to
    the other sector, and only a format clears them. A phase-1 build changes no
    bit. Every suite run on a store whose fuses are all set gives the same
    results, data and iostats as on a store with none.
10a. **Fuse trips** (with §4.5a): each trip point by fault injection, the
    persistence rules (a read that trips writes nothing, `blob_db_maintain()`
    then appends one record, a mount that trips appends one, a refused mount
    writes nothing, the wedge's one append), and deduplication: a fault found
    again at every mount writes one record at the first mount and none at the
    next ten. With all 32 set, results and data are the same as with none,
    and iostats differ only by the master records the deduplication saves.

## 12. Phases

| Phase | Content |
|---|---|
| **1** | Major 5: header, records, rotation, mount search and doubt rule; format writing both sectors (D18); ceilings and `seg_owner` as records; compaction recovery keyed on the seal (D17); geometry, the payload cap and `write_align` included (D19); features with `LARGE_PAYLOADS`, written only into a store that has them (D20); the fuse field and its carry-forward rules (§4.5), with no trip points |
| **2** | PR #63 rebased onto it: `suite` and `PROTECTED`, header and record MACs, the random compaction base |
| **3** | PR #62's `DELTA` rebased onto it: slot flag bit 5, set at format |
| **later** | Fuse trips (§4.5a), after open item 8: no format change |

**Landing phase 1 on existing boards.** Major 5 refuses stores of majors 1–4
with `-ENOTSUP` (D10), and `CONFIG_BLOB_DB_AUTOFORMAT_ON_CORRUPT` does not
cover them, because a foreign store is not corrupt. Every development board,
and every saved `native_sim` flash file, that holds a store needs one explicit
`blob_db_format()` or a raw erase before the first major-5 mount. Twister runs
start from fresh flash and are not affected. Nothing runs this stack in
production, so no migration is written (D10).

## 13. Open items

1. **Orphan reclaim in compaction** (§5.2): it removes the last two record
   programs from a large write, at the cost of an in-flight exemption and,
   in AEAD, a payload-sized buffer during compaction.
2. **A read-only mount**, which `ro_compat` assumes. Until one exists,
   `ro_compat` behaves like `incompat`.
3. **`store_uuid` in unprotected builds**: useful to phase 2's rollback
   anchor as a store identity, but it needs an entropy source that an
   unprotected `flash_area` build does not otherwise pull in.
4. **Large `write_align`**: R is rounded up to it, so on flash with a 32 B
   write unit an unprotected record is 64 B. Nothing breaks; there are only
   fewer records per sector.
5. **The id ceiling step**: with records cheap, `BLOB_DB_ID_HINT_STEP`
   (configurable in PR #62) mostly trades ids skipped at mount against
   records. 64-bit ids make the skip irrelevant, so 256 can stay.
6. **Evidence across a format**: fuses that outlive a format need storage
   outside the partition (OTP, UICR, a reserved sector). That is out of scope
   here, and D12 keeps the fuses with the store.
7. **The upgrade call**: its name and form (`blob_db_feature_enable()`, or a
   Kconfig opt-in that upgrades at mount) and its phase. Phase 1 needs none,
   because enabling a feature on a development store can format it.
   §4.2 fixes what the call must do: two rotations, and no use of the feature
   before both have landed.
8. **Fuse bits** (§4.5a): which bits to define, and whether a power cut can be
   told apart from damage, by making the provisional slot and master bits
   exact or by naming them for what was observed. Behaviour only: the field's
   rules (§4.5) do not depend on the answer.
9. **Orphans after a rotten `seg_owner` record**: a cut inside a segmented
   write followed by rot of its `seg_owner` record leaves orphan segments that
   no sweep reclaims, as today (§7). A mount that doubts could check every
   SEGMENT slot its scan meets against its owner's index: about one bucket
   walk per segment, only on a doubtful mount. That is behaviour, not format.
