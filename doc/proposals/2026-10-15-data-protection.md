# Design exploration — encrypting and authenticating data in `blob_db`

Status: **exploration** · 2026-10-15 · nothing implemented
· Target contract: `doc/layers/l1_blob_db.md` (§2 stability contract: one new error meaning; §3 cost bounds)
· Target implementation design: `doc/impl/l1_bucketlog.md` (§3.3 slot format, §3.1 master body, §5.4 read path)
· Related: `doc/impl/l0_backends.md` ("Authenticated metadata"), zephyr-ubi `docs/security.md`, `library/ubi_key.c`, `library/ubi_header.c` (algorithms aligned with these, §4.7)
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
The primitives are the ones zephyr-ubi already uses: HKDF-SHA256 from the same
key handle, AES-128, AES-CMAC for an authenticate-only mode, and the same
CRC-then-MAC verdicts (§4.7).

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

Compaction is still the right moment to make the region **smaller**: it can
share one nonce across the whole region, and with more trade-offs share one
seal. §5 works through that.

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
- `val_len` is the plaintext length. AES-CCM is a stream mode, so the
  ciphertext is the same length.
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

- **One IKM for both layers.** Today's `blob_db_ubi_ikm_key()` and the
  `BLOB_DB_UBI_KEY` choice (`BUILTIN` | `APP`) become backend-neutral, for
  example `blob_db_ikm_key()` and `BLOB_DB_KEY`. On the UBI backend the same
  handle keeps feeding UBI. On `flash_area` it feeds only `blob_db`. Nothing
  extra has to be provisioned.
- **The same derivation as UBI** (`ubi_key.c`): HKDF-SHA256 with a fixed
  compile-time salt, `info = label ‖ context`, and output into a volatile
  AES-128 key. UBI uses salt `"zephyr-ubi/v1"` and labels
  `"zephyr-ubi/header/v1"` and `"zephyr-ubi/volume-table/v1"`. `blob_db` uses
  salt `"blob_db/v1"`, and labels `"blob_db/slot-aead/v1"` and
  `"blob_db/slot-mac/v1"` (one per mode, §4.5). Different salts and labels
  give independent keys from the same IKM.
- **Context = `store_uuid`.** These are 16 random bytes drawn with
  `psa_generate_random()` at format and stored in the master, the counterpart
  of UBI's `image_seq`. This is the one place `blob_db` deliberately departs
  from UBI. UBI's context is a constant from the application
  (`key_context`, `"blob_db"` in our backend), and its salt is a constant
  "because anything read from the flash would have to be trusted before it
  could be verified." UBI can do that because its MAC needs no nonce. `blob_db`
  AEAD's counter nonce starts again from zero after a reformat, so the key has
  to change with each store incarnation, or nonces would repeat under one key.
  Reading the uuid from flash is safe in the sense UBI cares about: a forged
  uuid only produces a key under which nothing verifies, so the store is
  refused, never accepted. The salt stays constant, as in UBI. A fresh uuid
  also stops slots from before a format being spliced back in (row 4).
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

In the base design compaction needs no crypto. Compaction is already the most
expensive operation (0.69 s per compacting update on the DK with atomic
replace), and the base design adds nothing to it. §5 gives that up on purpose:
a few milliseconds of re-encryption per compaction buys back 12 B per
compacted slot.

### 4.5 Modes, as one Kconfig choice

```
choice BLOB_DB_PROTECT
    BLOB_DB_PROTECT_NONE   (default)   today's format; CRC only
    BLOB_DB_PROTECT_AUTH               payload in clear, tag over AAD ‖ payload
    BLOB_DB_PROTECT_AEAD               payload encrypted and tagged
BLOB_DB_PROTECT_TAG_LEN               int, 8 (default) or 16 (= UBI_MAC_SIZE)
    # AUTH: AES-CMAC-128, UBI's algorithm, truncated with
    #       PSA_ALG_TRUNCATED_MAC(PSA_ALG_CMAC, 8) at 8
    # AEAD: AES-CCM-128 under an HKDF key from the same IKM
choice BLOB_DB_KEY                     BUILTIN | APP — replaces BLOB_DB_UBI_KEY, feeds both layers
BLOB_DB_PROTECT_ANCHOR                 bool — §6.1 hooks
BLOB_DB_SEQ_STEP                       int, default 1024
```

AUTH fits a product that needs tamper evidence but not secrecy. It uses
**exactly UBI's primitive**, AES-CMAC through `psa_mac_compute` and
`psa_mac_verify`. On the UBI backend it adds no crypto code, and it keeps
`blob_db dump` readable. CMAC is deterministic, so an AUTH slot carries **no
nonce**:
`slot_hdr ‖ id ‖ payload ‖ tag ‖ crc16`, +8 B over today at an 8 B tag. With
no nonce, AUTH also needs neither the seq ceiling nor the mount salt. AES-CCM is preferred to GCM: a repeated nonce under GCM
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

### 4.7 Same algorithms as zephyr-ubi

On the UBI backend the stack runs two integrity layers, one on top of the
other. They should share their primitives, key handling and failure
vocabulary, so the product has one crypto story and one set of code. The
table maps what UBI does (`library/ubi_key.c`, `library/ubi_header.c`,
`docs/security.md`) to what `blob_db` does:

| Aspect | zephyr-ubi | `blob_db` (this proposal) |
|---|---|---|
| Keying material | PSA key handle with `PSA_KEY_USAGE_DERIVE`, from `blob_db_ubi_ikm_key()` | **the same handle**, from a backend-neutral `blob_db_ikm_key()` (§4.3) |
| KDF | HKDF-SHA256, fixed salt `"zephyr-ubi/v1"`, info = label ‖ `key_context` | HKDF-SHA256, fixed salt `"blob_db/v1"`, info = label ‖ `store_uuid` (§4.3 says why the context differs) |
| Derived keys | volatile AES-128, one per purpose (header, volume table) | volatile AES-128, one per mode (slot-mac, slot-aead) |
| MAC | AES-CMAC-128, 16 B, `psa_mac_compute` and `psa_mac_verify` | **AES-CMAC** in AUTH. AEAD uses AES-CCM, whose MAC is CBC-MAC under the same AES. Tag 8 B by default, 16 B to match UBI |
| Encryption | none: "protect data with an AEAD of your own" | AES-CCM, the AEAD UBI's security doc asks the layer above for |
| What is bound | `pnum` at the front of the MAC message; the doc asks the layer above to bind `vol_id` and `lnum` | the id, which fixes the bucket (= `lnum`, since one bucket is one LEB). `vol_id` and the partition are covered by `store_uuid` in the key |
| Nonce rule from UBI's doc | "a new nonce for every version of a block"; never offset alone, because `ubi_leb_change()` restarts at offset 0 | a new nonce per slot version, from the seq counter and the mount salt (§4.2), never position alone; §5 B adds a fresh base per compaction, i.e. per `ubi_leb_change()` |
| Layout | MAC, then CRC32 over everything before it, MAC included | tag, then CRC16 over everything before it, tag included |
| Check order | erased → magic → CRC → MAC; never `memcmp` a MAC | erased → flags and length → CRC → tag, compared in constant time by `psa_mac_verify` or `psa_aead_decrypt` |
| Verdicts | `UBI_EVENT_HDR_CORRUPT` (CRC fails), `UBI_EVENT_HDR_TAMPERED` (CRC passes, MAC fails) | **the same two**, for slots (§4.6) |
| Randomness | `image_seq` from `psa_generate_random()` at format | `store_uuid` at format and `mount_salt` at mount, from the same call |
| Rollback | `state_cb` at attach and every N writes, given `image_seq`, `revision`, `max_sqnum`, returns TRUSTED or UNTRUSTED | anchor check (§6.1) on `store_uuid` and the seq ceiling, the counterparts of `image_seq` and `max_sqnum` |
| Verify on read | optional: `CONFIG_UBI_VERIFY_ON_READ` (header only) | always: every `get` checks the tag of the slot it returns |
| Seal over rewritten data | `ubi_leb_change()` stores `data_crc` in the MAC'd header, checked at attach only | §5: a shared nonce at compaction; a region seal only as variant C |

Three places where the alignment should become shared code, not just matching
choices:

1. **One event sink.** `blob_db_store_ubi.c` already receives UBI's events in
   `on_event()`. Slot verdicts should go to the same application hook with the
   same two kinds, tagged with their layer, so an application handles
   "corrupt" and "tampered" once.
2. **One trust decision.** Merge the §6.1 anchor into the existing
   `blob_db_ubi_state_check()`. It becomes `blob_db_state_check()`, given UBI's
   `ubi_device_info` when the backend has one, plus `store_uuid` and the seq
   ceiling, with the same TRUSTED or UNTRUSTED verdict and the same `-EROFS`
   outcome. On `flash_area` it gets the `blob_db` fields only.
3. **One key path.** `key_acquire()` in `blob_db_store_ubi.c` moves into
   `blob_db` core. It runs once at mount, hands the IKM to UBI when the
   backend is UBI, and derives the slot key from it.

**UBI already seals a rewritten block, but with a CRC.** When compaction
replaces a bucket through `ubi_leb_change()`, UBI writes `data_crc` over the
new contents into the volume identifier header, which carries its CMAC. That
is the region seal from §5 variant C, built one layer down, and it follows
C's verify-once model ("data checksums are checked at attach only"). A CRC32
is linear and unkeyed, though, so an attacker who rewrites the data can make
its CRC32 match. It catches damage, not tampering. If zephyr-ubi ever stored a
keyed MAC there in place of the CRC, `blob_db` on UBI would get variant C's
saving with no extra bytes. It would also inherit C's limits: the check runs
at attach, not per read. That change belongs in zephyr-ubi, and it does
nothing on `flash_area`. Variant B does not depend on it.

## 5. Re-sealing at compaction to save space

Most of a bucket's live data sits in the region compaction wrote. Slots
appended since then are the churn. In the base design every slot in that
region keeps its own 12 B nonce and 8 B tag. Compaction rewrites the region
as a whole anyway, so it can re-encrypt it in a more compact form. Moving to
that form costs a decrypt and an encrypt per surviving slot: about 4 KB × 2
AEAD passes, a few milliseconds in software per 4 KB bucket, against the
0.69 s a compacting update already takes on the DK.

```
base     [bhdr][n|ct|tag|crc][n|ct|tag|crc][n|ct|tag|crc] … appended slots …
B        [bhdr+nbase][ct|tag|crc][ct|tag|crc][ct|tag|crc]  … appended slots …
C        [bhdr+nbase][ct|crc][ct|crc][ct|crc][SEAL]         … appended slots …
          └──────────── compacted region ─────────────┘
```

(Each slot also keeps `slot_hdr ‖ id` at its start, so the walk is unchanged.
A COMPACTED flag in `slot_hdr` tells the two forms apart.)

**The nonce can be shared. This is where most of the saving is.** Compaction
takes one fresh value from the seq counter (§4.2), stores it once in the
bucket header with the mount salt, and gives each compacted slot the nonce
`salt ‖ seq_base ‖ offset`. The offset is known whenever the slot is read: the
walk and the slot index both locate slots by it. Every nonce stays unique.
`seq_base` is new at every compaction, and appended slots draw their own seq
values from the same counter. A bucket rolled back to an old image does not
lead to a repeat either, because the device never encrypts under an old
image's base again. That repeat is what ruled out position-derived nonces in
§4.2, and a fresh base per compaction avoids it. Saving: 12 B per compacted
slot.

**The tag can be shared too, but reads pay for it.** Variant C replaces the
per-slot tags with one MAC over the whole compacted region, written at its
end. That saves another 8 B per slot. The cost is §1 again: a `get` that lands
in the region can trust its slot only after reading and MACing the whole
region. On the DK's 64 KB sectors that is the 23 ms full-sector read the
header walk replaced (RESULTS.md), plus about as much again in software MAC
time, against 460 µs today. Two ways to soften it:

- **Verify once per boot.** Keep one RAM bit per bucket (256 B for 2045
  buckets) set by the first successful region check. Later reads in that
  bucket trust the bit. The first read of each bucket after boot still pays
  the full cost. The guarantee also gets weaker: flash changed after the check
  (an attacker with a clip on the external QSPI chip while the device runs) is
  no longer caught. Variants A and B check the bytes they return on every
  read.
- **Seal in chunks.** One tag per ~512 B of the region sits between B (one
  tag per slot) and C (one per region). A read verifies the chunks its slot
  overlaps. Space and read cost both scale with the chunk size.

| Per compacted slot | A (base) | **B: shared nonce** | C: shared nonce and seal |
|---|---|---|---|
| Overhead vs today's 14 B | +20 B | **+8 B** | +0 B (+~24 B per bucket) |
| 256 B payload | +7.4 % | **+3.0 %** | ~0 % |
| `get` on a compacted slot | 1 AEAD over the slot | **1 AEAD over the slot** | whole region read and MAC, or a per-boot RAM bit |
| Compaction | copy only | decrypt and re-encrypt, ~ms | same as B, plus the region MAC |
| Tamper caught at read | yes | **yes** | only on the first read since boot, with the RAM bit |

Slots appended after compaction keep the full form in all three variants:
they have no shared base, and they are written one at a time.
In AUTH mode (CMAC, no nonce, §4.5) there is no nonce to share, so B saves
nothing there. Only C's shared seal would.

**Recommendation: B.** It keeps 60 % of the saving (12 of the 20 B), and reads
cost what they do in the base design. C saves the remaining 8 B by making
reads slower or checks weaker, which is the trade the UBI attempt already
failed on.

B affects four places:
- **Compaction verifies, and must decide what happens on failure.** It cannot
  re-encrypt a slot without decrypting it, so every survivor is checked. A
  slot that fails is copied unchanged in its full form, not converted. Reads
  of it then still return `-EIO` (§4.6), and compaction never makes up a
  verdict of its own.
- **Recovery stays crypto-free.** The re-encrypted image is staged and sealed
  exactly as today (scratch with trailer, or atomic replace), and recovery
  copies it byte for byte.
- **The bucket header grows** by `seq_base` and the salt, 12 B once per
  bucket. Its CRC can stay a CRC. A forged base only makes the region's tags
  fail.
- **The slot index is unaffected.** It already stores offsets, which is all
  B's nonce needs.

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

**Space.** AEAD (AES-CCM, T = 8): +20 B per slot. AUTH (AES-CMAC, T = 8):
+8 B, because there is no nonce. AEAD:

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
overhead. CCM makes two AES passes. AUTH's CMAC makes one, about half the
cost. Against measured
costs (`get` 460 µs, `update` 2.4–15 ms, compacting update 0.69 s):

| Op | Estimated added cost (software CCM) | Relative |
|---|---|---|
| `get` (256 B) | +100–200 µs, less ~50 µs saved I/O | roughly +10–35 % |
| `update` | +100–200 µs | ~1–8 % |
| compaction | 0 in the base design; a few ms with re-sealing (§5 B) | ≤ 1 % |
| mount | one HKDF and key import | negligible |

Reads are the only place this is visible, and they are the first thing to
measure with `app_perf_blob_db`. Hardware AES (CryptoCell through nRF Connect
SDK, CRACEN on nRF54) reduces it to call overhead, and benefits UBI's CMAC as
much as it does `blob_db`. ChaCha20-Poly1305 would probably be faster in
software, but it would bring a second cipher family next to UBI's AES (§4.7).
It is the fallback only if measurements show AES-CCM in software to be
unacceptable.

**RAM and stack.** No new static buffers: encryption goes into the staged slot
in `g_bbuf_new`, and decryption into `g_bbuf`, where `get` stages its payload
today. PSA one-shot AEAD uses a few hundred bytes of stack, within R7's 4 KB.
A mount adds one volatile PSA key slot.

**Footprint.** On the UBI backend PSA Crypto, AES, CMAC and HKDF are already
linked for UBI. AUTH adds no crypto code, and AEAD adds only the CCM module. On `flash_area` builds this is the first PSA
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
- `app_perf_blob_db` on the DK in AUTH (CMAC) and AEAD (CCM) modes.

**Phase 1b (re-sealing, §5 B).** Compaction re-encrypts the region it writes
under one nonce base kept in the bucket header. It is a format change of its
own, so it is better shipped with phase 1 than after it. Tests: compaction of
a bucket that holds a tampered slot (the slot survives unconverted and still
reads `-EIO`); crash at every compaction point with the re-encrypted image;
nonce uniqueness across repeated compactions of one bucket.

**Phase 2 (anchor).** The §6.1 hooks and a sample that anchors in PSA ITS.

**Later, if needed.** A key epoch in `slot_hdr.reserved`, for key rotation.
Re-sealing at compaction is the natural place to move slots to the new key.
Length padding, if `val_len` leaking is a concern. Chunked seals (§5), if
measurements show the 8 B per-slot tag matters.

**Questions that decide the design:**
1. **Confidentiality, or integrity only?** If only tamper evidence is needed,
   AUTH is enough and the CPU cost on reads roughly halves with CCM.
2. **Target hardware crypto.** Upstream Zephyr on the nRF5340 has no
   CryptoCell driver, so everything is software. The plan is AES throughout,
   matching UBI. Only a poor software measurement would reopen
   ChaCha20-Poly1305.
3. **Is rollback in scope?** If not, stop after phase 1. If whole-image
   restore matters, phase 2 is cheap. If per-blob rollback matters, it needs
   hardware (§6.2).
4. **Whole store, or some stores?** `blob_db` is one store per partition, so
   L1 protection covers all of it. If only some kvdb stores need it, the
   options are a second partition with its own store (blob_db is a singleton
   today) or an opt-out flag per slot.
