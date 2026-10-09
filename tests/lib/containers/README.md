# L2 containers — test suite

Two ztest suites, both in `src/main.c`.

## `map_contract`

Written against the **shape** (`include/app/lib/containers/shape_map.h`), not
against a container. Every test loops over the `providers[]` table, so adding
kvlist or kvtree is one row plus one Kconfig line in `prj.conf` — the existing
tests then cover it unchanged. That portability is what the `map_ops` vtable
buys, so it is worth exercising.

The suite is two-tier:

- **Tier 1** asserts behaviour `shape_map.h` documents. A failure is a
  provider bug.
- **Tier 2** is marked `UNSPECIFIED-n` in the source. The shape does not
  document these yet, so the tests pin what kvhash does today and a second
  provider cannot silently disagree. Each marker names the open question. When
  the shape is amended, the marker goes.

Note that this suite builds **without kvdb or rootreg** — L2 is tested with no
L3 consumer present, which is the point of having a shape header.

## `kvhash_layout`

Provider-specific: things that follow from kvhash's directory-of-buckets
layout (capacity clamping, bucket overflow, foreign-root rejection). These
must **not** be promoted into the contract.

Two tests here are deliberately pinning known defects rather than desired
behaviour, and both say so in a comment:

- `test_wrong_type_root_is_refused` expects `-EIO`, while
  `doc/layers/l2_containers.md` §2.3 specifies `-EINVAL` for a wrong-type
  root. One of the two has to move.
- `test_create_on_populated_root_orphans_its_buckets` asserts the leak that
  `create()` on an already-built root causes. It flips when `create()` learns
  `-EEXIST`. Its opposite number is `map_contract`'s
  `test_destroy_releases_every_blob_it_owned` — the same `blob_db_count()`
  measurement, with the opposite expectation.

## `kvhash_spill` (`src/spill.c`)

kvhash's spilling of large keys and values into blobs of their own
(`doc/proposals/2026-10-09-kvhash-spill.md`). The requirement it pins: the
inline thresholds are a write policy, not a format. Entries written under one
threshold must read, walk and count identically under any other, and move only
when their key is next set. Placement is read off `blob_db_count()`. Fingerprint
collisions are forced with `CONFIG_BLOB_CONTAINER_KVHASH_TEST_HOOKS` (set in
`prj.conf`).

## `kvhash_spill_crash` (`src/spill_crash.c`)

Power loss inside a spilled set or delete. `kvhash_test_cut_after` (same test
hooks) makes kvhash's N-th flash write and every one after it do nothing, as if
power had gone; blob_db writes are atomic, so sweeping N from 0 until the
operation completes uncut reaches every state a power cut can leave. Eight
scenarios: insert into a fresh or an existing bucket, a same-length and a
new-length rewrite of a spilled value, inline-to-spilled and spilled-to-inline
rewrites, and delete with and without neighbours. After each cut and a remount,
the key must be wholly old or wholly new, progress must be monotonic,
neighbours intact, count must equal a full walk, a retry must succeed, and
destroy must leave no more unreferenced blobs than the scenario allows (none
without a cut).

## Running

```sh
west twister -T key_value_db/tests/lib/containers -p native_sim --inline-logs
```
