# Design change proposal — the master as a log, and format features

Status: **proposal** · 2026-10-28
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
and erases nothing, and a sector is erased once per 60 to 1 300 updates. The
header records the geometry and three feature sets (`incompat`, `ro_compat`,
`compat`) in place of the major-number combinations. Protection is a `suite`
the build requires, not a feature the store chooses. Compaction no longer
touches the master: on UBI it never did, and in AEAD mode its nonces stop
coming from the master counter.

---

## 1. Requirements

| # | Requirement | Consequence |
|---|---|---|
| M1 | A master update erases nothing in the common case | an append-only log of records inside the master sector (§3) |
| M2 | Compaction never writes the master on a backend with atomic replace | AEAD compaction nonces drawn at random, not reserved from the master counter (§6) |
| M3 | A large-payload write costs no erase beyond its slots | the sweep window is two log records (§5) |
| M4 | Crash safety as today | the same two commit primitives: a slot append and a master update; a torn record reads as no update (§7) |
| M5 | Every write programs erased bytes only | works on NOR, on flash with ECC and on UBI; no rewrite of programmed bits (§8) |
| M6 | A store says which features it uses; a build refuses what it does not understand | `incompat` / `ro_compat` / `compat` in the header (§4) |
| M7 | A protected build accepts only a store protected exactly as it expects | `suite` must equal the build's; the store never chooses protection (§4.3) |
| M8 | The geometry is part of the format | `peb_size` and `n_buckets` in the header, checked at mount (§3.1) |
| M9 | No new steady-state RAM | a log cursor and a record number (P3) |

## 2. Decisions

| # | Decision | Alternatives rejected |
|---|---|---|
| D1 | A header plus fixed-size records, each a **full copy** of the mutable state | delta records replayed in order (mount logic, and one rotten record would break the chain) |
| D2 | Keep two master sectors; a full sector rotates into the other | one sector (a torn erase would leave no master) |
| D3 | Find the end of the log by binary search on "erased" | a linear walk (up to 1 300 reads on 64 KB) |
| D4 | Large payloads keep the mount-time sweep, as two log records | reclaiming orphans in compaction (§5.2, kept as open item 1) |
| D5 | AEAD compacted slots take a random 63-bit nonce prefix per compaction | reserving `peb_size` values from the master counter (PR #63 §3.4) |
| D6 | Features in three sets, ext4-style; a slot flag belongs to exactly one feature | a major number per combination (doubles with every feature) |
| D7 | A feature that adds a slot flag is marked in the store on **first use** | marking at format (an older build would refuse a store that never used it) |
| D8 | Protection is a `suite` byte plus an `incompat` bit, and the build requires it | an ordinary feature the store declares (a forged unprotected master would be obeyed: a downgrade) |
| D9 | No writes over programmed bits | clearing bits in place (§8: STM32F3, nRF word-write limits, ECC flash; and an attacker can clear bits too) |
| D10 | Major 5 for the new layout; majors 1–4 are refused, never migrated | migration code for stores that exist only on development boards |

## 3. Master sector layout (major 5)

```
offset 0      header            written once, right after the erase
offset 128    record[0]         written with the header
offset 128+R  record[1] ...     appended, one per update
              erased to the end of the sector
```

128 is `BLOB_DB_MASTER_HDR_MAX`, the window every reader already stages, so a
reader of any age reads the frozen prefix from the same place.

### 3.1 Header

Nothing in it changes until the sector is erased again.

| Bytes | Field | Notes |
|---|---|---|
| 0..11 | frozen prefix | `magic` `BDMS`, `format_major = 5`, `format_minor`, `hdr_len` (64, or 84 protected), `reserved`, `prefix_crc16`; layout unchanged since major 1 |
| 12..15 | `rot_gen` | +1 per rotation; the sector with the higher valid value is active |
| 16..19 | `peb_size` | must equal the device's |
| 20..21 | `n_buckets` | must equal the device's |
| 22..23 | `rec_size` | R: 48, or 64 protected, rounded up to `write_align` |
| 24..27 | `incompat` | a reader must understand every bit (§4) |
| 28..31 | `ro_compat` | a writer must understand every bit |
| 32..35 | `compat` | anyone may ignore an unknown bit |
| 36 | `suite` | 0 unprotected; otherwise mode and tag length, as PR #63's master byte 19 |
| 37..39 | reserved, 0 | |
| 40..55 | `store_uuid` | drawn at format when protected (PR #63 §3.1); 0 otherwise |
| 56..59 | reserved, 0 | |
| 60..63 | `hdr_crc32` | over [0, 60) |
| 64..79 | `mac` | protected only: AES-CMAC-128(K_meta, [0, 64)) |
| 80..83 | `tail_crc32` | protected only: over [0, 80) |

A device whose geometry differs from the header's is refused with `-ENOTSUP`
and nothing is written. In a protected store this closes the gap PR #63 left
on UBI, where the bucket count came from the volume table and only UBI's MAC
protected it. In an unprotected store it catches a build configured for
another partition.

### 3.2 Record

| Bytes | Field | Notes |
|---|---|---|
| 0..3 | `rec_no` | equals the record's position in this sector, from 0 |
| 4 | `state` | `CLEAN` or `COMPACTING` |
| 5 | reserved, 0 | |
| 6..7 | `compacting_bid` | meaningful only when `COMPACTING` |
| 8..15 | `next_id_hint` | the id ceiling, as today (§13.1) |
| 16..23 | `seq_hint` | the AEAD nonce ceiling (PR #63 §3.2); 0 otherwise |
| 24..31 | `seg_owner` | the segmented write in flight; 0 = none |
| 32..35 | `incompat` | the header's bits, plus any marked since the rotation (§4.2) |
| 36..43 | reserved, 0 | |
| 44..47 | `crc32` | unprotected: over [0, 44); R = 48 |

A protected record puts a MAC where the unprotected CRC was:

| Bytes | Field |
|---|---|
| 44..59 | `mac`: AES-CMAC-128(K_meta, LE32(rot_gen) ‖ [0, 44)) |
| 60..63 | `crc32` over [0, 60); R = 64 |

`rot_gen` in the MAC and `rec_no` equal to the position mean a record cannot
be moved within its sector or copied from an earlier rotation. Check order,
as everywhere in PR #63: erased, then CRC (damage), then MAC (tampering).

Records per sector: (sector − 128) / R.

| Sector | Unprotected (48 B) | Protected (64 B) |
|---|--:|--:|
| 4 KB | 82 | 62 |
| 64 KB | 1 362 | 1 022 |

### 3.3 Update

```
master_update(state):
    if cursor + R > peb_size: rotate(state); return
    write record(rec_no = (cursor - 128) / R, state) at cursor
    cursor += R          # also on failure: that slot is no longer erased
```

The write programs only erased bytes, moving forward through the sector:
`blob_db_store_write()`, which on UBI is `ubi_leb_write_at()`, as slot appends
already are.

### 3.4 Rotation

```
rotate(state):
    other = !active
    replace(other, header(rot_gen + 1, features = header ∪ last record) ‖ record[0](state))
    active = other; cursor = 128 + R
```

`blob_db_store_replace()` is an erase and a write on `flash_area`, and
`ubi_leb_change()` on UBI with atomic replace. UBI checks its data checksum
over the replaced length only, so the appends that follow keep it valid,
exactly as slot appends after a bucket compaction do today. Rotation is
today's A/B master write, made once per sector's worth of updates.

### 3.5 Mount

```
read [0, 128) of both sectors; classify each as erased, corrupt, foreign or ok (§3.1 of l1_bucketlog.md)
foreign on either sector                               -> -ENOTSUP, write nothing
features, suite or geometry not acceptable (§4.4)      -> -ENOTSUP, write nothing
active = the ok sector with the higher rot_gen
cursor = first record slot that is fully erased        # binary search: slots are written in order
state  = the newest record before cursor that verifies
doubt  = the record just before cursor does not verify
      or no record in the active sector verifies       # a torn rotation: fall back to the other sector
      or the other sector is neither erased nor ok
      or the store reports damage on a master sector   # as today, for UBI
if doubt: scan the buckets and raise the id ceiling    # as today (§5.1)
if state == COMPACTING: recover (§6.1), then append a CLEAN record
```

"Doubt" carries today's rule over: a ceiling that might have been
acknowledged and then lost makes mount scan. A torn last record was never
acknowledged, but bit rot in the last committed record looks the same, so
both trigger the scan. A rotten record further back is harmless, because a
later full-state record replaces it.

Cost: two header reads as today, plus about log₂(records) reads of R bytes:
7 on 4 KB, 11 on 64 KB.

## 4. Features

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

| `incompat` bit | Feature | Slot flags it owns | Marked |
|---|---|---|---|
| 0 | `LARGE_PAYLOADS` | bit 2 `INDEXED`, bit 3 `SEGMENT` | on the first segmented write |
| 1 | `PROTECTED` | bit 4 `COMPACTED` (AEAD suites) | at format |
| 2 | `DELTA` (PR #62) | bit 5 `DELTA` (moved from bit 4) | on the first delta slot |

`SEALED` (bit 0) and `TOMBSTONE` (bit 1) belong to every store. Bits 6 and 7
are free, and `slot_hdr.reserved` stays kept for a key epoch (PR #63 §11.4).
The slot walk takes the flags it accepts from the store's features, not from
`#ifdef`s: `known = SEALED | TOMBSTONE | flags of each feature set in the store`.
A `BUILD_ASSERT` checks that no two features own the same bit.

**Marking on first use.** A build with `LARGE_PAYLOADS` writes a store that an
older build can still mount, until the first segmented write. That write is
preceded by a record whose `incompat` adds the bit, and the record must be on
flash before the first slot that uses the feature. This is the same rule as
the ceilings: persist first, then use. A rotation folds the record's bits into
the new header. Bits are never cleared, except by a format.

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
store.suite    == build.suite            # else -ENOTSUP
store geometry == device geometry        # else -ENOTSUP
```

`store.incompat` is the union of the header and the newest record that
verifies. Every refusal writes nothing.

### 4.5 Atomic replace is a store property (to resolve)

`CONFIG_BLOB_DB_UBI_ATOMIC_REPLACE` (default `y` since PR #51) looks like write
policy, which §4.1 keeps out of the feature sets. It is not. A block written
by `ubi_leb_change()` carries UBI's seal: its VID header holds `data_size` and
`data_crc` over the replaced length, and the block it replaced waits in UBI's
reclaim queue. Appends past that length are fine, and §3.4 relies on them.
Bytes inside it must never change again.

A build with the option off breaks that rule in one place: `invalidate_buckets()`,
called by `blob_db_erase_all()`, zeroes each bucket's `BDBH` magic in place.
On a bucket last written by `ubi_leb_change()`:

1. the newest copy no longer matches its seal, so the next attach reports
   `UBI_EVENT_DATA_CORRUPT` and drops it;
2. if the block it replaced has not been reclaimed yet, UBI maps that one
   back, and the blobs `erase_all` deleted are live again.

`doc/impl/l0_backends.md` §3 only says that a build that disables the option
has to format the store first. Nothing on flash records that the store was
written that way, so nothing enforces it. The in-place zero is also the one
write in blob_db that programs over programmed bits, which §8 and D9 rule out
for the new format.

| | A. `ro_compat` bit `SEALED_REPLACE` | B. wipe generation |
|---|---|---|
| How | a record marks the bit before the first replace that `ubi_leb_change()` serves (persist first, then use, §4.2) | records carry `wipe_gen`; each bucket header carries the `wipe_gen` it was formatted under; a bucket whose value differs from the store's reads as unformatted |
| `erase_all` | unchanged: a replace per formatted bucket on UBI, an in-place zero on `flash_area` | one record append (`wipe_gen + 1`), then the root rebind; no bucket is written |
| Option turned off on a store that used it | refused with `-ENOTSUP` (no read-only mount yet, open item 2) | safe: no in-place write is left, so the option is write policy again and stays out of the feature sets |
| §8 / D9 | still broken on `flash_area` | met on every backend |
| Format change | one registry bit | 4 B in the record and in the bucket header. Major 5 changes both anyway, and `reserved[2]` is too small to rule out a wrap |
| Crash | as for any feature bit (§7) | a torn `wipe_gen` record is an `erase_all` not acknowledged, and the old buckets stand. Once it lands, every older bucket is gone at once, so `erase_all` becomes crash-atomic, which it is not today |
| Mount and read cost | none | a compare on the bucket header every reader already checks |

Still open within B:

- With both masters blank there is no `wipe_gen` to compare against, so the
  format path (`l1_bucketlog.md` §3.1) still has to clear what is present. It
  can erase (`ubi_leb_erase()` destroys every copy), since formatting is rare.
  Or the bucket header could carry `store_uuid`, which brings back open item 3.
- Compaction copies the bucket's `wipe_gen` into the new image.
- A stale bucket keeps its bytes on flash until it is formatted again, as a
  zeroed bucket does today.
- In a protected store `wipe_gen` is under the record and bucket header MACs.
  Rolling the master back to an older `wipe_gen` would bring buckets back,
  which is the rollback PR #63's anchor already has to cover.

Leaning: B in phase 1, since it also removes the last in-place write and the
per-bucket cost of `erase_all`. If B is deferred, A should land in phase 1
instead, so that a build with the option off refuses the store rather than
reviving deleted blobs.

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
| scratch path (`flash_area`; UBI with `ATOMIC_REPLACE=n`) | 2 sector replaces (COMPACTING, CLEAN) | 2 record appends |
| recovery at mount | 1 sector replace | 1 record append |
| AEAD, either path (PR #63) | a master write when the `peb_size` reservation passes the ceiling: nearly every compaction on 64 KB blocks | none: random prefix (below) |

**AEAD compacted slots** (PR #63 §3.2, §3.4). Compaction draws 8 random bytes
per compaction and stores them in the bucket header's existing 12-byte
`cbase` field, with the last 4 bytes reserved:

```
appended slot    nonce = mount_salt(4) ‖ LE64(seq)        top bit of mount_salt cleared
compacted slot   nonce = cbase(8)      ‖ LE32(offset)     top bit of cbase set
```

- The top bit keeps the two nonce forms apart under one key. The mount salt
  keeps 31 random bits and the compaction prefix 63.
- Two compactions collide only if they draw the same prefix: 2⁻⁶³ per pair,
  about 2⁻²⁴ over a million compactions. The appended form already accepts
  2⁻³² after a master rollback.
- Entropy is needed once per compaction, not per write. PR #63 rejected
  random nonces because they would have needed entropy per write.
- `seq_hint` then covers appended slots only: one record per
  `BLOB_DB_SEQ_STEP` appends.

## 7. Crash analysis (P7)

| Crash during | Mount sees | Outcome |
|---|---|---|
| a record append | the last record torn (CRC fails) or still erased | the previous record stands. The update was not acknowledged, so no ceiling or feature it carried was used. Doubt, so the bucket scan raises the id ceiling as today |
| a rotation, before the header lands | the other sector erased or corrupt; the full sector ok | the full sector's newest record stands; doubt, scan. The next update rotates again |
| a rotation, after the header, during record[0] | the new sector's header ok, no record verifies | that sector is unusable; fall back to the other; doubt, scan |
| a feature-marking record | the bit absent | no slot uses the feature yet (persist before use), so the store is still what an older build can read |
| `seg_owner` set / clear record | the old value | as `BLOB_DB_CUT_AFTER_OWNER_SET` / `_AFTER_RELEASE` today: an unset owner means no segment was written yet, a still-set owner means the sweep runs again (it is idempotent) |
| COMPACTING / CLEAN record (scratch path) | as the corresponding master write today | `l1_bucketlog.md` §6.1 unchanged |

A record write that fails without a power cut leaves its slot non-erased. The
cursor moves past it, and the next update writes the next slot.

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
| churn on `flash_area`, 1 000 updates | 40 erases = 16 master + 8 compactions × 3 | 24 erases + 16 record programs |
| churn on UBI, atomic replace | 8 erases, 0 master | unchanged |
| AEAD compaction on 64 KB blocks (PR #63) | about +1 erase per compaction (not measured) | 0 |
| id ceiling | 1 erase per 256 new ids | 1 record program per 256 new ids |
| `erase_all`, UBI with atomic replace, 200 of 2 038 buckets formatted (`native_sim`, `l0_backends.md` §3) | 13.8 KB written, ~200 blocks left to reclaim | with §4.5 option B: 1 record program + the root rebind |
| mount | 2 header reads | 2 header reads + ~7–11 record reads |
| RAM | — | a cursor (2 B) and the active sector's `rot_gen` |
| flash wear on the master sectors | 1 erase per update | 1 erase per 62–1 362 updates |

## 10. Code and documentation impact

| Where | Change |
|---|---|
| `lib/blob_db/blob_db_internal.h` | major-5 header and record structs; the feature and slot-flag registry |
| `lib/blob_db/blob_db.c` | `write_master()` becomes `master_update()` and `rotate()`; mount's master classification, log search, feature check and doubt rule; slot walk takes `known` from the store; `persist_seg_owner()`, `persist_next_id_hint()`, compaction and recovery append records |
| PR #63 | master MAC and tail move into the header and record layouts above; `cbase` becomes a random prefix; `seq_reserve()` is no longer called by compaction |
| PR #62 | `DELTA` moves to slot flag bit 5 and is marked on first use |
| `lib/blob_db/blob_db_inspect.c`, shell | show features, suite, geometry, log fill and rotation generation |
| `doc/impl/l1_bucketlog.md` | §3.1, §5.1, §5.6, §5.6a, §6 and §13.1 rewritten for the log |
| `doc/impl/l0_backends.md` | §3: the shortcomings of atomic replace that §4.5 resolves are struck |

## 11. Test plan (P8; `native_sim`, both backends)

1. **Log round trips**: every suite passes on major 5; iostats show 0 master
   erases for `lg pwrite` and for compaction on the atomic path.
2. **Power cut** at every byte of a record append and of a rotation (header
   and record[0]): mount sees the old state or the new one, never another, and
   an id or nonce value is never issued twice.
3. **The log end**: a torn last record, an erased-but-interrupted slot, and a
   rotten record in the middle give the newest committed state. A rotten last
   record, and a corrupt other sector, make mount scan.
4. **Rotation**: fill a sector, rotate, remount; features marked before the
   rotation are in the new header.
5. **Features**: an unknown `incompat` or `ro_compat` bit refuses with
   `-ENOTSUP` and writes nothing. A store with large payloads enabled but
   unused mounts in a build without them; after the first segmented write it
   is refused. The same for `DELTA`.
6. **Protection**: a protected build refuses an unprotected store, another
   suite, a cleared `PROTECTED` bit, and a forged record (MAC). A record
   copied from an earlier rotation or moved within the sector is rejected.
7. **Geometry**: a store mounted with another `n_buckets` or `peb_size` is
   refused. On UBI, a forged volume-table `leb_count` under the builtin UBI
   key is refused in a protected build (PR #63 §9 item 4, UBI half).
8. **AEAD nonces**: PR #63's recorder sees no nonce repeat across remounts and
   repeated compactions, and compaction writes no master on either path.
9. **Registry**: a `BUILD_ASSERT` that no two features own a slot flag.
10. **Atomic replace turned off** (§4.5): on UBI, write and compact with the
    option on, `erase_all` with it off, remount before reclaim runs. With A
    the second build refuses the store; with B no deleted blob comes back. A
    power cut at every byte of the `wipe_gen` record leaves all buckets or
    none.

## 12. Phases

| Phase | Content |
|---|---|
| **1** | Major 5: header, records, rotation, mount search and doubt rule; ceilings, `seg_owner` and compaction state as records; geometry; features with `LARGE_PAYLOADS` |
| **2** | PR #63 rebased onto it: `suite` and `PROTECTED`, header and record MACs, the random compaction prefix |
| **3** | PR #62's `DELTA` rebased onto it: slot flag bit 5, marked on first use |

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
6. **Atomic replace as a store property** (§4.5): choose between an
   `ro_compat` bit that refuses a build with the option off, and a wipe
   generation that makes `erase_all` one record and removes the last in-place
   write. Either one belongs in phase 1.
