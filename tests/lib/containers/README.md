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

## Running

```sh
west twister -T key_value_db/tests/lib/containers -p native_sim --inline-logs
```
