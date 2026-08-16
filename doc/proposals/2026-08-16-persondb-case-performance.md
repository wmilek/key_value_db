# Improving the cost of `app_cbor_persondb`'s benchmark cases

**Status: investigation. Nothing here is implemented.** Every number is
modelled or harness-measured; none is from hardware. The recommendations end
with what would have to change and what it would cost, so the decision — which
includes a store reformat and a re-taking of `RESULTS.md` — stays with whoever
makes it.

Harness, and how to re-derive all of this:
[`doc/proposals/persondb-perf/`](persondb-perf/README.md).

---

## 1. The short version

`app_cbor_persondb`'s five benchmark cases (`RESULTS.md` §4) are dominated by
two terms, and the application controls both of them through parameters it has
never varied:

| Term | What sets it | What the app does today |
|---|---|---|
| **flash transactions** — ~65.5 µs each, **67 %** of a decision at full scale | the number of blobs sharing a 64 KB `blob_db` sector | asks for the largest possible `kvhash` map, which maximises the blob count |
| **flash bytes** — ~0.63 µs/B, **33 %** | the `kvhash` directory (re-read on *every* operation) plus the bucket | asks for `initial_capacity = SIZE_MAX`, which maximises the directory at 4 096 B |

Both come from one line in `persondb.c`:

```c
const struct map_config cfg = { .initial_capacity = SIZE_MAX };
```

The comment above it explains the intent — "give me the largest map you can
build", to stay far from `K2`'s per-bucket cliff. That is a real constraint and
the sizing history behind it (`RESULTS.md` §9) is sound. But the maximum bucket
count is *also* the maximum directory and the maximum blob count, and those are
paid on every read the product ever performs.

Harness-measured, at the headline 10 000-person scale, times modelled with
`app_perf`'s fitted DK constants:

| configuration | `check` | `byid` | `miss` | cost |
|---|--:|--:|--:|---|
| **shipped** — 4 KB payload, 16 maps × 511 buckets, 1 credential map | **25.7 ms** | **12.5 ms** | **13.2 ms** | — |
| choose the directory size: 32 × 256, 4 credential maps × 128 | 22.4 ms | 11.5 ms | 11.0 ms | none — a reformat |
| …and raise the payload ceiling to 8 KB: 16 × 256, 4 × 128 | 18.8 ms | 9.5 ms | 9.5 ms | +12 KB RAM |
| …to 16 KB: 16 × 128, 4 × 64 | **12.7 ms** | **6.5 ms** | **6.3 ms** | +40 KB RAM, ~2× write bytes |

**Halving every read case is available, and the price is RAM and write
amplification, not correctness.** Nothing here denormalizes anything, so
`DESIGN.md` D2 and `README.md` practice 6 are untouched: `check` is still
`card_owner` + `person_get`, and the person record is still the single copy of
the truth.

Three things fell out along the way that are worth acting on independently of
any of the above:

- **A latent out-of-bounds write.** `CONFIG_APP_CBOR_PERSONDB_PEOPLE_MAPS`
  accepts `1..64`; `struct superblock` holds `people_root[16]`. §8.1.
- **A hash trap waiting for anyone who sharded the credential index** the
  obvious way. It cost this investigation an `-ENOSPC` before it was
  understood. §6.
- **A prediction for the outstanding `A4` run.** The harness says the
  full-scale DK `check` should land near **26.6 ms**, not the ~24 ms
  `README.md` currently projects. §3.3.

---

## 2. Method, and whether to believe it

There is no Zephyr tree in the environment this was done in, so the numbers do
not come from a build of the application. They come from a host harness that
compiles **the real `lib/blob_db/blob_db.c` and
`lib/containers/kvhash/kvhash.c`** against stub Zephyr headers and a RAM-backed
NOR model with the DK's exact geometry (8 MiB, 64 KB erase blocks, 4-byte write
alignment, program is AND), and replays persondb's own key formats, shard hash
and record lengths over them.

It does not encode CBOR; it writes blobs of exactly the length
`person_cbor_encode()` would produce. Flash traffic depends on key and value
*lengths* only, so nothing that matters is lost — and the omission is why a
`check` figure from the harness must have the measured 0.95 ms of codec added
before it is compared with a board.

This is the `doc/proposals/sizing/` pattern: not project code, built by its own
`measure.sh`, existing so the tables can be re-derived.

### 2.1 It reproduces both published runs

`sh doc/proposals/persondb-perf/measure.sh validate`:

| | measured | harness | Δ |
|---|--:|--:|--:|
| **DK, 1 000 persons** (`RESULTS.md` §8a) | | | |
| `check` flash ops/op | 112.1 | 113.0 | +0.8 % |
| `check` flash bytes/op | 10 030 | 10 042 | +0.1 % |
| `byid` flash ops/op | 53.8 | 52.5 | −2.4 % |
| `byid` flash bytes/op | 5 135 | 5 119 | −0.3 % |
| `miss` flash bytes/op | 4 846 | 4 871 | +0.5 % |
| **`native_sim`, 10 000 persons** (`RESULTS.md` §4) | | | |
| `check` flash ops/op | 274 | 264 | −3.6 % |
| `check` flash bytes/op | 13.4 KB | 13.3 KB | −0.7 % |
| `byid` flash ops/op | 131 | 128.5 | −1.9 % |
| `put` flash ops/op | 815 | 810 | −0.7 % |

And the time model closes the loop the same way `RESULTS.md` §5 does. Applying
`app_perf`'s independently fitted constants (~65.5 µs/transaction, ~0.63 µs/B)
to the harness's own counters for the DK 1 000-person configuration gives
13.73 ms; adding the measured 0.95 ms of CBOR gives **14.68 ms against a
measured 14.605 ms — 0.5 % out.**

The one row that does *not* match is `put`: harness 378.6 flash ops against 318
measured, 4.32 map ops against 3.32. That is expected and is not a harness
error — the DK run predates the replace fix, which `RESULTS.md` §5b flags in
place. The harness carries the fix, and its 4.32 map ops match the post-fix
`native_sim` figure of 4.3 in §4.

### 2.2 What it cannot tell you

- **Wall-clock is modelled, never measured.** For the read-only cases the model
  uses only the two constants `app_perf` fitted on the board, which is the same
  arithmetic `RESULTS.md` §5 validates. For `put` and `fill` it also needs a
  *write* constant that nobody has fitted against hardware; that column is
  indicative, and the op / byte / erase counts beside it are the results.
- **`put` and `fill` carry real run-to-run variance,** because whether a
  compaction lands inside the sample window moves them — the same effect
  `RESULTS.md` §3b records as ±20 % on the write path. Two configurations with
  identical cell counts differed by ~10 % in the read-op column for the same
  reason. Conclusions below rest on effects of 25–50 %, well clear of that;
  the smaller differences within a payload tier should not be over-read.
- **Nothing here has been near a board.** §9 says what to measure first.

---

## 3. Where a decision's time actually goes

### 3.1 The two terms

At 1 000 persons on the DK, a `check` is 14.605 ms:

| | | share |
|---|--:|--:|
| 112 flash read transactions × 65.5 µs | 7.34 ms | **50 %** |
| 10 030 B × 0.63 µs/B | 6.32 ms | **43 %** |
| CBOR encode + decode | 0.95 ms | 6.5 % |

At 10 000 persons the transaction term grows faster than the byte term, because
the store holds proportionally more blobs: 264 transactions is 17.3 ms of a
25.7 ms decision (**67 %**).

### 3.2 Why there are 264 transactions

`blob_db` locates a blob by walking its bucket sector one slot header at a time
(`scan_bucket_for`), because a slot's size comes from its own header and there
is no in-sector index. So **one lookup costs one flash transaction per blob
sharing that 64 KB sector**, plus two to read and verify the target.

A `check` is 2 map gets; a `kvhash` map get is 2 `blob_db` gets (the directory,
then the bucket — `FINDINGS.md` K11). So a decision is **4 sector walks**.

A census of the filled store (`HB_CENSUS=1`) confirms the arithmetic exactly:

| configuration | cells | slots per sector | dead | predicted `check` ops (4 × (slots + 2)) | measured |
|---|--:|--:|--:|--:|--:|
| 4 KB payload, 16 × 511 + 1 × 511 | 8 687 | **64.4** | 23 % | 266 | **264.2** |
| 8 KB payload, 16 × 256 + 4 × 128 | 4 608 | **46.7** | 28 % | 195 | **211.0** |
| 16 KB payload, 16 × 128 + 4 × 64 | 2 304 | **25.8** | 28 % | 111 | **123.9** |

Two things follow, and the second is the one that changes what to do:

1. **Transactions per decision are set by blobs per sector, and blobs per
   sector is set by the number of `kvhash` buckets.** Fewer, larger buckets is
   the whole lever.
2. **Compaction is not the lever.** Only 23–28 % of the slots walked are
   superseded. A post-provisioning compaction pass — the obvious idea, and one
   the stack has no API for — would remove at most a quarter of the walk. It
   was worth measuring before proposing; it is not worth proposing.

### 3.3 A prediction for `A4`

`RESULTS.md` has never measured the full 10 000-person configuration on
hardware; `README.md` projects "near 24 ms" for `check` from the tenth-scale
run. The harness's counters at full scale, through the same fitted constants,
give 25.69 ms of flash plus 0.95 ms of codec: **≈ 26.6 ms**. If the A4 run
lands materially below that, this harness is wrong somewhere and the rest of
this document should be re-read with that in mind.

---

## 4. Lever 1 — choose the directory size instead of maximising it

`kvhash` reads the whole directory blob on every `get`, `set` and `del`. The
directory is `8 + 8 × n_buckets` bytes: at the maximum 511 buckets that is
**4 096 B, re-read on every operation, for a structure that never changes once
the fill is done**. Two of them per decision is 8 192 B of the 13 310 B a
`check` reads at full scale — **62 %**.

The directory size is set by buckets *per map*. The capacity constraint (K2:
one bucket must hold its share of the population within the payload ceiling) is
set by the *total* number of cells, `n_maps × n_buckets`. **Those are different
numbers**, and the application has only ever varied the first factor:
`DESIGN.md` §6.1 sweeps 8/12/16/24/32 maps with buckets pinned at 511
throughout, and concludes that "every extra map adds directory-rewrite traffic
(K5), so more is not freely better".

Holding the *product* constant instead inverts that conclusion. 32 maps × 256
buckets has the same 8 192 cells as 16 × 511, so the same bucket occupancy and
the same capacity margin — and half the directory:

| people × buckets | credential maps | cells | `check` bytes | `check` | `byid` | `miss` |
|---|---|--:|--:|--:|--:|--:|
| **16 × 511** | 1 × 511 | 8 687 | 13 310 | **25.69 ms** | **12.49 ms** | **13.15 ms** |
| 32 × 256 | 2 × 256 | 8 704 | 9 874 | 21.25 ms | 10.40 ms | 10.83 ms |
| 32 × 256 | 4 × 128 | 8 704 | 8 132 | 22.39 ms | 11.46 ms | 10.99 ms |
| 64 × 128 | 8 × 64 | 8 704 | 6 793 | 22.30 ms | 11.30 ms | 11.12 ms |

**−13 % on a decision, for free** — no library change, no extra RAM, the same
capacity margin. The transaction count is essentially unmoved (264 → 264–275),
exactly as §3.2 predicts: the cell count did not change, so the blob count did
not change. The entire win is bytes.

Take −13 %, not the −17 % the `2 × 256` row shows. That row's transaction count
(229.5) is the outlier in a column where every other configuration with the
same cell count reads 264–275, so its extra 4 ms is compaction placement rather
than an effect of the split — §2.2. Quoting the best row of a noisy column is
how a sweep flatters itself.

The returns flatten below ~256 buckets per map because the directory stops
being the dominant byte, so 32 × 256 is the sensible stopping point rather than
the extreme.

`K5` is not made worse by this, despite §6.1's expectation. A directory is
rewritten once per *fresh bucket*, and the number of fresh buckets is the cell
count — unchanged. Each rewrite simply moves a quarter as many bytes. Fill
flash traffic falls accordingly (772 MB → 513 MB at 32 × 256 / 4 × 128).

---

## 5. Lever 2 — raise the bucket payload ceiling, and spend it on fewer buckets

Lever 1 leaves the transaction term untouched, and at full scale that term is
two thirds of a decision. Cutting it means cutting the number of blobs, which
means fewer cells, which the 4 KB payload ceiling forbids: at 8 176 person
cells the fullest bucket is already 2 711 B (`RESULTS.md` §9), and halving the
cell count would overflow it.

`CONFIG_BLOB_DB_MAX_PAYLOAD_LEN` is an application `prj.conf` setting. Raising
it buys the headroom to halve the cell count:

| payload | people × buckets | cred | cells | `check` ops | `check` | `byid` | `miss` | `put` bytes written | `put` erases |
|--:|---|---|--:|--:|--:|--:|--:|--:|--:|
| 4 KB | 16 × 511 | 1 × 511 | 8 687 | 264.2 | **25.69 ms** | 12.49 ms | 13.15 ms | 12 416 | 0.60 |
| 8 KB | 16 × 256 | 4 × 128 | 4 608 | 211.0 | **18.84 ms** | 9.47 ms | 9.46 ms | 14 044 | 0.70 |
| 8 KB | 32 × 128 | 4 × 128 | 4 608 | 202.9 | **17.60 ms** | 9.19 ms | 8.28 ms | 16 531 | 0.88 |
| 16 KB | 16 × 128 | 4 × 64 | 2 304 | 123.9 | **12.75 ms** | 6.50 ms | 6.25 ms | 26 465 | 1.32 |
| 16 KB | 32 × 64 | 8 × 32 | 2 304 | 123.9 | **12.35 ms** | 6.15 ms | 6.14 ms | 25 536 | 1.28 |

**At 16 KB every read case is roughly halved.** The mechanism is exactly §3.2:
2 304 cells instead of 8 687 means 25.8 slots per sector instead of 64.4.

### 5.1 What it costs

**RAM**, which `RESULTS.md` §4a already accounts for precisely:

| | today | 8 KB | 16 KB |
|---|--:|--:|--:|
| `kvhash` `dir_buf` + `bkt_buf` | 8 192 B | 16 384 B | 32 768 B |
| `CONFIG_MAIN_STACK_SIZE` (driven by `append_slot`'s `MAX_PAYLOAD + 46` frame, B5 job 3) | 12 288 B | ~16 KB | ~28 KB |
| **image total** (from 157 584 B) | 34.4 % of 448 KB | ~38 % | ~44 % |

Both are the same symbol doing two jobs that `FINDINGS.md` **B5** already
names. Stage 1 of the large-payload proposal — staging the slot in `.bss`
instead of on the stack — would remove the stack half of this cost entirely,
which makes the sequencing question in §9 worth asking.

**Write amplification.** Every insert rewrites its whole `kvhash` bucket (K4),
so doubling the bucket size doubles the bytes written per enrollment: 12.4 KB →
26.5 KB per `put`, and erases per `put` from 0.60 to 1.32. For a product whose
reads happen at every door and whose writes happen at enrollment, that is the
right side of the trade — but it is a real cost, and it lands on the phase that
is already the slowest thing the app does.

The 8 KB tier is the better-balanced choice: **−24 to −28 % on every read case
for +13 % write bytes and +12 KB of RAM**, and it also *improves* fill (772 MB →
493 MB of flash traffic, modelled 6 286 s → 4 228 s) because the directory
shrinks faster than the buckets grow. The 16 KB tier buys another 30 % of read
performance and gives the fill improvement back.

### 5.2 The capacity check is the fill itself

`DESIGN.md` §6.1's rule is that a `kvhash` map must be sized by enumeration,
because there is no per-bucket occupancy query (K10) and no growth path (K3).
Every configuration in the table above **completed a full 10 000-person fill
with zero `-ENOSPC`**, through the real `kvhash`. That is a stronger check than
`tools/sizing.py`'s model, since it is the shipping code doing the bucketing —
and it is the check that caught §6.

---

## 6. Lever 3 — shard the credential index (and the trap in doing so)

The credential map holds 24 932 entries in one 511-bucket map. It is where
`miss` lives, and `miss` is the case with the worst amplification in the file
(300×). Sharding it across several maps shrinks the directory the same way
lever 1 does for persons, and the tables above include it.

**The obvious way to shard it is wrong, and it fails loudly but confusingly.**
`persondb.c` shards persons with `fnv1a32(&id) % n_people_maps` while `kvhash`
buckets with `fnv1a(key) % n_buckets` — two different inputs (the raw id, and
the formatted `"pXXXXXXXX"` string), so the two hashes are independent. A card
has no such second identifier: the shard and the bucket would both hash the
same string. Whenever `n_buckets` is a multiple of `n_maps` — which it is for
every power-of-two split — the shard becomes a function of the bucket index,
only `n_buckets` of the `n_maps × n_buckets` cells are ever reachable, and the
map runs at `n_maps`× the intended load.

Measured: `-ENOSPC` at person 7 843 of 10 000, with a capacity plan that said
there was 4× headroom. The fix is a second, independent mixing step
(`mix(fnv1a(card), salt) % n_cred_maps`); the harness carries it and the
comment explaining it.

Worth recording in `FINDINGS.md`: *a container that hashes the caller's key
internally makes the caller's own sharding hash unsafe, and says nothing about
it.* The application cannot see `kvhash`'s hash function, cannot ask what it
is, and gets no warning — the only symptom is a capacity failure partway
through a provisioning run, which is the same symptom K2 produces for an
entirely different reason.

---

## 7. Lever 4 — let the fill skip the diff-get

`persondb_person_put()` reads the stored record before writing, so a replace
can unindex the cards it drops (`README.md` practice 5). The comment is
explicit that inserts pay it too, "where it only ever answers `-ENOENT`", and
`RESULTS.md` §4 prices it at 10 % of the write path.

`scenario_fill()` knows it is inserting — it drives from the `populated`
counter, and every index below it has already been written. An insert-only
entry point would let those 10 000 gets be skipped without weakening anything:
the diff is only meaningful when there is an old record to diff against.

Measured on the shipped configuration:

| | with the diff-get | insert fast path | Δ |
|---|--:|--:|--:|
| fill flash operations | 7 749 671 | 6 964 351 | **−10.1 %** |
| fill flash bytes | 772.2 MB | 719.7 MB | −6.8 % |
| modelled fill time | 6 286 s | 6 202 s | −1.3 % |

**A tenth of the fill's flash work, for about a fiftieth of its time** — which
is `RESULTS.md` §5's finding restated: the fill is erase-bound, so removing
read work barely moves it. Worth doing for a fill that is already ~2.2 h, not
worth doing *first*.

The API cost is the part to think about: `persondb_person_put()` currently
means "insert or replace, correctly", and that is the sentence practice 5 is
built on. A second entry point means a caller can now get it wrong. If it is
added, it should be named for the precondition it demands
(`persondb_person_insert_new()`), document that a wrong call resurrects exactly
the withdrawn-credential defect practice 5 exists to prevent, and be reachable
only from the fill.

---

## 8. Measured non-levers, and one bug

### 8.1 `CONFIG_APP_CBOR_PERSONDB_PEOPLE_MAPS` can write out of bounds

Not a performance finding, but it blocks levers 1 and 2 and it is live today:

```c
/* Kconfig */          range 1 64
/* person_cbor.h */    uint64_t people_root[16];
/* persondb.c */       for (uint8_t i = 0; i < sb->n_people_maps; i++)
                               sb->people_root[i] = blob_db_alloc_id();
```

`create_store()` writes `people_root[i]` for every `i` below the Kconfig value,
straight past the end of a 16-element array in the file-scope `g_db`, clobbering
`cred_root`, `n_persons`, `populated`, `rev` and then `struct persondb`'s own
fields. The guard that would catch it —
`sb->n_people_maps > ARRAY_SIZE(sb->people_root)` in
`superblock_cbor_encode()` — runs at `sb_commit()`, after the damage.

Anyone taking `DESIGN.md` §6.1's table at face value and trying 24 or 32 maps
hits this. It should be fixed regardless of whether anything else here is
adopted: either lower the Kconfig range to `1 16`, or raise the array — and
`SUPERBLOCK_CBOR_MAX` (256 B) with it, since 32 roots encode to roughly 330 B
and the commit would fail `-ENOMEM`.

### 8.2 CBOR is still not the problem, and still worth restating

The codec is 950 µs, 6.5 % of today's decision (`RESULTS.md` §5a). None of the
levers here touch it, so its *share* rises as flash gets cheaper — 7.5 % at the
16 KB configuration's 12.7 ms. It becomes the second-largest term rather than
the third, and a targeted decode for the decision path (the record is canonical
and its key order fixed, so `check` could skip name, dept, title, PIN and the
card list) could take maybe a third of it. That is ~2 % of a decision. It is
the right answer to the wrong question, which is exactly what `README.md`
practice 6 says about this measurement — and this document is another instance
of it.

### 8.3 Denormalization is still refused, and did not need to be reconsidered

The obvious "make `check` one lookup instead of two" is a permission bitmask in
the credential index, and `README.md` practice 6 prices refusing it at 53 % of
a decision. Nothing here changes that argument, and — worth stating — nothing
here *needs* it: halving the cost of both lookups gets most of what one lookup
would have bought, while keeping one copy of the truth. The cheaper the
lookups get, the weaker the case for denormalizing becomes.

### 8.4 A post-provisioning compaction pass would not pay

Measured in §3.2: 23–28 % of the slots a lookup walks are superseded. Even a
free, perfect compaction of the whole store would cut a decision by less than a
quarter — and the stack has no API to ask for one (`blob_db_prepare()` formats
empty buckets; `blob_db_format()` wipes). Worth knowing that the idea was
measured and dropped rather than never considered.

---

## 9. Recommendation and sequencing

1. **Fix §8.1 now.** It is independent of everything else and it is a memory
   corruption reachable from a documented Kconfig range.
2. **Do not adopt anything else before the `A4` full-scale DK run.** Every
   number here is modelled; `RESULTS.md` §5a is a standing account of what
   happens when a plausible model meets a board. The run also tests §3.3's
   26.6 ms prediction, which is the cheapest available check on this whole
   document.
3. **Then take lever 1** (32 × 256 person maps, 2 × 256 credential maps, with
   §6's independent shard hash). It costs no RAM, needs no library change, and
   is worth ~15 % on every read case. It does require a reformat and the
   superblock changes in §8.1.
4. **Then decide on lever 2 with the 8 KB tier as the default** — another
   ~25 % on reads and a faster fill for +12 KB of RAM. Consider whether
   `FINDINGS.md` B5 job 3 (staging the slot in `.bss`) should land first, since
   it removes the stack half of the RAM cost and would make the 16 KB tier
   affordable too.
5. **Lever 4 whenever the fill is being touched anyway.** A tenth of its flash
   work, and it is the change with the largest API-safety cost per millisecond
   saved.

Each of steps 3 and 4 bakes a bucket count into the store at create time (K3),
so each is a reformat: ~2.2 h of refill, and `RESULTS.md` has to be re-taken
rather than edited. That, not the code, is the expensive part.

---

## 10. What this investigation did not do

- **Nothing was built for a target, and nothing was run on hardware.** No
  Zephyr tree was available, which is why the harness exists at all.
- **`put` and `fill` durations are the weakest numbers here.** The write
  constant in the model is not fitted against a board. Read them as op, byte
  and erase counts.
- **One run per configuration.** §2.2 quantifies the variance that implies.
- **`persondb_card_revoke`, `persondb_person_delete`, the permission
  operations, and a steady-state `persondb_open` are still unmeasured** — the
  same gap `RESULTS.md` §5b lists. The harness could cover them; this
  investigation stayed on the five benchmark cases it was asked about.
- **`CONFIG_BLOB_DB_LARGE_PAYLOADS` was left off**, matching the app's
  configuration. Whether segmented payloads change the arithmetic in §5 is a
  separate question.
