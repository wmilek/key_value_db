# `app_cbor_persondb` — good practices for building on this stack

A worked example: a CBOR-serialized person/credential database holding ~10 000
people in ~4 MiB — about half the nRF5340-DK's external flash — with the access
decision, crash safety and capacity planning a real product would need.

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

`src/persondb.h` speaks persons, cards, permissions and decisions. It does not
export a key, a shard index, a blob id or a CBOR byte. Everything above it —
the scenario layer, both frontends — is written against that vocabulary.

*What it prevents:* storage decisions leaking into application code, where they
become impossible to change. The layout in `persondb.c` was redesigned twice
during this work (L3 → L2, 8 shards → 16) and neither change touched a line
above it. Acceptance criterion **A7** checks this rather than trusting it.

### 2. Use the highest layer whose *shape* matches — then drop down

`kvdb` (L3) is the ergonomic interface and it does not fit a sharded dataset:
one instance per name means nine registry entries, nine meta blobs and eighteen
sector reads at boot, and it overruns `CONFIG_ROOTREG_MAX_ROOTS` (default 8).
Dropping to the L2 Map shape — one registry key, one app-owned superblock,
seventeen map roots — costs **two** sector reads at boot.

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

The reverse order in row 1 would leave a credential granting access on behalf
of a person who does not list it — a crash that fails **open**. Same two
writes, same cost, opposite security posture.

*See* `persondb_card_assign()` / `persondb_card_revoke()` in `src/persondb.c`.

### 6. Don't denormalize until you have measured the thing you'd be avoiding

The tempting optimization is a permission bitmask cached in the credential
index, turning the access decision into one lookup instead of two. This app
refuses it, and measures what the refusal costs (`RESULTS.md`): the decision is
2 map gets, and the CBOR decode inside it is **1 %** of the time on `native_sim`
and a projected **0.01 %** on hardware.

*What it prevents:* buying a second copy of the truth — and the consistency
problem in practice 5 — before knowing whether the first copy was the problem.
Note what falls out for free: because permissions live only in the person
record, `persondb_permission_grant()` is a *single atomic write* with no
ordering to get right.

### 7. Make your data a pure function of a key, so you can verify without a journal

`src/dataset.c` derives every field of person *i* from *i*. Verification
re-derives what the store should hold and compares; there is no shadow copy,
no expected-value table, and no RAM proportional to the dataset.

*What it prevents:* being unable to check your own store. The Map shape has no
iteration (**K6**), so a store cannot be enumerated — without a regenerable
dataset, a rerun has no way to discover what a previous boot wrote. It is also
the only reason `scenario_report_get()` can state how full the flash is.

### 8. Compare canonical encodings instead of hand-written field comparisons

`CONFIG_ZCBOR_CANONICAL=y` makes a record's bytes a pure function of its
content, so `same_person()` encodes both sides and `memcmp`s.

*What it prevents:* a comparison that silently stops covering a field when the
schema grows.

### 9. Commit progress in idempotent batches

`scenario_fill()` writes a batch and then commits a counter to the superblock.
Replaying a partial batch rewrites identical values, so an interrupted fill
resumes rather than restarting — which matters when the fill is a projected
~2.5 h on the DK.

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

### 11. Assert the geometry your configuration assumes

`CONFIG_BLOB_DB_MAX_PAYLOAD_LEN` must fit *twice* inside a sector or a full
bucket can be written once and never rewritten (**B10**), and nothing in the
library checks it (**B9**). `src/persondb.c` carries a `BUILD_ASSERT` for the
configured geometry and re-checks the real one in `persondb_open()`.

*What it prevents:* an app that works on 64 KB-sector QSPI and fails
mysteriously partway through a fill on 4 KB-sector internal flash.

### 12. Count your own flash traffic

Nothing below reports occupancy, entry counts or I/O (**B3**, **K10**), so
`persondb.c` counts map operations and converts them to bytes with the sector
size it read from devicetree. That is where every amplification figure in
`RESULTS.md` comes from — including the one that says an access decision moves
**256 KB of flash to answer a question about 365 bytes**.

*What it prevents:* optimizing the part you can see instead of the part that
costs.

### 13. Separate "operations on the data" from "how they are invoked"

`src/scenario.c` fills, verifies, mutates, benchmarks and reports — and never
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

```
src/dataset.c      pure generator: index -> person. No storage, no CBOR.
src/person_cbor.c  the wire format (zcbor). No storage.
src/persondb.c     the domain API, on kvhash map_ops + rootreg + blob_db.
src/scenario.c     operations on a population. Returns structs, never prints.
src/ui_bench.c     frontend: the unattended run.
src/ui_shell.c     frontend: interactive commands.
tools/sizing.py    offline capacity check — see practice 10.
```

Dependencies point one way only, mirroring the stack's own P6.

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

## Rerunning

Every run verifies content a *previous* boot wrote, so persistence is proven
rather than asserted:

```
native_sim: ./zephyr.exe --flash=db.bin        (--flash_erase to start over)
hardware:   reset the board; CONFIG_APP_CBOR_PERSONDB_FRESH_START=y wipes once
```
