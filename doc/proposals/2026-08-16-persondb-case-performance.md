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
| raise the payload ceiling to 8 KB: 16 × 256, 2 credential maps | 19.0 ms | 9.5 ms | 9.4 ms | 616 | +8 KB RAM |
| …to 16 KB: 16 × 128, 4 credential maps | **13.4 ms** | **7.1 ms** | **6.3 ms** | **426** | +24 KB RAM |

**The 16 KB row is better than the shipped configuration on every axis
measured** — every read case roughly halved, 47 % fewer flash operations per
`put`, its bytes within 2 %, and the fill 57 % faster. It costs RAM and a
reformat, nothing else.

**Read §3.2a before acting on any of it.** The read cases are dominated by a
~65 µs per-transaction cost that is ~25× the QSPI bus time for a read command,
so it is driver overhead rather than flash. Profiling that path could beat this
whole table without a reformat. What the flash genuinely imposes is the
**erase**, and only `put` and `fill` pay it.

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

### 3.2a How much of that 65 µs is the flash? Almost none

The two fitted constants are solid — `app_perf/RESULTS.md` derives them from
the `lg read` pair and then predicts, independently, the small-blob `read`
phase to 4 %, an `update` regression to 1 %, and this app's `check` to a
fraction of a percent. They describe **this system**. They do not describe NOR.

The DK's `mx25r6435f` node is `readoc = "read4io"` at `sck-frequency =
<8000000>` — quad I/O at 8 MHz, so **4 MB/s = 0.25 µs/B** of bus, and a read
command (1 cmd + 3 address + dummy) is on the order of **2.5 µs**:

| | measured | bus ceiling | what the gap is |
|---|--:|--:|---|
| per transaction | **65.5 µs** | ~2.5 µs | ~25× — nrfx QSPI transfer setup, peripheral start latency, the driver's per-transfer completion wait |
| per byte, small reads | 0.63 µs | 0.25 µs | 2.5× — per-transfer overhead spread over few bytes |
| per byte, 64 KB bulk | 0.35 µs | 0.25 µs | 1.4× — close to the bus |
| **64 KB erase** | **~1 072 ms** | — | **the flash itself.** MX25R6435F block erase is ~0.5–0.7 s typical. Irreducible |

So the cost model splits into two very different halves, and the benchmark
cases fall cleanly on either side:

- **`check` / `byid` / `miss` issue no erases at all.** Their cost is
  transactions (67 %) plus bytes (33 %), and the transaction term is *software
  overhead*, not flash. It is the largest single number in this document and it
  is the one least anchored in physics.
- **`put` and `fill` are erase-bound**, and that erase cost is real device
  physics. 0.493 erases per person added, ~1.07 s each — ≈88 min of the ≈2.2 h
  fill, and the reason `RESULTS.md` §5b records the same `put` call costing
  84 ms or 808 ms.

**This demotes §5.** Restructuring the store attacks the transaction *count*,
which is only worth attacking while a transaction costs 65 µs. If that is
driver overhead and it can be cut to, say, 5 µs, `check` goes to
264 × 5 µs + 13 270 × 0.63 = **9.7 ms** — better than the 16 KB reformat's
13.4 ms, with no format change, no extra RAM and no reformat. Hitting the bus
ceiling on bytes as well would give ~4.6 ms.

Nobody has profiled that path, so this is arithmetic, not a measurement. But
the ordering it implies is worth stating plainly: **profile the QSPI driver
before reformatting the store.** The erase-side lever (§B2 and the atomic-LEB
sketch in §12) is the one that attacks a cost the flash genuinely imposes.

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

### 5.1 What it costs — measured, and less than first claimed

An earlier revision of this document put the 16 KB tier at **+40 KB of RAM**,
by arithmetic on `RESULTS.md` §4a. That was wrong, and the way it was wrong is
worth keeping.

Measured instead — `nm --size-sort` on the `native_sim` image at each ceiling,
which is exact for these objects because they are fixed-size arrays whose size
does not depend on the architecture:

| symbol | 4 KB | 8 KB | 16 KB |
|---|--:|--:|--:|
| `kvhash` `dir_buf` | 4 096 B | 8 192 B | 16 384 B |
| `kvhash` `bkt_buf` | 4 096 B | 8 192 B | 16 384 B |
| `blob_db` `g_bbuf` + `g_bbuf_new` | 131 072 B | 131 072 B | 131 072 B |
| `z_main_stack` | 12 288 B | 12 288 B | 12 288 B |
| **added by the ceiling** | — | **+8 KB** | **+24 KB** |

**Two of the three costs I assumed do not exist.**

- **The stack does not move.** `-fstack-usage` gives byte-identical frames at
  `MAX_PAYLOAD` 4096 and 16384, because `main` stages the slot in `blob_db`'s
  existing `.bss` scratch (`append_slot2`) rather than on the stack. That is
  `FINDINGS.md` **B5 job 3**, which the register already records as fixed —
  but `prj.conf`'s comment and `RESULTS.md` §4a still describe the 4 200 B
  frame it removed, and this document believed them. The measurement builds in
  §11 passed `CONFIG_MAIN_STACK_SIZE` overrides that were never needed; they do
  not affect a single flash counter, so the results stand and only this column
  changes.
- **`blob_db`'s `g_chunk[MAX_PAYLOAD]` is not compiled in**, because it lives
  under `CONFIG_BLOB_DB_LARGE_PAYLOADS` and this app does not enable it. It
  would add the same again if that ever changed.

#### The baseline itself moved, and CI measured it

An earlier revision added those deltas to `RESULTS.md` §4a's 157 584 B and
called the totals arithmetic. **PR #19's `nrf5340dk cross-build` job settles
them** — a real `arm-zephyr-eabi` link of this branch:

| build | FLASH | RAM | `RESULTS.md` §4a | Δ RAM |
|---|--:|--:|--:|--:|
| `nrf_persondb` (bench) | 61 864 B | **158 200 B** | 157 584 B | **+616 B** |
| `nrf_persondb_shell` | 96 316 B | **162 944 B** | 162 328 B | **+616 B** |

The +616 B is this branch's own doing and §5 never accounted for it: the
superblock widened (`people_root[16]`→`[64]` = +384 B, `cred_root` scalar →
`[16]` = +120 B, plus `n_cred_maps`, `n_buckets` and alignment), and it lands
in the file-scope `g_db`. Both frontends move by exactly the same amount, which
is the check that it is `g_db` and not something frontend-specific. The ~2.8 KB
of FLASH is the new `cred_root()`/`mix32()`, the wider CBOR loops and the log
strings.

So the honest cost, from the measured ARM baseline:

| | image RAM | of 448 KB |
|---|--:|--:|
| today, on this branch (**measured**) | 158 200 B | 34.5 % |
| 8 KB ceiling | 166 392 B | **36.3 %** |
| 16 KB ceiling | 182 776 B | **39.8 %** |

Two of those percentages were previously quoted as 37.0 % and 40.7 %. Both were
arithmetic slips — 448 KB is 458 752 B, and the correct figures are ~0.9 pp
lower. The check that should have caught it was in the same table: §4a's
157 584 B is 34.35 %, which the document already printed as 34.4 %.

And there is change to spare. The write chain now measures **3 008 B**
(`scenario_bench` 1696 + `persondb_person_put` 784 + `kvhash_set` 128 +
`blob_db_update` 112 + `compact_bucket` 192 + `append_slot` 96) against a
12 288 B reservation sized for a 7 640 B chain at ~1.5× margin. At the same
margin the stack would be ~4.5 KB, returning **~7 KB** — which makes the 8 KB
ceiling **RAM-neutral** and the 16 KB ceiling cost ~17 KB net.

Those are host frames, so this is a claim about the *shape* of the requirement,
not a number to set `CONFIG_MAIN_STACK_SIZE` from; `RESULTS.md` §4a's ARM
figures are what that value was chosen against and they need re-taking. The
comment in `prj.conf` is corrected to say so rather than keep describing a
frame that no longer exists.

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
   is better than the shipped configuration on every measured column, and it
   costs **+24 KB of RAM** — 40.7 % of the DK's 448 KB against 34.4 % today
   (§5.1, measured). If that is too much, the 8 KB row is −25 % for +8 KB.
4. **Re-take `RESULTS.md` §4a's stack measurement on the board.** It, and
   `prj.conf`'s comment, still describe an `append_slot` frame that `main`
   removed; the chain is now ~3 KB against a 12 288 B reservation. Confirming
   that on ARM would return ~7 KB, which pays for the 8 KB ceiling outright
   and most of the way to the 16 KB one.
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
- **The RAM figures in §5.1 are now measured on ARM**, by PR #19's
  `nrf5340dk cross-build` job, which also revealed that this branch's own
  superblock widening costs +616 B that §5 had not accounted for. What is still
  *not* settled is the **stack** question in §9 step 4: the 3 008 B chain is a
  host measurement, and `CONFIG_MAIN_STACK_SIZE` was chosen from ARM frames that
  need re-taking with `-fstack-usage` on a target build.
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

---

## 12. Does any of this describe the UBI backend? No — measured

Everything above is the **`flash_area`** backend: `CONFIG_BLOB_DB_BACKEND_FLASH_AREA`
is the default and `prj.conf` selects no other. The harness's `store_host.c`
models that seam too, so it says nothing about UBI either.

**"UBI" here is not Linux UBI.** `west.yml` pulls `wmilek/ubi`
(`feature/leb-partial-update`), a fork of `kamil-kielbasa/ubi` — a Zephyr
library whose own README says it is "inspired by Linux's `drivers/mtd/ubi`,
written from scratch for Zephyr's `flash_area` API". The model and the API
names carry over, and the EC/VID magic values are literally the kernel's
(`0x55424923`, `0x55424921`), but the headers are 16 B and 32 B against the
kernel's 64 B each, and volume metadata lives in a device header on reserved
PEBs rather than in a layout volume. **The two on-flash formats are not
interchangeable, and share enough magic to be mistaken for one another.**

**The action tree is unchanged.** `blob_db`'s slot walk, its append, its
five-erase compaction commit, `kvhash`'s directory-plus-bucket structure and
persondb's write orderings all live *above* `lib/blob_db/blob_db_store.h`. Only
the four functions beneath it are swapped. So the map-op counts, the three
sector walks per `set`, the whole-bucket read-modify-write: identical.

What changes is underneath, and it is not free. One run each, same app, same
10 000 persons, `native_sim`:

| per op | `flash_area` | UBI | Δ |
|---|--:|--:|--:|
| `check` flash ops | 260.9 | 287.2 | **+10.1 %** |
| `check` flash bytes | 13 270 | 13 586 | +2.4 % |
| `byid` flash ops | 127.6 | 135.7 | +6.4 % |
| `miss` flash ops | 132.4 | 152.6 | **+15.3 %** |
| `put` flash ops | 805.8 | 857.9 | +6.5 % |
| `put` flash bytes | 93 395 | 103 930 | +11.3 % |
| buckets formatted by `prepare` | 106 | **100** | −6 |

Both runs `VERIFY PASS` with zero bucket overflows, so the app is correct on
either substrate. Three mechanisms account for the difference:

- **blob_db gets fewer, smaller erase blocks.** A PEB becomes a UBI **LEB** —
  a PEB minus UBI's two headers — and the backend holds back
  `BLOB_DB_UBI_SPARE_PEBS` (4) plus whatever UBI reserves for itself. Six
  sectors fewer here. Since `id_to_bucket()` is `id % n_buckets`, fewer buckets
  means **more blobs per sector**, and §3.2 says the walk length *is* the blob
  count. That is where the +10 % comes from — the same lever as §5, pushed the
  wrong way.
- **Every store read acquires a mapping check.** `blob_db_store_read()` calls
  `ubi_leb_is_mapped()` before each `ubi_leb_read()`, and the slot walk issues
  one store read *per slot*. The `miss` case, which walks furthest without
  finding anything, pays the most (+15 %).
- **An erase is no longer an erase.** `blob_db_store_erase()` becomes
  `ubi_leb_unmap()` plus a bounded `ubi_device_erase_peb()` reclaim. The
  physical block erase still happens, but deferred and amortised rather than
  inline — so the ~1 072 ms-per-erase term in every model above **does not
  transfer**, and UBI's own wear-levelling copies never appear in
  `blob_db_iostats_get()` at all.

**Consequently: do not apply §5's DK milliseconds to a UBI build.** Its two
fitted constants were measured on raw flash. The operation and byte counts
above are real; the time model behind every other table in this document is
not calibrated for this substrate, and calibrating it needs a board.

### 12.1 What `ubi_leb_write_at()` does and does not promise

Worth stating because blob_db's whole design rests on it: the partial write
**never erases and never relocates**. `ubi_plain_leb_write_at()` checks the
volume is dynamic, the offset is write-block aligned and the range fits, maps
the LEB if this is its first touch, and then programs straight through to
`flash_area_write` at `pnum × eb_size + EC_HDR + VID_HDR + offset`. There is no
read-modify-write and no check that the target is still erased.

So a LEB behaves as raw NOR: **a write may only clear bits, and nothing
enforces it** — set a bit back to 1 and you silently get the AND. blob_db
depends on exactly that liberty in `blob_db_erase_all()`, which invalidates a
bucket by programming zeros over its `BDBH` magic. It works identically on both
backends.

### 12.2 Where the contract diverges from the Linux one it mimics

The project's aim is to carry Linux concepts onto Zephyr, so the interesting
comparison is semantic, not structural. The **model** carries over intact: a
two-tier write — a non-atomic in-place offset write plus an atomic whole-LEB
replace — over LEBs mapped on first touch, EC headers carrying erase counts for
a global wear pool, unmap returning a PEB for reclaim, and bad-block isolation
on write failure. Five things do not carry over, and each is a contract, not a
detail.

**1. The two write calls are named the other way round.**

| Linux | semantics | here |
|---|---|---|
| `ubi_leb_write(…, buf, offset, len)` | in-place, offset-based, **not** atomic | `ubi_leb_write_at()` |
| `ubi_leb_change(…, buf, len)` | atomic whole-LEB replace: fresh PEB, then mapping swap | **`ubi_leb_write()`** |

`leb_prepare_new_mapping()` + `leb_commit_mapping_swap()` is exactly
`ubi_leb_change`'s algorithm — payload to a PEB off the free pool, VID header
as the commit point, then the EBA swap, so a failure before the swap leaves the
previous content intact. Correct, and correctly atomic. But a caller carrying
Linux intuition writes `ubi_leb_write(…, buf, len)` expecting an in-place write
at offset 0 and gets a copy-on-write replace instead.

**2. Reading an unmapped LEB is an error, not 0xFF.** In Linux, unmapped means
*erased*: the read fills 0xFF and returns 0. Here `ubi_plain_leb_read()`
returns `-ENOENT` and logs, which is why `blob_db_store_ubi.c` calls
`ubi_leb_is_mapped()` before every read.

That guard is **not** a flash cost: `ubi_plain_leb_is_mapped()` is
`ubi_find_volume()` plus an rbtree lookup in the RAM-resident EBA table, under
the device mutex. An earlier revision of this document claimed it contributed to
the +10 % in §12. It cannot — it never reaches the flash — and §12.2a is what
actually does.

### 12.2a blob_db's own counters stop being the truth under UBI

`RESULTS.md` practice 12 is "count your own flash traffic — measure, do not
model", and `blob_db_iostats_get()` is where that counting happens. It sits
*above* the store seam, so it counts one read per `blob_db_store_read()`. On
`flash_area` that is exactly one `flash_read()`. On UBI it is not.

Measured with the `native_sim` flash simulator's own
`flash_sim_stats.flash_read_calls`, which counts physical calls *below*
everything — `byid`, 200 ops, 2 000 persons:

| | blob_db counters | physical | ratio |
|---|--:|--:|--:|
| `flash_area` reads | 17 443 | **17 443** | **1.000×** |
| `flash_area` bytes | 1 120 977 | **1 120 977** | **1.000×** |
| UBI reads | 18 348 | **36 696** | **2.000×** |
| UBI bytes | 1 131 837 | **1 718 973** | 1.519× |

The 2.000× is exact and the cause is one line: `ubi_plain_leb_read()` calls
`ubi_vid_hdr_read()` on **every** read, to recover `data_size` and bound the
range, with a CRC32 over the header. It is not cached. So each logical read is
two physical transactions, and the byte gap is exactly
`18 348 × 32 B = 587 136 B` of VID headers — 1 131 837 + 587 136 = 1 718 973,
to the byte.

Two consequences:

- **§12's +10 % / +15 % understates UBI.** Those are blob_db's counters; the
  physical transaction count is double. Applying `app_perf`'s ~65.5 µs per
  transaction to the counters would price a UBI `byid` at ~6 ms when the flash
  actually sees ~12 ms of transactions. The host clock agrees in direction —
  21 µs/op on `flash_area` against 47 µs on UBI, and `native_sim` has no real
  flash to wait for.
- **The instrument became a model when the backend changed**, which is the
  exact failure practice 12 exists to prevent, one layer further down than it
  was written for. Any iostats figure in this repo taken on the UBI backend
  needs the ×2 stated alongside it.

**Why this is worth more than "2× transactions" suggests.** The VID read is a
*fixed* 32 B plus one transaction added to every read, whatever its size, so its
weight is set by how small the caller's reads are. blob_db's dominant read is
the slot-header walk, and `struct slot_head` is **12 B** — so the hottest read
in the system carries 32 B of someone else's metadata, 267 % of its own payload.
Averaged over the mix (`slot_verify` also pulls whole payloads), the measured
read is 61.7 B and becomes 93.7 B.

That makes it a tax in **either** cost regime, and §3.2a says which one is
which:

| per `byid` op | `flash_area` | UBI | |
|---|--:|--:|--:|
| at today's 65.5 µs/tx + 0.63 µs/B | 9.57 ms | 17.4 ms | **+82 %** |
| at a hypothetical 5 µs/tx + 0.25 µs/B (bus-bound) | 1.87 ms | 3.07 ms | **+64 %** |

Fixing the driver does not rescue it — the byte ratio is worse than the
transaction ratio, so the tax survives into the regime where reads are
proportional to size, which is the regime NOR actually has.

**And it is not really a cache problem.** Linux does not read the VID header on
a dynamic-volume read at all: `data_size` is a *static*-volume field there, and
a dynamic LEB reads to `usable_leb_size`. This cost exists solely to support the
divergence in §12.2 #5. So there are two fixes, and the second is free:

- cache `data_size` in the EBA node — it is immutable for the life of a mapping
  and the node is already resident, so ~8 B/LEB, ~1 KB for this device; or
- **drop the bound for dynamic volumes and match Linux**, which deletes the read
  rather than caching it.

The per-read `check = true` CRC goes with it either way. Linux validates the VID
header at attach and during scrubbing, not on every data read — and re-reading
metadata per read is an expensive integrity strategy that protects the header
and not the payload, which blob_db already CRCs itself.
[`2026-08-16-blob-db-on-ubi.md`](2026-08-16-blob-db-on-ubi.md) makes it a
prerequisite: until it lands, UBI pays 2× on every read, which is enough to eat
the erase win that makes the backend worth having.

**3. The in-place path does not recover from a write failure.** Linux
`ubi_eba_write_leb()` reacts to a failed write with `recover_peb()`: take a
fresh PEB, copy the LEB across, retry, torture and mark the old one — so the
caller sees success and the LEB is never left damaged. Here `write_at` returns
`-EIO` with the LEB partially written and the mapping untouched; recovery is the
caller's problem. The atomic path *does* behave (`leb_mark_peb_bad()`, old
mapping intact), so the divergence is specific to the path blob_db uses for
every append.

**4. The alignment precondition is silently repaired instead of enforced.**
Linux requires offset *and* length aligned to `min_io_size` and returns
`-EINVAL` otherwise. Here only the offset is checked; `ubi_leb_data_write()`
pads a short tail from a **zero-filled** buffer and writes a whole write-block.
On NOR that drives those bytes permanently to 0 — in an append log, the next
record's space. blob_db escapes it only because it rounds every write itself
(`slot_size_for()`, and the `0xff`-filled staging in `append_slot2()` and
`write_master()`), so it always lands on the aligned path.

**5. `data_size` is load-bearing on dynamic volumes.** Linux uses the VID
header's `data_size` for static volumes only; a dynamic LEB is readable to
`usable_leb_size`. Here a whole-LEB `ubi_leb_write()` records `data_size = len`
and **bounds later reads to it**, with `data_size == 0` meaning "whole LEB".
So mixing the two write forms on one LEB changes what can be read back.
blob_db only ever uses `write_at`, so it always sees `data_size == 0`.

One more, less a divergence than a different cost model: **reclaim is
caller-driven.** Linux unmaps immediately and erases later in a background
wear-levelling thread. Here `blob_db_store_erase()` loops
`ubi_device_erase_peb()` itself to keep the free pool up, so the erase is paid
inline. For a single-threaded store that is arguably the better trade —
deterministic rather than deferred — but it means an unmap is not the cheap
operation Linux's contract makes it.

And a caveat on all five: the Zephyr side is read from this tree; the Linux side
is not, because there is no kernel source in the environment these were written
in. `drivers/mtd/ubi/eba.c` and `kapi.c` are where to check #2 and #3.

Neither write path is power-fail atomic on the in-place side, and the header
says so — which is what blob_db's per-slot CRC and the compaction seal (B2)
exist to detect.

### 12.3 It also demonstrates `FINDINGS.md` B11

The run reports `live content: 4336158 B = 51.6 % of the partition` — the same
figure as the `flash_area` run, and **wrong**. `persondb.c`'s `geometry()`
opens `storage_partition` and reports its raw size, but under UBI `blob_db`
lives on a *volume* over that partition: fewer LEBs, each smaller than a
sector. The true denominator is smaller, so the true occupancy is higher.

`persondb.c` already says this in a comment and `FINDINGS.md` records it as
**B11**. What was missing was a run that shows the wrong number being printed
with no error and no warning, which is the whole point of the finding: the app
cannot detect it, because no layer will tell it how big the store actually is.

### 12.4 Reproducing

```
west build -p always -b native_sim app_cbor_persondb --                 \
      -DCONFIG_BLOB_DB_BACKEND_UBI=y -DCONFIG_UBI_MAX_NR_OF_DATA_PEBS=256
./build/zephyr/zephyr.exe --flash=ubi.bin --flash_erase
```

The `UBI_MAX_NR_OF_DATA_PEBS` override is required: its default of 14 is far
below the 126 data PEBs an 8 MiB partition presents, and the failure is a clear
`-ENOMEM` at `ubi_device_init()`. The two on-flash layouts are not
interchangeable (`lib/blob_db/Kconfig` says so), so a backend switch needs a
fresh store.
