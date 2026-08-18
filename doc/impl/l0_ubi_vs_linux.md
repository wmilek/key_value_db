# UBI as blob_db's L0 — the delta against Linux UBI

Status: v1 · **Non-normative analysis.** Nothing above L0 may depend on
anything here (P6). Companion to `doc/layers/l0_flash.md` (the L0 contract)
and `doc/impl/l1_bucketlog.md` (the allocator this substrate carries).

---

## 1. What is being compared

**Linux UBI** — `drivers/mtd/ubi/` in the mainline kernel: `attach.c` (scan),
`eba.c` (LEB→PEB association), `wl.c` (wear-leveling + the `ubi_bgt` erase
thread), `vtbl.c` (volume table), `fastmap.c`, `kapi.c` (the in-kernel API
declared in `include/linux/mtd/ubi.h`), over MTD.

**This project's UBI** — the `ubi` west module pinned in `west.yml` to
`wmilek/ubi @ feature/leb-partial-update`, a fork of `kamil-kielbasa/ubi`. It
is a from-scratch Zephyr implementation of the same model (PEB/LEB, EC and VID
headers, an EBA, sequence-number recovery, bad-block retirement) over Zephyr's
`flash_area` API. Only its *plain* backend is in play here.

**The seam between them and us** is one file: `lib/blob_db/blob_db_store_ubi.c`
(220 lines), implementing `lib/blob_db/blob_db_store.h`. blob_db itself
contains no UBI code — it addresses a flat array of PEBs, and the shim
translates `off → (lnum = off / leb_size, within = off % leb_size)`.

The module ships its own operation-by-operation API map against Linux
(`doc/reference/linux_ubi_comparison.md` in the `ubi` repo). This document does
not repeat it. It asks the narrower question the stack actually cares about:
**which of those deltas change how blob_db is built, how fast it runs, and
whether it is correct** — and where Linux's design would do better.

Measurements quoted throughout are from `app_perf/RESULTS.md` (nRF5340-DK,
MX25R6435F QSPI NOR, 8 MB partition, 64 KB sectors) unless stated otherwise.

---

## 2. The deltas that reach blob_db

| # | Delta vs Linux UBI | For blob_db |
|---|---|---|
| 3.1 | Offset-based in-place LEB write (`ubi_leb_write_at`) restored to Linux parity | **enabling** — the whole design rests on it |
| 3.2 | Everything synchronous; no background thread, no `ubi_sync`/`ubi_flush` | **good** — L0's durability rules hold by construction |
| 3.3 | Static slab memory, EBA as an rb-tree of *mapped* LEBs only | **good** — keeps blob_db's O(1)-RAM promise end to end |
| 3.4 | 48 B of per-PEB header vs Linux's 128 B (up to 4 KB after NAND padding) | **good** — bigger buckets, fewer compactions |
| 3.5 | Formats a blank partition on first use; volumes created idempotently by name | **good** — no factory provisioning step |
| 3.6 | Bad-PEB retirement + erase-torture recovery on NOR, not just NAND | **good** — the reason UBI is the default |
| 3.7 | Degraded read-only mode that self-heals | **good** — healed by the loop blob_db already runs |
| 4.1 | No `ubi_leb_erase`; `unmap` leaves the old PEB re-attachable | **a correctness bug today** |
| 4.2 | No asynchronous erase — reclamation runs on the caller's thread | **costly** — the erase stays on the critical path |
| 4.3 | No fastmap — attach rescans every PEB | **costly at scale** — a second O(device) mount pass |
| 4.4 | Passive wear-leveling only; live data is never relocated | **costly** — cold buckets pin PEBs out of circulation |
| 4.5 | UBI reserves nothing; blob_db holds back 4 PEBs itself | **fragile** — a scale-blind constant |
| 4.6 | Secure (AEAD) mode exists — but not for in-place writes | **closed door** — mutually exclusive with 3.1 |

---

## 3. Deltas that make blob_db better

### 3.1 In-place LEB append — the one the design rests on

Linux splits LEB mutation in two: `ubi_leb_write(desc, lnum, buf, offset, len)`
programs in place at increasing offsets (dynamic volumes, not power-fail
atomic), and `ubi_leb_change()` replaces a whole LEB atomically via a fresh
PEB. The Zephyr port shipped only the second, under the name `ubi_leb_write`.
The first is what `feature/leb-partial-update` adds back as
`ubi_leb_write_at()` — which is why `west.yml` pins a branch rather than a
release tag.

blob_db needs exactly the Linux-`ubi_leb_write` shape. A bucket is an
append-only slot log: erase the sector, write a 16-byte header, then append
14+N-byte slots at a growing cursor (`doc/impl/l1_bucketlog.md` §3.2). With one
LEB per bucket that is a direct translation:

```
blob_db_store_erase(peb)          -> ubi_leb_unmap(lnum)  [+ reclaim]
blob_db_store_write(peb, off, ..) -> ubi_leb_write_at(lnum, off, ..)
blob_db_store_read (peb, off, ..) -> ubi_leb_read(lnum, off, ..)
```

Without it the shim would have to emulate an append with whole-LEB replace:
read the 64 KB LEB, splice in a 38-byte slot, write all 64 KB back onto a fresh
PEB. That is not a constant-factor difference — **it changes the asymptotics of
the substrate**. Every `update()` would consume a free PEB and dirty the old
one, so blob_db's append (today: zero erases; compaction is the only eraser)
becomes *one erase per update*. At the measured 1.09 s per 64 KB sector erase,
`update` goes from 3.6 ms to over a second, and the store's endurance budget is
spent ~50× faster (49 slots per bucket at 100 k blobs, all now erase-backed).

The corollary is worth stating too: **blob_db never uses UBI's atomic whole-LEB
replace.** It uses only `write_at`, `read`, `unmap`. Atomicity comes from
blob_db's own machinery — per-slot CRC16, a double-buffered master with a
generation counter, a sealed compaction scratch. Deliberately taking the weaker
primitive means not paying for atomicity twice; it is the same division of
labour UBIFS makes on Linux.

### 3.2 Nothing is deferred

There is no thread and no work queue anywhere in the module (`grep -rn
'k_thread\|k_work' lib/src/` → nothing). Every call completes its flash I/O
before returning. Linux instead runs `ubi_bgt%d`, queues erases from
`ubi_leb_unmap()`, and exposes `ubi_sync()`/`ubi_flush()` for callers that need
the queue drained.

Three L0 rules (`doc/layers/l0_flash.md` §6) are then satisfied by construction
rather than by discipline: no RAM caching acknowledged before durability, no
reordering of a write past a later write's acknowledgment, and geometry stable
for the mount. blob_db's master-generation ordering — the thing that decides
which of masters A/B wins after a crash — cannot be reordered by a queue that
does not exist. There are no `ubi_flush()` call sites to get wrong, no
priority-inversion story, and `deinit` is a mutex acquire and a free.

The bill for this arrives in §4.2.

### 3.3 Memory that does not scale with the store

Linux allocates a `struct ubi_wl_entry` per PEB plus a `lookuptbl` of pointers,
and a **flat** EBA array per volume sized to its LEB count. Here every
allocation comes from compile-time `K_MEM_SLAB` pools (`CONFIG_UBI_MEM_BACKEND_STATIC`),
at 16 B per PEB, and the EBA is a red-black tree keyed by `lnum` holding nodes
only for LEBs that are actually **mapped**.

That second point matters for blob_db specifically. Buckets are mapped lazily —
an untouched bucket has no PEB — so on a sparsely filled store UBI's RAM tracks
live buckets, not partition size. The measured total cost of the backend on the
DK (126 PEBs) is **+3 408 B of RAM**, and blob_db's contract promise of O(1) RAM
per operation survives all the way down to L0.

### 3.4 48 bytes of header, not 4 KB

| | Linux UBI | this UBI |
|---|---|---|
| EC header | 64 B, padded to `min_io_size` | 16 B |
| VID header | 64 B, at `vid_hdr_offset`, padded | 32 B |
| Typical NAND (2 KB page, 128 KB PEB) | `leb_start` = 4096 B → **3.1%** | 48 B → **0.04%** |
| This board (64 KB NOR sector) | — | 48 B → LEB = 65 488 B (**99.93%**) |

Bucket capacity is slot count between compactions, and compaction is the
erase-bound operation (`prepare`: 1 076 300 µs/op). Header overhead converts
directly into compaction frequency, so on NAND-class geometry the smaller
headers are worth several percent of the store's whole erase budget.

It also validated a design rule that had not been under load before: the LEB
size is 65 488 — not a power of two, not the sector size. blob_db treats
geometry as a runtime property (P2, `lib/blob_db/blob_db.c:453`), so the odd
number costs nothing. A store that had baked in `1 << 16` would not have
accepted this substrate at all.

### 3.5 It formats itself

Linux will not format an MTD device for you: `ubiformat`/`ubinize` prepare the
image and `ubiattach` binds it, all out of band. Here `ubi_device_init()`
formats a blank partition on first use and `ubi_volume_create()` is idempotent
by name, so `blob_db_store_open()` self-provisions on first boot
(`blob_db_store_ubi.c:116`). For firmware that ships on blank flash, that
removes a production step outright.

Two rough edges follow from the missing "open by name": the shim probes volume
ids 0..15 comparing names (`find_existing_volume()`), and each miss logs at
`<err>` — an operational trap already documented in `app_perf/RESULTS.md`.

### 3.6 Bad blocks are handled on NOR, and it has been observed

Linux's bad-block machinery is NAND-shaped: an MTD bad-block table, a reserve
pool sized by `CONFIG_MTD_UBI_BEB_LIMIT` (default 20 per 1024 PEBs), and
torture on suspicious erases. On NOR there is effectively nothing to hook.

This implementation retires a PEB on *any* I/O error regardless of technology,
and `ubi_device_erase_peb()` will attempt to bring a retired PEB back via erase
torture. That is not theory here — from `RESULTS.md`:

> A reflash interrupted a UBI write; the next boot logged `EC header corrupt on
> PEB 78` and then `Torture recovered PEB 78`, self-healing in 2.3 s. The same
> interruption on `flash_area` leaves the mx25r64 answering a null JEDEC id and
> needs `nrfutil device recover`.

This single behaviour is the stated justification for making UBI the default
backend, against a measured 1.5–2.5× read regression.

### 3.7 Degraded mode that heals

Linux has `ubi_ro_mode()`: a one-way latch into read-only after a fatal error,
cleared only by detach/reattach. Here `read_only_degraded` is raised when the
reserved-PEB bank loses redundancy, and *cleared* when `ubi_device_erase_peb()`
succeeds in rewriting the damaged copy from the survivor.

The lucky part for blob_db is that `ubi_device_erase_peb()` is already on its
erase path (`blob_db_store_erase()`), so a store that compacts occasionally
repairs its own metadata bank with no extra code. The unlucky part is in §4.2:
a store that only *reads* never calls it.

---

## 4. Deltas that cost blob_db

### 4.1 `unmap` is not an erase — and there is no `ubi_leb_erase`

**This is a live data-loss window, not a performance note.**

Linux distinguishes three operations, and its kernel-doc is explicit about why:

| Linux | Guarantee |
|---|---|
| `ubi_leb_unmap()` | schedules background erasure; **does not** guarantee 0xFF after an unclean reboot |
| `ubi_leb_erase()` | unmap **plus synchronous erase** |
| `ubi_leb_map()` | maps a fresh PEB; **guarantees** 0xFF even after an unclean reboot |

This implementation has `unmap` and `map` but no `ubi_leb_erase`. And `unmap`
writes nothing to flash (`ubi_plain_leb.c:500` — it reads the old EC header,
removes the EBA node, and moves the item to the dirty pool in RAM). The old PEB
keeps its VID header claiming `(vol_id, lnum)`. Attach re-scans every PEB and
maps any valid claimant (`map_leb_first_occurrence()`,
`ubi_plain_core_init.c:513`; duplicates lose to a higher `sqnum`), so **an
unmapped-but-not-yet-reclaimed PEB comes back on the next boot with its old
contents.**

`blob_db_store_erase()` tries to cover this with a bounded reclaim loop after
the unmap — but `ubi_device_erase_peb()` erases the **lowest-EC dirty PEB**,
which is not necessarily the one just unmapped. The LEBs blob_db erases most
(master A/B, scratch) are precisely those whose PEBs carry the *highest* erase
counts, so they are the *least* likely to be the ones physically erased. The
header's stated contract — "After erase the region reads back as the erased
value (0xff)" (`blob_db_store.h`) — is therefore not honoured across a power
cut on this backend.

Whether that matters depends on which erase:

- **Master sectors: safe.** `write_master()` erases the inactive slot then
  writes it. A resurrected old master carries a *lower* generation than the
  active one, and generation ordering decides the winner — so the stale copy
  loses exactly as intended.
- **Compaction scratch: not safe.** Phase 2 is `master := COMPACTING(bid)` →
  *erase scratch* → write image → seal → erase bucket → restore → erase scratch
  → `master := CLEAN` (`blob_db.c:1205`). Mount recovery restores the bucket
  from scratch whenever the seal verifies and the header's `bucket_id` matches
  (`recover_compaction()`, `blob_db.c:1347`). There is **no generation tying the
  scratch image to this compaction.**

  So: compact bucket X once; its sealed image is left on the scratch PEB, which
  step 4 unmaps but does not necessarily erase. Later, compact X again. Power is
  cut between step 2's unmap and the first byte of the new image — a window that
  contains the inline reclaim erase, i.e. up to ~1.09 s of it. On reboot the
  stale scratch PEB re-attaches, the master says `COMPACTING(X)`, the seal
  verifies, the bid matches — and recovery overwrites bucket X with the
  **previous** compaction's image, discarding every slot appended since.

  On `flash_area` this cannot happen: the erase is physical, scratch reads
  0xff, the seal fails, and recovery correctly leaves the bucket alone.

**The fix is one line, and Linux names it for us.** `ubi_leb_map()` after the
unmap allocates a free (already erased) PEB and writes a VID header with a
fresh `global_sqnum`; attach takes `max(sqnum)+1` across every PEB
(`ubi_plain_core_init.c:679`), so the new empty mapping beats the stale one by
sequence number and the LEB reads 0xff. Cost: one free PEB and one 32-byte
header write per erase — no extra flash erase. The alternative, and the better
long-term answer, is to add a real `ubi_leb_erase()` to the module, closing the
last `⚠️` row in its own Linux comparison table.

### 4.2 The erase runs on blob_db's thread

Linux returns from `ubi_leb_unmap()` immediately and lets `ubi_bgt` do the
erase; a caller pays only when the free pool runs dry, or when it explicitly
asks via `ubi_flush()`. Here `blob_db_store_erase()` drives
`ubi_device_erase_peb()` inline, so the full sector erase lands in the caller's
latency.

The measurements say what that costs, and what it doesn't:

| | `flash_area` | UBI |
|---|--:|--:|
| `prepare` (compaction-bound) | 1 096 500 µs/op | 1 076 300 µs/op |
| `lg rewrite` | 4 479 500 µs/op | 4 497 500 µs/op |

Identical — "**UBI moves erase cost in time rather than removing it**", and on
this path it does not even move it. The decomposition of a warm 64 KB write is
~55% erase, ~45% page programming, write amplification 1.02×: blob_db writes
almost exactly the bytes asked of it, so **erase is the only remaining lever**,
and Linux's asynchronous model is the one design that pulls it off the caller.
This is the largest unclaimed performance win in the stack — worth a thread and
a work queue, but only after §4.1 is answered, because a background eraser
makes the unmap/erase durability gap wider, not narrower.

A second, smaller consequence: because reclamation only runs when blob_db
erases, a read-mostly deployment never calls `ubi_device_erase_peb()` — so the
self-healing of §3.7 never fires and the dirty pool is never drained. A periodic
call from the application (or a maintenance entry point on blob_db) would fix
both.

### 4.3 No fastmap — mount is O(device) twice

Attach reads the EC and VID headers of every PEB and rebuilds the whole EBA.
Linux has the same scan but also `CONFIG_MTD_UBI_FASTMAP` (default `n`, but it
exists), a persistent checkpoint that attaches "in nearly constant time".

blob_db then does its own O(device) pass: mount reads every bucket to recover
write cursors and the id ceiling (`doc/impl/l1_bucketlog.md` §1 — 2045 bucket
reads on native_sim). So boot is now **linear in partition size twice over**,
and the second pass pays UBI's inflated 178 µs per transaction (vs 65.5 µs on
`flash_area`).

On this board the numbers are small — 126 PEBs, tens of milliseconds — but the
scaling claim in the README deserves an asterisk. "Nothing is rebuilt at mount"
is still true of blob_db's *own* structures; it is no longer true of the stack
as a whole, because L0 rebuilds a full LEB→PEB map on every boot. On a
2048-PEB NAND that becomes the boot budget, and fastmap is the known answer.

### 4.4 Passive wear-leveling pins cold PEBs out of circulation

Linux does two things this implementation does not:

- **Threshold-driven relocation.** When `max(EC) − min(EC)` exceeds
  `CONFIG_MTD_UBI_WL_THRESHOLD` (default 4096), it *moves static data* off a
  low-EC PEB so that block re-enters the churn.
- **Scrubbing.** A correctable bit-flip on read (`-EUCLEAN` from MTD) queues the
  PEB for relocation before the error becomes uncorrectable.

Here wear-leveling is greedy-min on both sides — allocate the lowest-EC free
PEB, reclaim the lowest-EC dirty PEB — and **a PEB holding live data is never
moved**.

blob_db's access pattern makes that gap sharper than average. Master A and B are
erased and rewritten on every state change (`write_master()` erases first,
`blob_db.c:272`), and the scratch sector is erased twice per compaction. Those
3 LEBs of 128 absorb a large share of all erases. Meanwhile the great majority
of buckets are written once and then read for the life of the device.

The hot LEBs themselves wear evenly — each cycle takes a fresh min-EC PEB — but
the *pool they cycle through* is only the free PEBs plus whatever compaction
releases. PEBs sitting under cold buckets never rejoin it. Endurance is
therefore bounded by that working subset while the cold majority stays at EC≈1,
which is precisely the imbalance Linux's threshold relocation exists to correct.
A store whose data is mostly cold is the worst case for passive-only WL, and
that is exactly the store this project builds.

### 4.5 The reservation policy is inverted

Linux makes reservation UBI's job: it holds back `beb_rsvd_pebs` — scaled from
`CONFIG_MTD_UBI_BEB_LIMIT`, 20 per 1024 PEBs ≈ 2% — plus a WL reserve, and
reports the volume size that remains.

Here UBI reserves nothing for churn, and the client compensates:
`BLOB_DB_UBI_SPARE_PEBS = 4`, hard-coded in `blob_db_store_ubi.c:39`. That
constant is **3.1%** of the DK's 128 PEBs and **0.2%** of native_sim's 2048 —
scale-blind in the direction that matters, since bad blocks accrue with device
size while churn headroom does not. Past four retired PEBs the volume can no
longer remap and writes start failing `-ENOSPC`, with nothing watching
`bad_peb_count` to warn first.

A related fragility, currently benign: `n_buckets = n_pebs − 3` and a blob lives
in bucket `id % n_buckets` (`blob_db.c:733`), so `leb_count` must be identical
on every boot or every blob moves. The shim recomputes it from
`info.total_peb_count` at each open. That happens to be safe — `total_peb_count`
is pure geometry, `nr_of_pebs − RES_PEBS` (`ubi_plain_core_init.c:853`), with
bad PEBs counted separately — but the field is documented as "total **usable**
data PEBs", which reads exactly like something that should shrink when a PEB is
retired. The store's stability rests on an internal definition rather than a
contract. `find_existing_volume()` already reads the attached volume's config
and throws it away; taking `leb_count` from there instead would make the
invariant structural.

### 4.6 Secure mode and in-place writes are mutually exclusive

The module has something Linux UBI has no counterpart for: a secure backend
that AEAD-wraps every commit-visible structure, LEB payloads included, binding
`data_size` into the AAD. Because an in-place partial write cannot be expressed
under a whole-LEB AEAD envelope, `ubi_leb_write_at()` returns `-ENOSYS` there
(`ubi.c:150`).

So the one feature that beats Linux is the one blob_db cannot use, precisely
because of the choice in §3.1. "Encrypted blob_db on secure UBI" would mean
going back to whole-LEB replace and paying the erase-per-append cost computed
in §3.1 — worth knowing before anyone plans it.

---

## 5. Against the L0 contract

`doc/layers/l0_flash.md` §2 and §6 state what L1 relies on. How each provider
scores:

| L0 requirement | `flash_area` | UBI (this) | Linux UBI |
|---|---|---|---|
| read returns last committed bytes | ✅ | ✅ | ✅ |
| a completed write is durable | ✅ | ✅ (synchronous) | ⚠️ needs `ubi_sync`/`ubi_flush` |
| erase → reads back 0xff | ✅ | ❌ **not across a power cut** (§4.1) | ✅ via `ubi_leb_erase` / `ubi_leb_map` |
| geometry stable across mounts | ✅ | ✅ (by internal definition, §4.5) | ✅ |
| torn-write blast radius ≤ 1 sector | ✅ | ✅ for data; UBI metadata is extra surface, covered by dual-bank + CRC + degraded mode | ✅ same |
| no RAM caching before ack | ✅ | ✅ | ⚠️ deferred erase queue |
| no reordering past an ack | ✅ | ✅ | ⚠️ background thread |

Two observations. First, the row that fails is the one §4.1 is about, and it
fails *only* on this backend — the substrate adopted for robustness is currently
weaker than the raw one on a contract L1 depends on. Second, Linux's three ⚠️
rows are all consequences of asynchrony: the feature we want in §4.2 is the
same feature that would put those warnings in our column too. Getting it means
adopting Linux's answer as well (`ubi_leb_erase` for the cases that need the
guarantee, `map`'s 0xff guarantee for the rest), not just its thread.

`doc/layers/l0_flash.md` §4 still describes the UBI provider as future work with
a design "to be specified when scheduled". It landed, it is the default
(`CONFIG_BLOB_DB_BACKEND_UBI`), and the design lives in the `ubi` module's own
`doc/`. That section needs a status correction.

---

## 6. What to do, in order

1. **Close §4.1.** Call `ubi_leb_map()` after `ubi_leb_unmap()` in
   `blob_db_store_erase()`, or add `ubi_leb_erase()` to the module and use it.
   One line, no on-flash format change, removes a data-loss window. Add a test
   that unmaps a LEB, re-attaches without reclaiming, and asserts 0xff.
2. **Take `leb_count` from the attached volume** rather than recomputing it
   (§4.5) — makes the hash modulus structurally stable instead of incidentally
   stable.
3. **Make the spare reserve proportional and configurable**, and surface
   `bad_peb_count` (§4.5), so the device warns before it wedges.
4. **Run reclamation off the erase path** — a periodic `ubi_device_erase_peb()`
   drains the dirty pool and heals the reserved bank on read-mostly devices
   (§3.7, §4.2).
5. **Asynchronous erase** (§4.2) is the biggest performance item left in the
   stack — ~55% of a large write — but it needs 1. done first.
6. **Revisit wear-leveling** (§4.4) before shipping on flash where endurance
   binds: cold-data relocation is the missing half, and blob_db's read-mostly
   profile is its worst case.
7. **Fastmap-equivalent** (§4.3) only if the target grows past a few hundred
   PEBs.
8. **Correct `doc/layers/l0_flash.md` §4** (§5).

Items 1–3 are small and local to `lib/blob_db/blob_db_store_ubi.c`. Items 5–7
are changes to the `ubi` module, i.e. upstream work, and each one is a feature
Linux UBI already has.
