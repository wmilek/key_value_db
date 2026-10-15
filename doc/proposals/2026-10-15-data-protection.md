# Design exploration — encrypting and authenticating data in `blob_db`

Status: **exploration** · 2026-10-15 · nothing implemented
· Target contract: `doc/layers/l1_blob_db.md` (§2 stability contract: one new error meaning; §3 cost bounds)
· Target implementation design: `doc/impl/l1_bucketlog.md` (§3.3 slot format, §3.1 master body, §5.4 read path)
· Related: `doc/impl/l0_backends.md` ("Authenticated metadata"), zephyr-ubi `docs/security.md`
· New dependency: none beyond what the UBI backend already selects (`PSA_CRYPTO`), but now needed on both backends
· Governed by `doc/principles.md`

**The ask.** Application data is stored in the clear and unauthenticated on
both backends. An earlier attempt to add encryption and validation in UBI
failed on performance: UBI does not know which part of a block is in use, so
sealing the whole block made appends into it impossible. The suggestion is to
move protection up to `blob_db`, possibly as a seal over a whole bucket. This
document looks at that and the alternatives.

**Short answer.** Protect each **slot** with an AEAD, not each bucket. The
slot is already the unit `blob_db` writes atomically and the unit a `get`
verifies, so a tag per slot costs one AEAD call per read or write. Appends,
the header walk, the slot index and compaction all stay as they are. The slot
header and id stay in clear text, because the walk reads only those 12 bytes
per slot. They are authenticated as associated data when the slot is opened.
Nonces come from a store-wide write counter persisted ahead of use, the same
way the id ceiling is (`l1_bucketlog.md` §13.1), plus a random per-mount
prefix. The space cost is about 20 B per slot. Compaction needs no crypto,
because it copies slots byte for byte. A **bucket seal** cannot be the main
mechanism: it brings back the problem that sank the UBI attempt (§3.2).
Bucket-level and store-level state does matter for one thing, **freshness**
(rollback). §6 shows what an anchor can achieve and where the limit is.

---

## 1. Why protection at UBI failed, as a rule

Two access patterns in `blob_db` decide where protection can go:

1. **Writes append inside a block.** A bucket is an append-only stream of
   slots. Each `update` programs one slot into erased space at the bucket's
   cursor (`append_slot2()`), and nothing before it is rewritten. A seal over
   the whole block changes with every append. On flash that means rewriting
   the block, or never appending after sealing it. The UBI attempt hit this.
2. **Reads take small pieces of a block.** A `get` walks the bucket by
   reading **12 B per slot** (flags, `val_len`, id; `scan_bucket_for()`).
   Then it reads one payload and its CRC. RESULTS.md measures a read at
   about 460 µs on the nRF5340-DK, and nearly all of it is per-transaction
   overhead (~65 µs per read). If the walk had to verify a sealed unit larger
   than those 12 bytes, it would have to read and MAC the whole unit. That
   costs as much as the full-sector read the header walk was built to avoid
   (23 ms down to 460 µs, ×50).

**Rule:** the protected unit must be the unit that is read and written as a
whole, which is the slot payload. Metadata the walk needs must stay readable
without crypto. Below `blob_db` (UBI, or the `blob_db_store` seam) slot
boundaries are unknown, so the first half of the rule cannot be met there. A
bucket is larger than one read, so a bucket-level seal breaks the second half.

## 2. Threat model

As in zephyr-ubi: the attacker can **read and write the raw flash** (a
desoldered chip, an SPI clip, a debug port, a malicious image in another
partition) and can boot and observe the device. The attacker does **not** hold
the keying material, which stays in the PSA key store. Out of scope are side
channels, fault injection, compromised secure-domain code and availability:
anyone who can write the flash can erase it.

| # | Attack | Today | With per-slot AEAD (§4) | With anchor (§6) |
|---|---|---|---|---|
| 1 | Read stored data | open | **caught** (encrypted) | — |
| 2 | Change bytes of a slot, CRC recomputed | open | **caught**: tag fails, `-EIO` | — |
| 3 | Move a slot to another id or bucket | open | **caught**: id is in the AAD | — |
| 4 | Splice a slot from another device or store | open | **caught**: key bound to device and store uuid | — |
| 5 | Forge a tombstone or segment table | open | **caught**: flags in AAD, index payload sealed | — |
| 6 | Roll back the whole partition image | open | open | **caught**, to within one ceiling step |
| 7 | Roll back one bucket to an older authentic image | open | open | **open** (§6.2) |
| 8 | Clear bits in a blob's newest slot so reads fall back to the previous version | open | open, same class as 7 (§4.6) | open |
| 9 | Infer ids, sizes, write frequency | open | open: header and id stay in clear | — |

Rows 7 and 8 mark the limit. They are the per-block rollback that zephyr-ubi
also leaves open (its row 9) and that UBIFS authentication leaves open too.
Closing them takes one write to trusted storage per update (§6.2).

## 3. Options considered

| Where | Confidentiality | Integrity | Fits append + header walk? | Verdict |
|---|---|---|---|---|
| **L0 store seam** (`blob_db_store_read/write`), offset-tweaked cipher | yes | no room for tags | no: erased `0xff` stops being visible, and 12 B reads hit cipher-block boundaries | rejected (§3.1) |
| **UBI**, per block | yes | yes | no: append and partial read break | tried, failed (§1) |
| **Bucket seal**, MAC over the bucket image | no | coarse | no: a read must verify the whole bucket, and the seal changes on every append | rejected as primary (§3.2) |
| **Chained slot tags**, each tag covering the previous one | with AEAD | adds order | partly: compaction has to rechain, torn slots break the chain | rejected (§3.2) |
| **Per-slot AEAD in `blob_db`** | yes | yes | **yes**: the slot is the write unit and the read unit | **recommended (§4)** |
| **Per-value in L2/L3** (kvdb encrypts values) | values only | values only | yes | rejected (§3.3) |
| **Store-level freshness anchor** | — | rollback of the whole image | yes: written once every *K* writes | recommended add-on (§6.1) |
| **Per-bucket freshness** (Merkle tree or bucket heads) | — | rollback of one bucket | needs a trusted write per update | out of reach without cheap trusted storage (§6.2) |

### 3.1 Why not at the store seam

Encrypting at `blob_db_store_read/write` by flat offset fails in three ways:

- **End-of-log detection breaks.** The walk ends at a slot whose flags read
  `0xff`. The seam does not know which bytes are erased, so it would decrypt
  erased flash into noise. That is the UBI problem again.
- **Nonces repeat.** Compaction erases a bucket and writes new data at the
  same offsets. A CTR or GCM nonce derived from the offset therefore repeats,
  which gives away the XOR of plaintexts and, for GCM, the authentication key.
  XTS avoids that but is deterministic, has no integrity and works in 16 B
  units. The walk's 12 B reads at 4 B-aligned offsets do not line up with it.
- **There is no place for a tag.** The seam maps bytes one to one.

### 3.2 The bucket seal, looked at closely

A bucket is rewritten as a whole at exactly one point: compaction. So a seal
can be written there, as a MAC over the compacted image. The scratch seal
(`blob_db_scratch_seal`, `image_crc32`) already works that way. What would it
buy?

- **For reads, nothing.** To check that a slot belongs to the sealed image, a
  `get` must read and MAC the whole image. That is the full-sector read again,
  and it is what made the UBI attempt slow.
- **Slots appended after compaction are not covered.** They need their own
  protection anyway, so per-slot tags are still required. Once every slot has
  a tag, the image seal adds no integrity.
- **A chained variant** (each slot's tag also covers the previous tag, and the
  last tag summarises the bucket) does find a slot deleted or reordered in the
  middle of the log. It costs three things. Compaction has to recompute every
  surviving tag, because the chain changes once garbage is dropped. A torn
  slot breaks the chain, and the walk now steps over torn slots on purpose
  (`l1_bucketlog.md` §3.4). And the chain can only be checked by walking
  every payload. It still misses the real attack: an attacker who can erase
  writes back any earlier prefix of the chain, which is a valid chain.

What a bucket-level summary is useful for is **freshness**: checking whether
the bucket in flash is the latest one. That takes trusted state outside the
flash (§6). Without it, a bucket seal says "authentic", not "current", and
per-slot tags say "authentic" already.

### 3.3 Why not per value in kvdb or the containers

It would encrypt values only. Keys inside kvhash buckets, the rootreg map and
container structure (child ids, list links) would stay in the clear and
unauthenticated, so an attacker could swap whole nodes. Every container would
need its own scheme. Nonce management above L1 has no monotonic counter to
work from either. L1 protects every client, rootreg included, in one place.
The only argument for an upper layer is **selectivity**, protecting some
stores and not others. That is an open question (§8).

## 4. Recommended design: per-slot AEAD

### 4.1 Slot format

```
today      slot_hdr(4) | id(8) | payload[val_len]                  | crc16(2)
protected  slot_hdr(4) | id(8) | nonce(12) | ct[val_len] | tag(T) | crc16(2)
           └── read by the walk (unchanged) ──┘
```

- `slot_hdr` and `id` stay in clear text and stay where they are, so the walk,
  the slot index, mount's max-id scan, `count`, `inspect` and compaction's
  liveness test do not change.
- **AAD** = `slot_hdr ‖ id ‖ nonce`. The flags are covered (TOMBSTONE,
  INDEXED, SEGMENT), and so are `val_len` and the id. A slot cannot be
  retagged, resized or moved to another id. The bucket follows from `id mod N`
  and is bound implicitly.
- The AAD does **not** include the bucket offset, bucket generation or
  `peb`. That is deliberate: compaction moves slots, and leaving position out
  lets it copy them byte for byte (§4.4).
- **Keep the CRC16, computed over every byte before it.** The CRC is there for
  damage and the tag for tampering, as in UBI's headers (MAC first, then a CRC
  over everything). The CRC keeps torn and rotten slots recognisable without
  a key or a cipher call, so compaction and the walk keep their crash
  behaviour. A CRC that passes over a tag that fails means tampering, which
  can be told apart from damage (§4.6).
- `val_len` is the plaintext length. AES-CCM and ChaCha20-Poly1305 are stream
  modes, so the ciphertext is the same length.
- A tombstone has `val_len = 0` and still carries a nonce and tag, so it
  cannot be forged.

`slot_size(val_len) = round_up(4 + 8 + 12 + val_len + T + 2, W)`. For
T = 8 that is 34 B of overhead against 14 B today.

### 4.2 Nonces

AEAD needs the nonce never to repeat under one key. **This is the part that
has to be right.** Nonce = `mount_salt(4) ‖ seq(8)`, stored in the slot:

- **`seq`** is a store-wide write counter, incremented once per slot appended.
  It is persisted as a **leading ceiling** in the master, exactly like
  `next_id_hint`: mount resumes from the ceiling, and a new ceiling is written
  `BLOB_DB_SEQ_STEP` ahead whenever the current one is reached. A crash can
  only skip values, never repeat them. The ceiling write can share the master
  write that raises the id ceiling. At a step of 1024 it adds one master
  replace per 1024 slot writes.
- **`mount_salt`** is 32 random bits drawn with `psa_generate_random()` at
  every mount. It covers the case the ceiling cannot: an attacker who restores
  an older master, or the whole older image (rows 6 and 7), so that the device
  resumes from an old ceiling and writes new plaintexts under nonces it has
  used before. With the salt, a repeat needs that **and** a 2⁻³² collision on
  the same mount. That way rollback, which cannot always be detected, does not
  also become a confidentiality break. It is also why the master needs no MAC
  for nonce safety: a forged master cannot pick the salt.
- **Rejected: deriving the nonce from the position** (bucket, generation,
  offset). It would save storing 12 B per slot. But a rolled-back bucket would
  have the device append new data at old (bucket, generation, offset) triples,
  so an undetectable rollback would turn into a nonce repeat. A torn slot
  whose header never landed also gets its offset reused by the next append.

The stored `seq` also orders the slots of one id. "Highest authentic seq
wins" would stop an attacker who appends a replayed old slot without erasing
anything. An attacker who can erase rewrites the bucket instead, so this
protects little and is left out of the first cut.

### 4.3 Keys

- The application provides input keying material through a hook shaped like
  `blob_db_ubi_ikm_key()`, for example
  `blob_db_protect_ikm_key(psa_key_id_t *)`, with a `BUILTIN` development
  key as the alternative, the same choice `BLOB_DB_UBI_KEY` offers. With the
  UBI backend the same IKM can feed both: the derivations below use their own
  `info` label, so nothing extra has to be provisioned.
- Slot key = HKDF-SHA256(IKM, salt = `store_uuid`, info = `"blob_db/slot/v1" ‖ suite`).
  `store_uuid` holds 16 random bytes written into the master at format. It
  separates partitions and incarnations: a reformat changes the key, so slots
  from before the format cannot be spliced back in (row 4), and seq may start
  again from zero.
- The derived key is a volatile PSA key, created at mount and destroyed at
  unmount. It is not kept in RAM in usable form beyond what PSA holds.
- With the `BUILTIN` key, AUTH catches damage only and AEAD only obfuscates.
  That is the same position `BLOB_DB_UBI_KEY_BUILTIN` is in, and it should
  be documented the same way.

### 4.4 What changes per path

| Path | Change | Crypto |
|---|---|---|
| `update` / `append_slot2()` | take `seq`, encrypt the payload from the caller's buffer straight into the staged slot in `g_bbuf_new`, add the tag, then the CRC | 1 AEAD encrypt |
| `get` / `slot_verify()` | read `nonce ‖ ct ‖ tag ‖ crc` as **one** contiguous read (today the payload and the CRC are two reads), check the CRC, then decrypt into `g_bbuf` and verify | 1 AEAD decrypt |
| `blob_db_read` (inline) | already stages the whole payload, so it is the same as `get` | 1 |
| Header walk, slot index, mount scan, `count`, `exists` | **none**: they read only the clear 12 B head | 0 |
| **Compaction** and recovery | **none**: liveness and CRC filtering are as today, and slots are copied byte for byte | **0** |
| `iterate` | decrypt each live slot into `g_bbuf_new` (unused outside compaction) before the callback | 1 per live slot |
| Segmented objects | each segment and the index slot are ordinary sealed slots. A partial read decrypts only the segments it touches. `blob_db_write` already rewrites whole segments under new ids, which gives a new seq | 1 per segment |
| Segment sweep (mount after a crash) | `blob_db_seg_hdr` (`owner_id`, `seq`) has to be readable without a full decrypt. Keep those 12 B in **clear text and in the AAD**, the same treatment as the slot header | 0 to find, 1 to confirm |
| `inspect` / shell `dump` | shows ciphertext unless a decrypting variant is added (debug builds only) | — |

Compaction needing no crypto is the main property of this design. Compaction
is already the most expensive operation (0.69 s per compacting update on the
DK with atomic replace), and this adds nothing to it. It would have to
re-encrypt if position were in the AAD or the nonce, and it would have to
re-MAC if a bucket seal were used.

### 4.5 Modes, as one Kconfig choice

```
choice BLOB_DB_PROTECT
    BLOB_DB_PROTECT_NONE   (default)   today's format; CRC only
    BLOB_DB_PROTECT_AUTH               payload in clear, tag over AAD ‖ payload
    BLOB_DB_PROTECT_AEAD               payload encrypted and tagged
choice BLOB_DB_PROTECT_ALG             (AUTH, AEAD)
    AES_CCM            tag 8 (default) / 12 / 16 — uses AES hardware where PSA has it
    CHACHA20_POLY1305  tag 16 — faster in pure software (§7)
choice BLOB_DB_PROTECT_KEY             BUILTIN | APP
BLOB_DB_PROTECT_ANCHOR                 bool — §6.1 hooks
BLOB_DB_SEQ_STEP                       int, default 1024
```

AUTH fits a product that needs tamper evidence but not secrecy. It keeps
`blob_db dump` readable and has the same nonce and format machinery, so it is
nearly free to offer. AES-CCM is preferred to GCM: a repeated nonce under GCM
reveals the authentication key, while under CCM it reveals the XOR of the two
plaintexts. CCM also allows an 8 B tag, which is enough here, because each
forgery attempt needs physical access and a reboot.

**Format and downgrade.** The slot layout changes what a payload means, so
this is a MAJOR change. The `format_major` scheme is linear today ("reads
anything up to its own major"). That cannot hold here: if a protected build
accepted unprotected slots, an attacker could write plaintext slots with
nothing to check. The master records a **suite** byte (0 = none), and a build
mounts only a store whose suite equals its own. Switching takes a reformat or
an explicit one-time migration (iterate and rewrite). A master rewritten to
suite 0 is then a refusal to mount, which is a denial of service, not a
bypass. The master body has room for this without growing: `reserved1[24]`
holds `store_uuid[16]` and `seq_hint[8]`, and `reserved0` holds the suite.

### 4.6 What a failed check means

Reads fall back today: if the newest slot for an id fails its CRC, `get`
returns the newest slot below it that passes (`walk_live_slot()`). That is
correct for a torn write, because the interrupted update never committed. A
protected store needs one more case:

| CRC | Tag | Meaning | Result |
|---|---|---|---|
| ok | ok | committed data | data |
| fails | (not checked) | torn or rotten: damage | fall back as today, and report an event |
| ok | fails | **tampering**: someone recomputed the CRC | **`-EIO`**, no fallback, report an event |

Falling back on a tag failure would turn one forged slot into a rollback to
the previous version. Row 8 remains: an attacker can clear bits in the newest
slot so that its CRC fails, and the reader falls back as if the write had
been torn. A torn tail cannot be told apart from deliberate damage, because
after a reboot a torn slot can end up mid-log. So row 8 is the same class as
row 7. The event hook (`blob_db_protect_event()`, weak) lets an application
count damage and react to tampering, like UBI's `HDR_CORRUPT` and
`HDR_TAMPERED`.

Contract impact: `-EIO` is already the documented "cannot return committed
data" error (§2, "No partial reads"). The contract gains a sentence: in a
protected build, `-EIO` also covers an authentication failure. The per-slot
payload cap drops by the extra overhead, from `(sector − 16)/2 − 14` to
`(sector − 16)/2 − 34`: 2026 → 2006 B on 4 KB sectors.

## 5. A bucket-level mechanism that is worth having

Only one bucket-wide check pays for itself: **optional verification at
compaction**. Compaction already holds the whole bucket in RAM and spends
hundreds of milliseconds on erase and program. Running the AEAD over each
surviving slot adds a few milliseconds in software (§7), and it means a
tampered slot is found at maintenance time, not at the next read of that id.
It does not change what is caught (row 2 is caught at read anyway), only
when. It also brings back a question this proposal avoids so far: what
compaction should keep when the winning slot fails. So it is an option to
measure later, not part of the first cut.

## 6. Freshness and rollback

### 6.1 Store-level anchor: cheap, and recommended

The seq ceiling in the master only grows, which makes it a usable **store
clock**. Optional hooks, shaped like `blob_db_ubi_state_check()`, would work
like this:

- `blob_db_anchor_commit(store_uuid, seq_ceiling)` is called **after** every
  ceiling master write: once every `BLOB_DB_SEQ_STEP` slot writes, to PSA ITS,
  a monotonic counter or a secure element.
- `blob_db_anchor_check(store_uuid, seq_ceiling)` runs at mount. If the master
  ceiling is below the anchored one, the partition was rolled back, and mount
  fails with `-EROFS`. A different uuid without a format the application asked
  for is refused too, using the same rule as UBI's sample (`image_seq`).
- The weak default trusts everything, so "no rollback detection" is a choice
  that shows in the code, as with UBI.

The anchor is committed after the master write, so the anchor never runs
ahead of flash. A crash between the two leaves the anchor behind, which is
safe. This closes row 6, the whole-image restore, to within one step of
writes. It does not close row 7.

### 6.2 Per-bucket and per-blob freshness: the limit

Detecting a single bucket restored to an older authentic image needs trusted
state that **changes on every write to that bucket**. Ways to get it:

- Per-bucket heads (last seq, or an incremental MAC over the bucket's
  (id, seq, tag) set) in RAM: 2045 × 8–16 B = 16–32 KB. That is a large P3
  exception, and the heads still have to be anchored.
- A Merkle tree over the bucket heads, with the root anchored: log₂N ≈ 11
  extra node writes per update in flash, **plus** one trusted write per
  update.
- An order-independent multiset hash of the live set: compaction leaves it
  unchanged, but it still needs one trusted write per update to be fresh.

All three need **one trusted-storage write per `update`**. On parts where
trusted storage is itself flash-backed (PSA ITS on internal flash), that
doubles write cost and wear. It becomes reasonable only with hardware that has
a cheap authenticated monotonic counter (RPMB, a secure element). zephyr-ubi
and Linux UBIFS stop at the same point. The proposal is to document rows 7
and 8 as the limit and leave this for a product that brings such hardware.

## 7. Costs

**Space.** +20 B per slot with AES-CCM and T = 8, +28 B with
ChaCha20-Poly1305:

| payload | slot today | protected, CCM-8 | overhead |
|---|---|---|---|
| 0 (tombstone) | 14 | 34 | +20 B |
| 64 | 78 | 98 | +26 % |
| 256 (default cap) | 270 | 290 | +7.4 % |
| 512 (perf churn blob) | 526 | 546 | +3.8 % |
| 2006 (segment, 4 KB sector) | 2020 | 2040 | +1.0 % |

Buckets fill a little sooner, so compaction runs a little more often. For
mid-size kvhash node blobs that is a few percent. The 12 B nonce could shrink
to 10 B (seq 48 bits is 2.8 × 10¹⁴ writes) if this matters.

**Flash I/O.** No change to walks, mount or compaction. On a `get`, one fewer
read transaction (nonce, payload, tag and CRC in one contiguous read, against
payload and CRC in two today) and about 22 B more read. On the DK model
(~65 µs per transaction, ~0.63 µs/B) that is about **−50 µs**, slightly
faster. The single read is worth doing on its own, protection or not.

**CPU.** One AEAD call per `get`, `update` or `delete`. The DK builds use
Mbed TLS in software (no CryptoCell driver upstream). **These figures are
estimates, not measured:** AES-CCM in software on a Cortex-M33 at 128 MHz
should be on the order of 100–200 µs for a 256 B payload, including PSA call
overhead. ChaCha20-Poly1305 should be several times cheaper. Against measured
costs (`get` 460 µs, `update` 2.4–15 ms, compacting update 0.69 s):

| Op | Estimated added cost (software CCM) | Relative |
|---|---|---|
| `get` (256 B) | +100–200 µs, less ~50 µs saved I/O | roughly +10–35 % |
| `update` | +100–200 µs | ~1–8 % |
| compaction | 0, or a few ms with verification | ≤ 1 % |
| mount | one HKDF and key import | negligible |

Reads are the only place this is visible. It is the first thing to measure
with `app_perf_blob_db` before choosing between CCM and ChaCha20-Poly1305 per
board. Hardware AES (CryptoCell through nRF Connect SDK, CRACEN on nRF54)
reduces it to call overhead.

**RAM and stack.** No new static buffers: encryption goes into the staged slot
in `g_bbuf_new`, and decryption into `g_bbuf`, where `get` stages its payload
today. PSA one-shot AEAD uses a few hundred bytes of stack, within R7's 4 KB.
A mount adds one volatile PSA key slot.

**Footprint.** On the UBI backend PSA Crypto is already linked, so this adds
the CCM or ChaCha code paths. On `flash_area` builds this is the first PSA
dependency, which is a real cost for the backend that exists to be small. It
is one more reason protection stays opt-in.

## 8. Plan and open questions

**Phase 1 (per-slot AEAD).** The slot format, the seq ceiling and mount salt
in the master, key derivation, the suite byte, the failure policy in §4.6,
and AUTH and AEAD modes. Tests, per P8:
- tamper cases in `tests/lib/blob_db`, using its existing raw-offset fault
  injection: a payload byte changed with the CRC recomputed gives `-EIO`; a
  slot copied onto another id's bucket gives `-EIO`; a slot from another store
  uuid gives `-EIO`; a forged tombstone has no effect;
- nonce uniqueness across crash points (crash hooks around the ceiling write);
- the full blob_db, container, kvdb and blobfs suites rerun with protection
  on, using the same scenario approach as `.slot_index`;
- `app_perf_blob_db` on the DK with CCM and with ChaCha20-Poly1305.

**Phase 2 (anchor).** The §6.1 hooks and a sample that anchors in PSA ITS.

**Later, if needed.** A key epoch in `slot_hdr.reserved`, for rotation in
which compaction re-encrypts slots under an old epoch. Length padding, if
`val_len` leaking is a concern. Verification at compaction (§5).

**Questions that decide the design:**
1. **Confidentiality, or integrity only?** If only tamper evidence is needed,
   AUTH is enough and the CPU cost on reads roughly halves with CCM.
2. **Target hardware crypto.** Upstream Zephyr on the nRF5340 has no
   CryptoCell driver, so everything is software. That decides CCM against
   ChaCha20-Poly1305.
3. **Is rollback in scope?** If not, stop after phase 1. If whole-image
   restore matters, phase 2 is cheap. If per-blob rollback matters, it needs
   hardware (§6.2).
4. **Whole store, or some stores?** `blob_db` is one store per partition, so
   L1 protection covers all of it. If only some kvdb stores need it, the
   options are a second partition with its own store (blob_db is a singleton
   today) or an opt-out flag per slot.
