# Improving the cost of `app_cbor_persondb`'s benchmark cases

**Status: investigation, now measured.** The levers below were found with a
host harness and then **measured on `native_sim` with the real application**,
which changed one of the conclusions — see §4. Flash operation and byte counts
are therefore real; wall-clock for the DK is still modelled from `app_perf`'s
fitted constants, and **nothing here has been run on hardware**.

The app carries the three Kconfig options the measurements need
(`MAP_BUCKETS`, `CRED_MAPS`, and a `PEOPLE_MAPS` that no longer overruns its
array). **Their defaults reproduce the shipped behaviour exactly** — this
document proposes changing them, and does not change them.

- Harness, for the mechanism and for re-deriving the model:
  [`doc/proposals/persondb-perf/`](persondb-perf/README.md).
- Reproducing the measured tables: §11.

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

Measured on `native_sim` with the real application at the headline
10 000-person scale — real flash counters, DK times modelled from them with
`app_perf`'s fitted constants, CBOR excluded (add 0.95 ms as `RESULTS.md` §5
does). Every configuration passed both verification rounds with zero bucket
overflows:

| configuration | `check` | `byid` | `miss` | `put` ops | cost |
|---|--:|--:|--:|--:|---|
| **shipped** — 4 KB payload, 16 × 511, 1 credential map | **25.4 ms** | **12.4 ms** | **12.9 ms** | 806 | — |
| choose the directory size: 32 × 256, 2 credential maps | 24.0 ms | 11.5 ms | 12.3 ms | 801 | none — a reformat |
| raise the payload ceiling to 8 KB: 16 × 256, 2 credential maps | 19.0 ms | 9.5 ms | 9.4 ms | 616 | +12 KB RAM |
| …to 16 KB: 16 × 128, 4 credential maps | **13.4 ms** | **7.1 ms** | **6.3 ms** | **426** | +40 KB RAM |

**The 16 KB row is better than the shipped configuration on every axis
measured** — every read case roughly halved, 47 % fewer flash operations per
`put`, its bytes within 2 %, and the fill 57 % faster. It costs RAM and a
reformat, nothing else.

**Halving every read case is available, and the price is RAM and write
amplification, not correctness.** Nothing here denormalizes anything, so
`DESIGN.md` D2 and `README.md` practice 6 are untouched: `check` is still
`card_owner` + `person_get`, and the person record is still the single copy of
the truth.

Three things fell out along the way that are worth acting on independently of
any of the above:

- **A latent out-of-bounds write**, now fixed because the measurements needed
  it: `CONFIG_APP_CBOR_PERSONDB_PEOPLE_MAPS` accepted `1..64` while
  `struct superblock` held `people_root[16]`. §8.1.
- **A hash trap waiting for anyone who sharded the credential index** the
  obvious way. It cost this investigation an `-ENOSPC` before it was
  understood. §6.
- **A prediction for the outstanding `A4` run.** The harness says the
  full-scale DK `check` should land near **26.4 ms**, not the ~24 ms
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

### 2.1 It reproduces the published runs, and the application itself

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
| **the real app, rebuilt and rerun here** (`native_sim`, 10 000) | | | |
| `check` flash ops/op | 260.9 | 264.2 | +1.3 % |
| `check` flash bytes/op | 13 270 | 13 310 | +0.3 % |
| `byid` flash ops/op | 127.6 | 128.5 | +0.7 % |
| `miss` flash ops/op | 132.4 | 135.2 | +2.1 % |
| `put` flash ops/op | 805.8 | 809.5 | +0.5 % |

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
- **It found the levers; it did not size all of them correctly.** §4 is the
  case in point: the harness modelled lever 1 at −13 % and the real
  application measured −5.9 %, because the harness's bookkeeping held the cell
  count fixed while the map count rose, and every extra map is another root
  blob. It was also wrong in the *pessimistic* direction on `put` bytes at
  16 KB (§5). Where §4 and §5 disagree with a modelled figure, the measured
  one is the one quoted.
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
give 25.45 ms of flash plus 0.95 ms of codec: **≈ 26.4 ms**. If the A4 run
lands materially below that, the cost model is wrong somewhere and the rest of
this document should be re-read with that in mind.

---

## 4. Lever 1 — choose the directory size instead of maximising it

`kvhash` reads the whole directory blob on every `get`, `set` and `del`. The
directory is `8 + 8 × n_buckets` bytes: at the maximum 511 buckets that is
**4 096 B, re-read on every operation, for a structure that never changes once
the fill is done**. Two of them per decision is 8 192 B of the 13 270 B a
`check` reads at full scale — **62 %**.

The directory size is set by buckets *per map*. The capacity constraint (K2:
one bucket must hold its share of the population within the payload ceiling) is
set by the *total* number of cells, `n_maps × n_buckets`. **Those are different
numbers**, and the application has only ever varied the first factor:
`DESIGN.md` §6.1 sweeps 8/12/16/24/32 maps with buckets pinned at 511
throughout, and concludes that "every extra map adds directory-rewrite traffic
(K5), so more is not freely better".

Holding the *product* constant instead — 32 maps × 256 buckets is the same
8 192 cells as 16 × 511, so the same bucket occupancy and the same capacity
margin — does shrink the directory as expected. Measured:

| people × buckets | credential maps | `check` ops | `check` bytes | `check` | `byid` | `miss` |
|---|--:|--:|--:|--:|--:|--:|
| **16 × 511** (shipped) | 1 | **260.9** | **13 270** | **25.45 ms** | **12.43 ms** | **12.94 ms** |
| 32 × 256 | 2 | 276.3 | 9 304 | 23.96 ms | 11.49 ms | 12.34 ms |
| 32 × 256 | 4 | 308.4 | 9 138 | 25.96 ms | 13.17 ms | 12.84 ms |

**This is where the real application corrected the harness.** The bytes fall
exactly as modelled — 13 270 → 9 304, −30 % — but the transaction count *rises*
(260.9 → 276.3), and transactions are the larger term. The net is **−5.9 % on a
decision, not the −13 % the harness predicted.**

The mechanism is the one §3.2 already gives, applied to a population the
harness's cell-count bookkeeping treated as fixed: **every extra map is another
root blob**, and every root blob is another slot header on the walk. Going from
17 maps to 34 adds 17 blobs and their directory-rewrite garbage. Push it
further and the effect swallows the win outright: the third row above, with
four credential shards at 256 buckets each, has 9 216 cells against the
baseline's 8 687 and comes out **2 % slower than doing nothing**.

So lever 1 is real but small, and it is only safe when the cell count is held
constant — which means the credential shard count and the bucket count must be
chosen together, not independently. It remains worth taking because it is free,
and because §5 needs the same option.

`K5` is not made worse, despite §6.1's expectation: a directory is rewritten
once per *fresh bucket*, the number of fresh buckets is the cell count, and
each rewrite moves a quarter as many bytes. The fill is **25 % faster**
(6 394 ms → 4 790 ms on `native_sim`).

---

## 5. Lever 2 — raise the bucket payload ceiling, and spend it on fewer buckets

Lever 1 cannot touch the transaction term; §4 shows it slightly worsening it.
Cutting transactions means cutting the number of blobs, which means fewer
cells, which the 4 KB payload ceiling forbids: at 8 176 person cells the
fullest bucket is already 2 711 B (`RESULTS.md` §9), and halving the cell count
would overflow it.

`CONFIG_BLOB_DB_MAX_PAYLOAD_LEN` is an application `prj.conf` setting. Raising
it buys the headroom to halve the cell count. Measured, real application,
10 000 persons, all rows `VERIFY PASS` with zero bucket overflows:

| payload | people × buckets | cred | cells | `check` ops | `check` | `byid` | `miss` | `put` ops | `put` bytes | fill |
|--:|---|--:|--:|--:|--:|--:|--:|--:|--:|--:|
| 4 KB | 16 × 511 | 1 | 8 687 | 260.9 | **25.45 ms** | 12.43 ms | 12.94 ms | 806 | 93 395 | 6 394 ms |
| 4 KB | 32 × 256 | 2 | 8 704 | 276.3 | 23.96 ms | 11.49 ms | 12.34 ms | 801 | 79 552 | 4 790 ms |
| 8 KB | 16 × 256 | 4 | 5 120 | 241.6 | 21.37 ms | 10.84 ms | 10.50 ms | 733 | 61 238 | 3 138 ms |
| **8 KB** | **16 × 256** | **2** | **4 608** | **204.1** | **18.97 ms** | **9.51 ms** | **9.43 ms** | **616** | **78 685** | **3 719 ms** |
| **16 KB** | **16 × 128** | **4** | **2 560** | **138.3** | **13.41 ms** | **7.05 ms** | **6.34 ms** | **426** | **95 494** | **2 758 ms** |
| 16 KB | 16 × 128 | 2 | 2 304 | 119.1 | 12.71 ms | 6.44 ms | 6.32 ms | 362 | 141 556 | 3 828 ms |
| 16 KB | 16 × 128 | 1 | 2 176 | 113.2 | 13.70 ms | 5.97 ms | 7.73 ms | 338 | 242 803 | 6 210 ms |

Against the shipped configuration:

| | 8 KB, 16 × 256, 2 cred | 16 KB, 16 × 128, 4 cred |
|---|--:|--:|
| `check` | **−25 %** | **−47 %** |
| `byid` | −24 % | −43 % |
| `miss` | −27 % | −51 % |
| `put` flash operations | −24 % | −47 % |
| `put` flash bytes | −16 % | +2 % |
| fill | −42 % | −57 % |

**The 16 KB row is not a trade at all** — it is better than the shipped
configuration on every column measured. That is the one result here that the
harness got wrong in the *pessimistic* direction: it predicted `put` bytes
roughly doubling, because it modelled the bucket rewrite and not the erase
traffic that dominates the real total. Halving the number of buckets halves the
directory-rewrite traffic too, and the two effects cancel.

The last two rows show why the credential shard count has to be chosen with
the bucket count rather than maximised: at 16 KB, dropping from 4 shards to 2
buys 0.7 ms of `check` and costs 48 % more `put` bytes; dropping to 1 costs
160 %.

### 5.1 What it costs

**RAM**, on the arithmetic `RESULTS.md` §4a already sets out:

| | today | 8 KB | 16 KB |
|---|--:|--:|--:|
| `kvhash` `dir_buf` + `bkt_buf` | 8 192 B | 16 384 B | 32 768 B |
| `CONFIG_MAIN_STACK_SIZE` (driven by `append_slot`'s `MAX_PAYLOAD + 46` frame, B5 job 3) | 12 288 B | 16 384 B | 28 672 B |
| **image total** (from 157 584 B) | 34.4 % of 448 KB | ~38 % | ~44 % |

The stack figures are what the measured builds used, not estimates; the image
totals are arithmetic on `RESULTS.md` §4a and have not been re-measured for the
DK. Both costs are the same symbol doing two jobs that `FINDINGS.md` **B5**
already names, and Stage 1 of the large-payload proposal — staging the slot in
`.bss` instead of on the stack — would remove the stack half of it.

### 5.2 The capacity check is the fill itself

`DESIGN.md` §6.1's rule is that a `kvhash` map must be sized by enumeration,
because there is no per-bucket occupancy query (K10) and no growth path (K3).
Every configuration above **completed a full 10 000-person fill and both
verification passes with zero `-ENOSPC`**, in the real application. That is a
stronger check than `tools/sizing.py`'s model, and it is the check that caught
§6.

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
there was 4× headroom. The fix is a second, independent avalanche step before
the modulo; `persondb.c`'s `cred_root()` carries it, with the comment
explaining why, and so does the harness.

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

Not a performance finding. It blocked levers 1 and 2, so it is fixed here —
but it was live in every build before this branch:

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
hit this. The fix here raises the array to 64 — matching the range that was
already documented — and `SUPERBLOCK_CBOR_MAX` from 256 B to 896 B with it,
since 32 roots alone encode to roughly 330 B and the commit would otherwise
fail `-ENOMEM`. Lowering the Kconfig range to `1 16` would have been the other
valid answer; it was not taken because §5 needs the range.

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

1. **§8.1 is already fixed** — `people_root[]` now matches the Kconfig range,
   and `SUPERBLOCK_CBOR_MAX` grew with it. It had to be, to measure anything
   above 16 maps.
2. **Do not change a default before the `A4` full-scale DK run.** Every
   duration here is modelled; `RESULTS.md` §5a is a standing account of what
   happens when a plausible model meets a board. That run also tests §3.3's
   26.6 ms prediction, which is the cheapest available check on this document.
3. **Then take the 16 KB configuration** — `MAP_BUCKETS=128`, `CRED_MAPS=4`,
   `CONFIG_BLOB_DB_MAX_PAYLOAD_LEN=16384`, `CONFIG_MAIN_STACK_SIZE=28672`. It
   is better than the shipped configuration on every measured column, and the
   only question is whether the image can afford ~44 % of the DK's RAM
   (`RESULTS.md` §4a puts it at 34.4 % today). If it cannot, the 8 KB row is
   −25 % for +12 KB.
4. **Consider landing `FINDINGS.md` B5 job 3 first.** Staging blob_db's slot
   in `.bss` removes the stack half of the RAM cost, which is 16 KB of the
   40 KB at the 16 KB tier and would make the choice in step 3 easy.
5. **Lever 1 on its own is not worth a reformat.** At −5.9 % it is inside the
   noise a board run would have to resolve. Take it as part of step 3, where
   choosing the bucket count is required anyway.
6. **Lever 4 whenever the fill is being touched.** A tenth of its flash work,
   ~2 % of its time, and the largest API-safety cost per millisecond saved.

Steps 3 and 5 bake a bucket count into the store at create time (K3), so each
is a reformat: ~2.2 h of refill on the DK, and `RESULTS.md` has to be re-taken
rather than edited. That, not the code, is the expensive part.

---

## 10. What this investigation did not do

- **Nothing was run on hardware.** The flash operation and byte counts are
  real, from `native_sim`; every duration is those counts through `app_perf`'s
  fitted constants.
- **`put` and `fill` durations remain the weakest numbers.** The model has no
  hardware-fitted write constant, so §5 quotes `put` as operations and bytes.
  `RESULTS.md` §3b's ±20 % on the write path applies to the fill column too.
- **One run per configuration**, and `native_sim`'s host µs/op column is not
  comparable to anything (`RESULTS.md` §1).
- **The DK RAM figures in §5.1 are arithmetic, not a target build.** No ARM
  toolchain was available here; `west build -b nrf5340dk/nrf5340/cpuapp -t
  ram_report` would settle them.
- **`persondb_card_revoke`, `persondb_person_delete`, the permission
  operations, and a steady-state `persondb_open` are still unmeasured** — the
  same gap `RESULTS.md` §5b lists. This investigation stayed on the five
  benchmark cases it was asked about.
- **`CONFIG_BLOB_DB_LARGE_PAYLOADS` was left off**, matching the app's
  configuration. Whether segmented payloads change §5's arithmetic is a
  separate question.

---

## 11. Reproducing the measured tables

The defaults are the shipped configuration, so a plain run is the baseline row:

```
west build -p always -b native_sim app_cbor_persondb
./build/zephyr/zephyr.exe --flash=db.bin --flash_erase
```

Each other row is the same command with its overrides, on a fresh store:

```
west build -p always -b native_sim app_cbor_persondb --                 \
      -DCONFIG_APP_CBOR_PERSONDB_MAP_BUCKETS=128                        \
      -DCONFIG_APP_CBOR_PERSONDB_CRED_MAPS=4                            \
      -DCONFIG_BLOB_DB_MAX_PAYLOAD_LEN=16384                            \
      -DCONFIG_MAIN_STACK_SIZE=28672
./build/zephyr/zephyr.exe --flash=db16k.bin --flash_erase
```

`--flash_erase` matters: a bucket count is fixed at create time, so a build
that disagrees with the store refuses to mount and says so.

The modelled DK column is `flash ops × 65.5 µs + flash bytes × 0.63 µs`, from
the benchmark's own printed counters. `RESULTS.md` §5 is where those constants
come from and where the same arithmetic is checked against a board.

The harness in [`persondb-perf/`](persondb-perf/README.md) is not needed to
reproduce any of the above. It is what found the mechanism in §3.2 — the
per-sector slot census, which the application cannot report — and it is the
only way to explore a configuration without a full build and fill.
