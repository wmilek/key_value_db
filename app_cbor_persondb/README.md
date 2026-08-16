# `app_cbor_persondb` — good practices for building on this stack

A worked example: a CBOR-serialized person/credential database holding 10 000
people, with the access decision, crash safety and capacity planning a real
product would need.

**What it measures is time per operation** — 44 µs to resolve a credential and
decide, on `native_sim` at the full 10 000 persons; 14.605 ms on the DK, where
the largest run so far is 1 000 persons and the full-scale figure is expected
near 24 ms (`RESULTS.md` §5, `FINDINGS.md` N1). The ~4 MiB of
data it carries (about half the board's external flash) is *ballast*: it exists
so those numbers are taken against a realistically-loaded store, not an empty
one. It earns its place — the same operation costs 38 % more at half-full than
at 2.5 % (`RESULTS.md` §3b).

It is also a probe. Everything it ran into is in **[`FINDINGS.md`](FINDINGS.md)**;
the design and the decisions behind it are in **[`DESIGN.md`](DESIGN.md)**;
measured numbers are in **[`RESULTS.md`](RESULTS.md)**.

```
west build -b native_sim app_cbor_persondb && ./build/zephyr/zephyr.exe --flash=db.bin
west build -b nrf5340dk/nrf5340/cpuapp app_cbor_persondb && west flash
```

---

## The practices

Each one names the failure it prevents, and points at the code.

### 1. Put a domain API between your application and the storage stack

`persondb/persondb.h` speaks persons, cards, permissions and decisions. It does not
export a key, a shard index, a blob id or a CBOR byte. Everything above it —
the scenario layer, both frontends — is written against that vocabulary.

*What it prevents:* storage decisions leaking into application code, where they
become impossible to change. The layout in `persondb/persondb.c` was redesigned twice
during this work (L3 → L2, 8 shards → 16) and neither change touched a line
above it. Acceptance criterion **A7** checks this rather than trusting it.

### 2. Use the highest layer whose *shape* matches — then drop down

`kvdb` (L3) is the ergonomic interface and it does not fit a sharded dataset:
one instance per name means **seventeen** registry entries, seventeen meta blobs
and thirty-four sector reads at boot, and it overruns
`CONFIG_ROOTREG_MAX_ROOTS` (default 8) twice over. Dropping to the L2 Map shape
— one registry key, one app-owned superblock, seventeen map roots — costs
**two** sector reads at boot.

*What it prevents:* paying for an abstraction whose shape you are fighting. The
comparison is tabulated in `DESIGN.md` §12; the decision is not "L2 is faster",
it is "L3's unit of naming is not this application's unit of structure".

### 3. Make every persistent structure reachable from one integer

```
rootreg[ROOTREG_KEY('PADB', 1)] -> superblock -> people_root[0..15], cred_root
```

`persondb_open()` reads one registry entry and one blob, and has the whole
database. No mount-time index rebuild, no journal replay, no side-band state.

*What it prevents:* boot cost proportional to the store, and the class of bug
where recovery depends on state that recovery is supposed to reconstruct.

### 4. Publish a multi-blob structure with a single final write

`create_store()` allocates seventeen map roots and creates each map, then binds
the superblock **last**. Until that one atomic write lands, none of it is
reachable and the next boot simply builds it again.

*What it prevents:* a half-built structure that reads as valid. Note the cost,
recorded as **B8**: the abandoned blobs are never reclaimed, because `blob_db`
has no reachability GC. The pattern is right; the platform does not yet finish
the job.

### 5. Order writes so that every crash point fails safe

There is no multi-key transaction (`blob_db` contract), so a record and its
index cannot be updated together. Pick the order deliberately:

| Operation | Order | Crash in between leaves |
|---|---|---|
| assign a card | **person, then index** | a card the person lists that resolves to nothing → **deny** |
| revoke a card | **index, then person** | a card already resolving to nothing → **deny** |
| delete a person | **credentials, then record** | credentials gone, record orphaned → **deny** |
| **replace a record** | **unindex dropped cards, then record, then index added ones** | either a card not yet listed or one already unindexed → **deny** |

The reverse order in row 1 would leave a credential granting access on behalf
of a person who does not list it — a crash that fails **open**. Same two
writes, same cost, opposite security posture.

**Row 4 is the one that gets forgotten.** An update that *drops* an indexed
attribute is two orderings at once, and this app shipped without it: a replace
that removed a card left the card's index entry live, so it kept resolving to a
person who no longer listed it — the fail-open outcome of row 1, reached with no
crash at all, by two ordinary calls. If a record has indexed attributes, "insert
or replace" means read the old one and diff it. Costs a get; see `RESULTS.md` §4
for what that is worth (10 % of the write path).

**Fail-safe is not the same property as repairable.** Ordering buys you the
first: every crash point denies. It does not buy the second, and the two get
conflated. `persondb_card_assign()` used to return early when the record already
listed the card, on the reasoning that a redo had nothing to write — but the
state where the record lists a card the index does not is exactly what a crash
between its two writes leaves, so the redo had to *finish* it, not skip it. The
index write is now unconditional. Then note what cannot be fixed the same way:
`persondb_card_revoke(card)` deletes the very index entry it needs to find the
person again, so its redo returns `-ENOENT` and stops. That is why
`persondb_card_revoke_from(person, card)` exists for callers that can name the
holder — and why the card-only form documents the limitation instead of hiding
it.

*See* `persondb_person_put()`, `persondb_card_assign()`,
`persondb_card_revoke_from()` in `persondb/persondb.c`.

### 6. Don't denormalize until you have measured the thing you'd be avoiding

The tempting optimization is a permission bitmask cached in the credential
index, turning the access decision into one lookup instead of two. This app
refuses it — and the honest price of refusing is **the second lookup**:
`persondb_check` is `card_owner` + `person_get`, 6.77 ms + 7.77 ms on the DK, so
**53 %** of every decision is the work a bitmask would have removed
(`RESULTS.md` §5b). That is the number the argument should be about.

What the 53 % buys is one copy of the truth. `persondb_permission_grant()` is a
*single atomic write* with no ordering to get right, whereas a cached bitmask
lands squarely in practice 5 — and it would have to cache the validity window
too, or the shortcut could grant on an expired badge. Refusing is a defensible
trade at this cost; it would not be at any cost, which is why the cost is
measured.

A separate question, routinely confused with that one: is the *serialization*
the problem? No. CBOR encode and decode together are **6.5 %** of a decision —
950 µs against 14.605 ms, measured on the DK. Leaner bytes would buy almost
nothing.

That 6.5 % was wrong twice before it was right. This file once claimed "a
projected 0.01 % on hardware" — a `native_sim` compute figure carried onto a
Cortex-M33, wrong by 159×. The correction then over-predicted at 9–14 %,
because the storage term it divided by was itself a guess. Only the board
settled it (`RESULTS.md` §5a). And for a while this practice quoted that 6.5 %
as *the cost of refusing to denormalize*, which it never was — it is the cost
of the codec, an order of magnitude below the lookup the refusal actually pays
for. The practice survived every correction; none of the numbers did.

*What it prevents:* buying a second copy of the truth — and the consistency
problem in practice 5 — before knowing whether the first copy was the problem.
And, on the evidence above, quoting a number that answers a neighbouring
question as though it settled this one.

### 7. Make your data a pure function of a key, so you can verify without a journal

`dataset/dataset.c` derives every field of person *i* from *i*. Verification
re-derives what the store should hold and compares; there is no shadow copy,
no expected-value table, and no RAM proportional to the dataset.

*What it prevents:* being unable to check your own store. The Map shape has no
iteration (**K6**), so a store cannot be enumerated — without a regenerable
dataset, a rerun has no way to discover what a previous boot wrote. It is also
the only reason `scenario_report_get()` can state how full the flash is.

### 8. Compare canonical encodings instead of hand-written field comparisons

`CONFIG_ZCBOR_CANONICAL=y` makes a record's bytes a pure function of its
content, so `persondb_person_equal()` encodes both sides and `memcmp`s.

*What it prevents:* a comparison that silently stops covering a field when the
schema grows.

### 9. Commit progress in idempotent batches

`scenario_fill()` writes a batch and then commits a counter to the superblock.
Replaying a partial batch rewrites identical values, so an interrupted fill
resumes rather than restarting — which matters when the fill is **≈ 2.2 h on
the DK**, extrapolated from a measured 13.5 min at a tenth of the scale
(`RESULTS.md` §5).

*What it prevents:* long provisioning runs that cannot survive a power cut, and
the resume logic that gets written badly under time pressure when they do.

### 10. Size to fit, and size by enumeration — not by a model

A `kvhash` bucket overflows at 4 KB while the store is nearly empty (**K2**),
there is no per-bucket occupancy query (**K10**), and the bucket count cannot
change after create (**K3**). So the margin must be chosen up front and blind.

An analytic compound-Poisson estimate said eight maps gave 5.5 σ of headroom.
The fill hit `-ENOSPC` at person 9 232. `tools/sizing.py` enumerates the actual
population through the actual hash and reports the fullest bucket; it says
sixteen maps, fullest bucket 66 % full. **Rerun it whenever the record shape,
the population size or `CONFIG_BLOB_DB_MAX_PAYLOAD_LEN` changes.**

*What it prevents:* discovering your capacity plan was wrong several hours into
a provisioning run, with no repair path short of a reformat.

**Then freeze the number.** Sizing is a one-time act. 10 000 persons was chosen
to put the store near half the board's flash; `tools/sizing.py` takes that as
its input and answers the question that follows from it — how many map shards
the population needs. From that point the person count is a constant of the
benchmark, because two runs are only comparable if they used the same one. The
*fill percentage*
that results is an output — and if a future stack stores the same 10 000 people
in 40 % of the flash instead of 51.6 %, that is the improvement being measured,
not a target to restore by growing the dataset (`RESULTS.md` §3a).

### 11. Let the layer below answer its own questions — delete the copy

`CONFIG_BLOB_DB_MAX_PAYLOAD_LEN` must fit *twice* inside a sector, or a full
bucket can be written once and never rewritten (**B10**). This app used to
assert that itself, which meant restating blob_db's slot overhead, its bucket
header size, the formula, and using `CONFIG_BLOB_DB_SECTOR_BUF_SIZE` as a proxy
for the sector size it could not otherwise obtain.

All four are now **deleted**. `blob_db` checks the same inequality at mount
against the real geometry — B9 and B10 were findings this app raised, and the
fix took the question away from callers rather than exporting the constants to
them.

*What it prevents:* four private constants drifting out of step with the layer
that owns them, silently. They were still correct when `main` changed that
layer — but nothing checked, and nothing could have.

*What is left:* the app still restates kvhash's capacity formula and per-entry
framing, and still reads the partition geometry behind blob_db's back, because
those questions have no API either. `FINDINGS.md` **X1** inventories all seven
and sketches the two read-only calls that would close the rest.

### 12. Count your own flash traffic

`persondb.c` reads `blob_db_iostats_get()` and reports what the flash actually
did. It used to *model* it — "map operations × sector size" — which was right
until `main` taught `blob_db` to walk buckets by slot header, and then wrong by
20×. Measure, do not model: that is where every amplification figure in
`RESULTS.md` comes from — including the one that says an access decision moves
**13.4 KB of flash to answer a question about 402 bytes**, an amplification of
33×.

This paragraph used to quote 256 KB and 711× there. Those were the *modelled*
figures, from exactly the arithmetic the practice is warning about — kept as an
illustration long after measurement had replaced them. Worth stating plainly:
the model was wrong in the direction that flatters the storage layer's critics,
and the measurement is 19× kinder.

*What it prevents:* optimizing the part you can see instead of the part that
costs.

### 13. Separate "operations on the data" from "how they are invoked"

`scenario/scenario.c` fills, verifies, mutates, benchmarks and reports — and never
prints. Both frontends are argument parsing and formatting over the same calls.

*What it prevents:* a benchmark and an interactive tool that drift into
measuring different things. It is also what makes the shell able to demonstrate
crash safety at all: `persondb fill 500`, pull the power, reboot,
`persondb stat`.

### 14. Ship a configuration small enough to run in CI

`sample.yaml` builds the headline 10 000-person configuration and the shell,
and *runs* a 200-person `smoke` scenario under a console harness. Fill, verify,
mutate and re-verify are regression-tested in seconds even though the real
configuration takes hours.

*What it prevents:* a test app that only ever gets exercised by hand.

---

## Layout

**One directory per layer, one `CMakeLists.txt` each.** The tree is the
architecture:

```
ui/         main.c, ui_bench.c, ui_shell.c   parse and print only
scenario/   scenario.c                       fill, verify, mutate, measure
persondb/   persondb.c                       the person management API
            person_cbor.c                    the wire format — private here
dataset/    dataset.c                        pure generator: index -> person
tools/      sizing.py                        offline capacity check (not built)
```

Dependencies point one way only, mirroring the stack's own P6 — and because
each directory publishes its own header directory, adding an edge between
layers means editing a `CMakeLists.txt`, not just typing an `#include`:

| directory | may include |
|---|---|
| `ui/` | `scenario.h` |
| `scenario/` | `persondb.h`, `dataset.h` |
| `persondb/` | `person_cbor.h` |
| `dataset/` | `persondb.h` (the record type only) |

`persondb/` is the only directory that *calls* `blob_db`, `rootreg` or
`map_ops`, and the only one that encodes or decodes CBOR — acceptance criterion
**A7**, checkable by grepping for those includes and call sites.

It is a claim about dependencies, not about vocabulary. Comments elsewhere do
name lower layers, where the observation being recorded is about them: the main
stack this app must reserve is set by `blob_db`'s slot builder, not by its own
call depth, and that belongs next to the number it explains. A7 would be easy to
pass by deleting those sentences, and the app would be worse for it.

## Shell

```
persondb stat                     geometry, fill, counters
persondb fill [n]                 populate n more persons (resumable)
persondb verify [n]               check n sampled persons against the generator
persondb mutate                   advance one revision
persondb bench [kind] [n]         measure
persondb show <index>             print a stored person
persondb cardof <index> [slot]    the card id for a dataset index
persondb card <card-id>           resolve a card to its owner
persondb check <card> <perm>      the access decision, timed
persondb grant|revoke <id> <perm>
persondb assign <id> <card> | unassign <card>
persondb reset                    erase and start over
```

## What this app does *not* show you

Worth stating, because a practices guide is read as a checklist:

- **Concurrency.** The whole stack is single-threaded — `blob_db`'s v1 contract,
  and `kvhash`'s scratch buffers are shared by every map in the image
  (`FINDINGS.md` K7). `persondb.h` says so; nothing *enforces* it. A second
  caller would corrupt the store silently, and this app never tries, so it
  demonstrates nothing about what a real product's locking should look like.
- **Recovery from a corrupt store.** A bad superblock or an unreadable registry
  entry surfaces as `-EIO` or `-ENOTSUP` from `persondb_open()`, and both
  frontends print the errno and stop. That is honest but not a recovery story:
  the only repair on offer is `persondb reset` (or `--flash_erase`), which
  discards everything. What a product should do instead — a second superblock,
  a rebuild from a durable source — is out of scope here.
- **Corrupt *records*, on the other hand, are handled**: a record that fails to
  decode returns `-EILSEQ` and is counted as a bad record by `verify` rather
  than crashing the run.
- **The full-scale board run.** `RESULTS.md` §5 is 1 000 persons. Acceptance
  criterion **A4** is not met until the 10 000-person run exists.

## Rerunning

Every run verifies content a *previous* boot wrote, so persistence is proven
rather than asserted:

```
native_sim: ./zephyr.exe --flash=db.bin        (--flash_erase to start over)
hardware:   reset the board; CONFIG_APP_CBOR_PERSONDB_FRESH_START=y wipes once
```
