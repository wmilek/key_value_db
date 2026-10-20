# Design change proposal — authenticated and encrypted storage in `blob_db`

Status: **phases 0 and 1 implemented** (§12 lists where the build differs
from the text below); phase 2 open · 2026-10-15
· Target contract: `doc/layers/l1_blob_db.md` (§2 error meanings, R6 payload cap, a new optional hook header)
· Target implementation design: `doc/impl/l1_bucketlog.md` (§3 on-flash format, §5.4 read path, §5.6 compaction, §6 crash table)
· Also changes: `doc/impl/l0_backends.md` ("Authenticated metadata"), `lib/blob_db/blob_db_store_ubi.c` (key and event path)
· Rationale, alternatives and cost estimates: `doc/proposals/2026-10-15-data-protection.md` (the exploration; cited below as **[E §n]**)
· Algorithms aligned with zephyr-ubi v0.1: `library/ubi_key.c`, `library/ubi_header.c`, `docs/security.md`
· New dependency: PSA Crypto (already selected by the UBI backend) on both backends, plus `PSA_WANT_ALG_CCM` for encryption
· Governed by `doc/principles.md`

**The ask.** Data in `blob_db` is stored in the clear and protected only by
CRCs on both backends. Anyone who can read the flash reads it. Anyone who can
write the flash can change it, move it between blobs, or point one bucket at
another, and `blob_db` serves the result as data or quietly loses it. An
earlier attempt to protect whole blocks in UBI failed, because neither appends
into a block nor small reads out of it survive sealing at block granularity
[E §1].

**Short answer.** Protect at the slot, the unit `blob_db` already writes and
verifies whole. Choosing `CONFIG_BLOB_DB_PROTECT` gives one of two modes:

- **AUTH:** an AES-CMAC tag on every slot, exactly UBI's primitive. The data
  stays readable and every change to it is detected.
- **AEAD:** AES-CCM on every slot. The data is encrypted and authenticated.

In both modes the slot header and id stay in the clear for the walk and are
covered by the tag. `blob_db`'s own metadata (master, bucket headers,
compaction seal) gets a MAC under a separate key, and a bucket header that
fails its check is reported, never reformatted over. Keys come from the same
application key handle UBI uses, through the same HKDF-SHA256 construction.
In AEAD mode, compaction re-encrypts the slots it keeps under one shared nonce
base, so compacted slots carry no nonce of their own. Rollback detection is an
optional application hook, merged with UBI's state check.

---

## 1. Requirements

| # | Requirement | Consequence |
|---|---|---|
| R1 | Nothing `blob_db` returns as data was forged, changed, or taken from another id or another store | a tag per slot, with the id in the authenticated data and the store in the key |
| R2 | In AEAD mode, data at rest is confidential | AES-CCM per slot, with a nonce that never repeats under one key, rollback included |
| R3 | `blob_db` does not depend on UBI's header MAC for R1 | its own metadata is authenticated; a displaced sector gives `-EIO`, never data, a reused id or an erased bucket (§3.6) |
| R4 | The cost of a `get` stays bounded by one slot, not one bucket | the walk reads only clear headers; one crypto call per read (P2, contract R2) |
| R5 | No new steady-state RAM | per-mount: one salt, one counter, three volatile PSA keys; nothing per blob or bucket (P3) |
| R6 | Crash safety as today | the same two commit primitives; crypto state persisted ahead of use (§5) |
| R7 | The same algorithms and key source as zephyr-ubi | HKDF-SHA256, AES-128, AES-CMAC; one key handle feeds both layers (§3.1) |
| R8 | Off by default; zero cost when off | `BLOB_DB_PROTECT_NONE` keeps today's format and code size (P4) |

Out of scope, and documented as limits [E §6.2]: an attacker who restores one
bucket to an older authentic image (UBI leaves the same gap, its row 9),
traffic analysis of ids, sizes and timing, side channels, fault injection,
and availability.

## 2. Decisions

| # | Decision | Alternatives rejected [E] |
|---|---|---|
| D1 | Protection lives in `blob_db` (L1), per slot | the store seam or UBI per block (§1, §3.1); a bucket seal as the main mechanism (§3.2); per value in kvdb (§3.3) |
| D2 | AUTH = AES-CMAC, AEAD = AES-CCM, both on AES-128 | AES-GCM (a repeated nonce leaks the authentication key); ChaCha20-Poly1305 (a second cipher family next to UBI's; kept only as a fallback if software AES measures badly) |
| D3 | Tag length 8 B by default, 16 B to match UBI exactly | 4 B (too weak); always 16 B (+8 B per slot) |
| D4 | Keep the CRC16, computed after the tag over everything before it: the CRC detects damage, the tag detects tampering | dropping it (compaction and the walk would need a key to recognise torn slots) |
| D5 | AEAD nonce = `mount_salt(4) ‖ seq(8)`; seq is a store-wide counter with a leading ceiling in the master | nonces derived from position (rollback would turn into nonce reuse); random per write (needs entropy per write) |
| D6 | AEAD compaction re-encrypts survivors under a nonce base in the bucket header (variant B) | copying verbatim (+12 B per compacted slot); one seal per region (reads must verify the whole region) [E §5] |
| D7 | MAC the master, bucket headers and scratch seal; only an erased bucket header means "unformatted" | trusting CRC'd metadata (remaps lose data silently) [E §4.8] |
| D8 | A protected store is format major **3** and records its suite; a build mounts only its own suite | a linear major (a protected build would accept plaintext slots: a downgrade) |
| D9 | `BLOB_DB_AUTOFORMAT_ON_CORRUPT` is forced off in protected builds | allowing it (corrupting both masters would become a factory reset) |

## 3. Design

### 3.1 Keys

```
IKM      = blob_db_ikm_key()                        PSA handle, USAGE_DERIVE, HKDF(SHA-256)
K_slot   = HKDF-SHA256(IKM, salt "blob_db/v1", info "blob_db/slot-mac/v1"  ‖ store_uuid)   AUTH
         | HKDF-SHA256(IKM, salt "blob_db/v1", info "blob_db/slot-aead/v1" ‖ store_uuid)   AEAD
K_meta   = HKDF-SHA256(IKM, salt "blob_db/v1", info "blob_db/meta/v1"      ‖ store_uuid)   CMAC
```

- The construction is `ubi_key.c`'s: a fixed compile-time salt,
  `info = label ‖ context`, and output into volatile AES-128 keys. Separate
  salts and labels keep these keys independent of UBI's
  (`"zephyr-ubi/v1"`, `"zephyr-ubi/header/v1"`, ...) under the same IKM.
- `store_uuid` takes the place of UBI's constant `key_context`. A counter
  nonce starts again after a reformat, so the key must change with each store
  incarnation [E §4.3]. A forged uuid only yields keys under which nothing
  verifies.
- On the UBI backend the same IKM handle keeps feeding UBI. Key acquisition
  moves from `blob_db_store_ubi.c` into the core and runs once per mount.
- The keys are created at mount and destroyed at unmount. Nothing in RAM
  holds key bytes outside PSA.

### 3.2 Nonces and the seq counter (AEAD)

- `seq` is a store-wide u64 counter, advanced once per appended slot. Its
  leading ceiling `seq_hint` sits in the master and follows the rules of
  `next_id_hint` (`l1_bucketlog.md` §13.1): mount resumes at the ceiling, and
  a new ceiling `BLOB_DB_SEQ_STEP` ahead is persisted *before* any value past
  the old one is used. Every master `blob_db` writes carries the current
  ceiling forward. It never writes the counter itself, for the reason
  `compact_commit()` gives for ids.
- `mount_salt` is 32 random bits from `psa_generate_random()` at each mount.
  If an attacker rolls back the master, the device resumes at an old ceiling,
  and the salt still keeps the nonces distinct, barring a 2⁻³² collision.
- An **appended** slot stores its nonce: `mount_salt ‖ LE64(seq)`.
- A **compacted** slot (§3.4) stores none. Its nonce is
  `cbase_salt ‖ LE64(cbase_seq + off)`, where `off` is the slot's byte offset
  in the bucket, and `cbase_seq` and `cbase_salt` come from the bucket header.
  Compaction reserves `[cbase_seq, cbase_seq + peb_size)` from the counter, so
  no appended slot ever draws a value in that range.

### 3.3 Slot formats (major 3)

```
flags: bit0 SEALED · bit1 TOMBSTONE · bit2 INDEXED · bit3 SEGMENT · bit4 COMPACTED (AEAD only)
slot_hdr.reserved stays 0 (kept for a key epoch, §9)

AUTH                slot_hdr(4) | id(8) |              payload[L]    | tag[T] | crc16(2)
AEAD appended       slot_hdr(4) | id(8) | nonce(12) |  P[L]          | tag[T] | crc16(2)
AEAD compacted      slot_hdr(4) | id(8) |              P[L]          | tag[T] | crc16(2)

AUTH tag    = CMAC_T(K_slot, slot_hdr ‖ id ‖ payload)
AEAD        = CCM_T(K_slot, nonce, AAD = slot_hdr ‖ id ‖ nonce [‖ seg_hdr], plaintext)
crc16       = CRC16-CCITT(0xffff) over every byte before it, tag included
```

- `val_len` = L, the plaintext length. CCM ciphertext is the same length.
- **SEGMENT slots in AEAD:** the first 12 B of P (`blob_db_seg_hdr`:
  `owner_id`, `seq`) stay in the clear and are added to the AAD. Only the
  chunk is encrypted. The mount-time segment sweep then finds owners without
  decrypting anything.
- A tombstone (L = 0) still carries a tag, and in AEAD a nonce.
- Overhead against today's 14 B, with T = 8: AUTH +8 B; AEAD appended +20 B;
  AEAD compacted +8 B. The walk computes `slot_size` from `flags` and
  `val_len` alone, as it does today.
- The per-slot payload cap becomes `(sector − DATA_OFF)/2 − overhead`. On
  4 KB sectors that is 2026 B today, 2002 B in AUTH and 1990 B in AEAD
  (`DATA_OFF` 16 → 48, §3.5). Mount already validates the cap against the
  geometry, and the large-payload chain check uses the new value.

### 3.4 Compaction (AEAD re-seal)

The image is still built in RAM. AEAD adds three steps:

1. Reserve the nonce range: `cbase_seq = seq`, `seq += peb_size`. If that
   passes the ceiling, persist a new ceiling first. On the scratch path it
   rides along in the COMPACTING master write compaction already makes. On
   the atomic-replace path it costs a master write once every
   `SEQ_STEP / peb_size` compactions.
2. Set `cbase_salt = mount_salt`. For each surviving slot: open it with its
   current nonce (stored, or derived from the *old* header), then reseal it
   under the derived nonce at its *new* offset with COMPACTED set. **A slot
   that fails to open is copied unchanged in its old form**, so reads of it
   keep returning `-EIO` (§3.7). Compaction never decides what tampered data
   means.
3. Write the new bucket header carrying `cbase_seq` and `cbase_salt`.

Recovery copies the staged image byte for byte, after checking the scratch
seal MAC (§3.5). AUTH compaction copies byte for byte as today: CMAC has no
nonce to share.

### 3.5 Metadata formats (major 3)

**Master** (`hdr_len` = 84; the frozen prefix is unchanged):

| Bytes | Field | Notes |
|---|---|---|
| 0..11 | compatibility prefix | `format_major = 3` |
| 12..15 | generation | |
| 16 | state | |
| 17..18 | compacting_bid | |
| 19 | **suite** | was `reserved0`; low nibble mode (1 AUTH-CMAC, 2 AEAD-CCM), high nibble T/4 |
| 20..27 | next_id_hint | |
| 28..35 | seg_owner | |
| 36..51 | **store_uuid** | 16 random bytes, drawn at format |
| 52..59 | **seq_hint** | the leading seq ceiling; 0 in AUTH |
| 60..63 | hdr_crc32 | over [0, 60), as today |
| 64..79 | **mac** | AES-CMAC-128(K_meta, [0, 64)) |
| 80..83 | **tail_crc32** | over [0, 80) |

**Bucket header** (48 B, so `DATA_OFF` = 48):

| Bytes | Field |
|---|---|
| 0..3 | magic `BDBH` |
| 4..5 | bucket_id |
| 6..7 | reserved, 0 |
| 8..11 | gen |
| 12..19 | cbase_seq (0 = none) |
| 20..23 | cbase_salt |
| 24..27 | reserved, 0 |
| 28..43 | mac: AES-CMAC-128(K_meta, [0, 28)) |
| 44..47 | crc32 over [0, 44) |

**Scratch seal** (40 B, at the sector tail):

| Bytes | Field |
|---|---|
| 0..3 | magic `BDSL` |
| 4..7 | image_len |
| 8..11 | image_crc32 |
| 12..13 | bucket_id |
| 14..15 | reserved, 0 |
| 16..19 | image gen |
| 20..35 | mac: AES-CMAC-128(K_meta, seal[0, 20) ‖ image[0, image_len)) |
| 36..39 | seal_crc32 over [0, 36) |

The scratch seal is the one place a MAC covers a whole bucket image. It is
checked once per compaction recovery, never on a read. This is the "seal over
the whole bucket" idea, used where it costs nothing.

**Check order**, everywhere: erased → magic and structure → CRC → MAC. A
CRC failure means damage; a CRC that passes over a failing MAC means
tampering. These are UBI's `header_verify()` verdicts. MACs are compared only
through `psa_mac_verify()` and `psa_aead_decrypt()`, in constant time.

### 3.6 Classification and mount

| Object reads as | Meaning | Action |
|---|---|---|
| master: prefix CRC fails, or a body or tail CRC fails | damaged | ignore it; the other master may win |
| master: CRCs pass, MAC fails | tampered | ignore it, raise TAMPERED; the other master may win |
| master: major 3 with a different suite, or major 1 or 2 in a protected build | foreign | `-ENOTSUP`, touch nothing |
| both masters unusable | — | `-EIO` (autoformat is forced off, D9) |
| bucket header all `0xff` | unformatted | `get` → `-ENOENT`; `update` formats it |
| bucket header: CRC fails | damaged | `-EIO` on every path, no format, raise CORRUPT |
| bucket header: CRC passes, MAC fails or another `bucket_id` | displaced or forged | `-EIO` on every path, no format, raise TAMPERED |

When the MAC is checked:
- **Mount** checks every bucket header's MAC during the scan it already
  makes: one CMAC over 28 B per bucket. A failure leaves the bucket out of the
  slot index and raises the event. Mount still succeeds, and that bucket
  answers `-EIO`.
- **Hit reads** do not re-check the header MAC. The tag of the slot returned
  already proves its content (R1), and for a compacted slot a forged
  `cbase` only makes that tag fail.
- **A read about to return `-ENOENT`** checks the header MAC first, so a
  displaced bucket answers `-EIO`, not "absent".
- **Writes** check the header before appending or formatting, unless the slot
  index already holds the bucket as verified since mount.

### 3.7 Slot verification and the read path

| Slot CRC | Slot tag | Meaning | `get` |
|---|---|---|---|
| ok | ok | committed data | data |
| fails | not checked | torn or rotten | fall back to the newest older slot, as today; raise CORRUPT |
| ok | fails | tampered | **`-EIO`**, no fallback; raise TAMPERED |

`slot_verify()` reads `[nonce] ‖ P ‖ tag ‖ crc` as **one** contiguous read
(today the payload and the CRC are two). It checks the CRC, then opens the
slot into `g_bbuf` (AEAD decrypts into the staging buffer, never into the
caller's `out`). For a compacted slot reached through a slot-index hit, it
first reads the 12 B `cbase` from the bucket header: one extra small read,
because the index caches no per-bucket crypto state (R5). `iterate` opens each
live slot into `g_bbuf_new` before calling back. `count`, `exists`, the walk
and mount read clear headers only.

The fallback on a CRC failure stays, because a torn write is
indistinguishable from damage after a reboot. An attacker who clears bits in
an id's newest slot therefore rolls that id back one version. That is the
same class as single-bucket rollback, and it is a documented limit [E §4.6].

### 3.8 Rollback: one state check for both layers (phase 2)

`blob_db_ubi_state_check()` becomes `blob_db_state_check()`. It is given
UBI's `ubi_device_info` when the backend has one, plus `store_uuid`,
`seq_hint` and `next_id_hint`, and returns TRUSTED or UNTRUSTED with UBI's
semantics: an UNTRUSTED mount fails with `-EROFS`. A companion
`blob_db_state_commit()` runs after every master write that raised a ceiling,
so an anchor in PSA ITS or a monotonic counter never runs ahead of the flash.
That catches a whole-image or master restore to within one ceiling step,
including the id ceiling, which closes id reuse through a replayed master. The
weak defaults trust everything, as UBI's do.

## 4. Cost summary (4 KB sectors; CPU figures are estimates [E §7])

| Op | Flash I/O vs today | Crypto | Notes |
|---|---|---|---|
| `get` hit | one read transaction fewer, +8 to +20 B; +1 small read for a compacted slot reached through the index | 1 CMAC or 1 CCM over the payload | est. +100–200 µs per 256 B in software CCM, about half that for CMAC, against 460 µs measured |
| `get` miss | + bucket header MAC | 1 CMAC over 28 B | |
| `update`, `delete` | slot +8 to +20 B | 1 tag or seal | ~1–8 % of 2.4–15 ms |
| `alloc_id` | unchanged | — | |
| compaction | unchanged writes; the AEAD range reservation rides the COMPACTING master | AEAD: open and reseal each survivor, ms per bucket | 0.69 s per compacting update measured |
| compaction recovery | unchanged | 1 CMAC over the image | rare |
| mount | header reads 16 → 48 B | 1 CMAC per bucket header; 1 HKDF per key | ~2045 small CMACs |
| space | AUTH +8 B per slot; AEAD +20 B appended, +8 B compacted; +32 B per bucket header | — | 256 B payload: +3.0 % (AUTH, or compacted AEAD), +7.4 % (appended AEAD) |
| RAM | +4 B salt, +8 B counter, 3 PSA key slots | — | nothing per blob or bucket |

Gate for phase 1: measured with `app_perf_blob_db` on the nRF5340-DK, before
the default tag length and the AEAD algorithm are frozen.

## 5. Crash analysis (P7)

| Crash during | Mount sees | Outcome |
|---|---|---|
| slot append | torn slot: CRC fails | as today: invisible, the older version stands. No seq value is reused, because the ceiling was persisted before use |
| ceiling master write | older master wins | the older ceiling still covers every seq value used (persisted ahead) |
| compaction, scratch path, any step | COMPACTING master | as in `l1_bucketlog.md` §6.1. Recovery checks the seal MAC before copying; a seal whose CRC fails means "unsealed", as today. The reserved nonce range is already under the persisted ceiling |
| compaction, atomic replace | old or new image | the range was reserved under a persisted ceiling before the image was built |
| reseal mid-compaction | nothing written yet | the image is built in RAM; nothing reaches flash before step 3 |
| `blob_db_state_commit` | anchor behind the flash | safe by construction: the anchor is committed after the master write |

## 6. Contract and API

**`l1_blob_db.md`**:
- §2 "No partial reads": `-EIO` also means "failed authentication" and
  "bucket displaced or damaged" in a protected build.
- R6: the payload cap is quoted per mode.
- One addition: in a protected build, `get` never returns bytes whose tag did
  not verify, and never reports `-ENOENT` for a blob in a bucket it cannot
  authenticate.

**New header `<app/lib/blob_db_security.h>`** (integration hooks, outside the
contract like `blob_db_ubi.h`):

```c
int  blob_db_ikm_key(psa_key_id_t *key_id);              /* BLOB_DB_KEY_APP: app defines it */

enum blob_db_event_kind { BLOB_DB_EVENT_CORRUPT, BLOB_DB_EVENT_TAMPERED };
enum blob_db_event_layer { BLOB_DB_EVENT_UBI, BLOB_DB_EVENT_MASTER,
                           BLOB_DB_EVENT_BUCKET, BLOB_DB_EVENT_SLOT };
void blob_db_event(enum blob_db_event_kind kind,          /* weak: log only */
                   enum blob_db_event_layer layer, uint32_t where);

struct blob_db_state { const struct ubi_device_info *ubi; /* NULL on flash_area */
                       uint8_t store_uuid[16]; uint64_t seq_hint, next_id_hint; };
bool blob_db_state_check(const struct blob_db_state *s);  /* weak: trust; phase 2 */
void blob_db_state_commit(const struct blob_db_state *s); /* weak: no-op; phase 2 */
```

`blob_db_ubi.h` keeps `blob_db_ubi_ikm_key()` and `blob_db_ubi_state_check()`
as deprecated wrappers for one release. UBI's `on_event()` forwards to
`blob_db_event()` with layer `UBI`.

## 7. Kconfig

```
choice BLOB_DB_PROTECT
    BLOB_DB_PROTECT_NONE   (default)   today's format, major 1 or 2
    BLOB_DB_PROTECT_AUTH               AES-CMAC per slot; selects PSA_CRYPTO, PSA_WANT_ALG_CMAC,
                                       PSA_WANT_ALG_HKDF, PSA_WANT_ALG_SHA_256, PSA_WANT_KEY_TYPE_AES,
                                       PSA_WANT_KEY_TYPE_DERIVE (UBI's set)
    BLOB_DB_PROTECT_AEAD               AES-CCM per slot; the above plus PSA_WANT_ALG_CCM
choice BLOB_DB_PROTECT_TAG_LEN         8 (default) | 16   (if !NONE)
config BLOB_DB_SEQ_STEP                int, default 65536 (AEAD; seq values per ceiling write)
choice BLOB_DB_KEY                     BUILTIN (default, NOT secret) | APP
                                       replaces BLOB_DB_UBI_KEY; feeds UBI too
BLOB_DB_AUTOFORMAT_ON_CORRUPT          forced n when !BLOB_DB_PROTECT_NONE
```

`BLOB_DB_KEY_BUILTIN` with AUTH detects damage only, and with AEAD only
obfuscates. The help text says so, as `BLOB_DB_UBI_KEY_BUILTIN`'s does.

## 8. Code and documentation impact

| Where | Change |
|---|---|
| `lib/blob_db/blob_db_protect.c` (new) | key acquisition and derivation; slot seal and open; meta MAC; seq and salt; events |
| `lib/blob_db/blob_db.c` | slot size from mode and flags; `append_slot2()` seals; `slot_verify()` does one read and opens; compaction reseals (AEAD); master, bucket header and scratch formats; the classification of §3.6 |
| `lib/blob_db/blob_db_internal.h` | major-3 structs, the COMPACTED flag, a mode-dependent `DATA_OFF` |
| `lib/blob_db/blob_db_store_ubi.c` | key acquisition removed (now in the core), events forwarded |
| `lib/blob_db/blob_db_slot_index.c` | "verified since mount" bucket state; `DATA_OFF` read from the core |
| `lib/blob_db/blob_db_inspect.c`, shell | show mode and suite; `dump` prints ciphertext in AEAD (no decrypting dump) |
| `include/app/lib/blob_db_security.h` (new), `blob_db_ubi.h` | §6 |
| `doc/layers/l1_blob_db.md`, `doc/impl/l1_bucketlog.md`, `doc/impl/l0_backends.md` | contract delta; the major-3 format; the backend key path |

## 9. Test plan (P8; `native_sim`, both backends)

1. **Round trips**: the full blob_db, contract, containers, kvdb and blobfs
   suites with `PROTECT_AUTH` and with `PROTECT_AEAD`, combined with large
   payloads and the slot index, as `.slot_index` does today.
2. **Slot tampering** (raw-offset injection): a changed payload byte with the
   CRC recomputed gives `-EIO`, no fallback, and TAMPERED; a slot copied to
   another id's bucket gives `-EIO`; a slot from a store with another uuid
   gives `-EIO`; a forged tombstone or INDEXED flag is rejected; a flipped
   COMPACTED flag is rejected.
3. **Damage stays benign**: a torn tail slot falls back to the older version,
   with CORRUPT.
4. **Remapping**: a bucket sector copied over another, two swapped, a forged
   header claiming the right id. Each gives `-EIO` on `get` and `update` and
   **erases nothing**. On UBI, repeat with the builtin UBI key and forged UBI
   headers, to show R3 holds without UBI's MAC.
5. **Master**: a forged lowered `next_id_hint`, or a forged COMPACTING state,
   is ignored (the other master wins); both masters forged gives `-EIO` and no
   format; a protected build refuses a major-1 or major-2 store, and an
   unprotected build refuses major 3.
6. **Nonces**: no (salt, seq) pair repeats across a crash at every
   ceiling-write point, at every compaction step on both compaction paths, or
   across repeated compactions of one bucket (a test hook records every
   nonce used).
7. **Compaction with a tampered survivor**: it is copied unconverted and
   still reads `-EIO`.
8. **Phase 2**: a restored older image fails the state check with `-EROFS`;
   a replayed master with a lower id ceiling is refused.
9. **Performance**: `app_perf_blob_db` on the DK in each mode; RESULTS.md
   gains a protection section.

## 10. Phases

| Phase | Content | Format |
|---|---|---|
| **0** | Separate from protection, for all builds: a non-erased bucket header that fails its check is `-EIO`, not "unformatted", and is reformatted only with `BLOB_DB_AUTOFORMAT_ON_CORRUPT=y`. Today one bit flip in a bucket header silently hides that bucket's blobs, and the next write to it erases them | none |
| **1** | Modes, keys, slot and metadata formats, classification, events, AEAD reseal | major 3 |
| **2** | `blob_db_state_check` and `blob_db_state_commit`, and a sample anchoring in PSA ITS | none (hooks only) |

Phase 1 defines the COMPACTED flag and the `cbase` fields from the start, so
the reseal never needs a second format change.

## 11. Open items

1. **Default tag length**: 8 B is proposed. 16 B matches UBI byte for byte at
   +8 B per slot. Decide after the phase-1 measurement.
2. **Software AES cost on reads**: if the DK measurement is unacceptable, the
   fallback is ChaCha20-Poly1305 for AEAD only (a new suite value), at the
   cost of a second cipher family.
3. **Selective protection**: `blob_db` is one store per partition, so
   protection covers all of it. Protecting only some kvdb stores would need a
   second `blob_db` instance, and `blob_db` is a singleton today.
4. **Key rotation**: `slot_hdr.reserved` can carry a key epoch later. AEAD
   compaction already reseals, which is where slots would move to a new key.
5. **zephyr-ubi**: a keyed MAC in place of `data_crc` on `ubi_leb_change()`
   blocks would give the stack an attach-time image seal for free [E §4.7].
   That change belongs in zephyr-ubi and is not needed here.

## 12. Implementation notes (phases 0 and 1)

Built as specified in §1–§9, with these differences. The format as built is
recorded in `doc/impl/l1_bucketlog.md` §3.2 (phase 0) and §3.6 (phase 1).

1. **Major 3 or 4.** A protected build declares major 3, or 4 with
   `LARGE_PAYLOADS`, and reads [3, its own]: the same reasoning that gives
   unprotected large-payload builds major 2, so a protected build without
   segments refuses a store that holds them.
2. **AAD is every clear byte before the ciphertext.** For an appended slot
   that is `slot_hdr ‖ id ‖ nonce [‖ seg_hdr]`, as §3.3 writes; a compacted
   slot stores no nonce, so its AAD is `slot_hdr ‖ id [‖ seg_hdr]`. Its
   derived nonce is still bound, as the CCM nonce.
3. **Where the bucket header MAC is checked.** Since PR #59, mount scans the
   buckets only when a master is in doubt, so §3.6's "mount checks every
   bucket header during the scan it already makes" holds only for that scan.
   Every writer checks the header before it appends or formats, and the slot
   index keeps no "verified since mount" state: one 48-byte read and one CMAC
   over 28 bytes per write, against 2.4–15 ms of write. Reads behave as §3.6
   says.
4. **Unformatted, precisely** (phase 0). A bucket is unformatted when its
   header is erased, or when a bad header sits over an erased first slot —
   what a format torn inside its header write leaves, where nothing can be
   lost. An unprotected store also honours a zeroed magic (erase_all's
   retirement mark). A protected store does not, since anyone can clear bits,
   so its `erase_all` erases each bucket it retires.
5. **AUTH compaction** copies kept slots byte for byte, as §3.4 says, but
   drops a tombstone only after its tag verifies: a forged tombstone would
   otherwise let compaction delete a blob.
6. **Compaction's nonce range** is reserved, and its ceiling persisted when
   needed, by a CLEAN master write of its own before the image is built, on
   both compaction paths. Riding the COMPACTING master write (§3.4 step 1)
   would save that write once every `SEQ_STEP / peb_size` compactions.
7. **Two authentic masters of different stores** (different uuids) refuse
   the mount with `-EIO` and TAMPERED: a format erases the other master, so
   this happens only if one was put there.
8. **`iterate`** visits every bucket it can read, skips a slot or bucket it
   cannot authenticate, and returns `-EIO` at the end. `count` reads clear
   headers only, as §3.7 says.
9. **Tests**: §9 items 1–7 are in `tests/lib/blob_db/src/protect.c` and the
   protected scenarios of every suite's `testcase.yaml`. Item 6 records every
   nonce across remounts and repeated compactions; a power cut at each
   ceiling write is covered by the crash argument of §5, not by injection.
   Item 9 (DK measurements) is not done, so §11.1 and §11.2 stay open.

