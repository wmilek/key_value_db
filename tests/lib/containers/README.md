# L2 containers — test suites

- **`logring/`** — implemented. Bounded per-entry log (chain of per-entry
  i-nodes): append/drain order and increasing ids, pre-reservation, tail
  rebuild and exact counts across unmount/mount, soft-budget eviction and
  evicted-position detection, cursor resume via a portable moniker, per-log
  epoch rejecting stale/foreign monikers, zero-length/binary records, reset
  and destroy. Runs under `west twister`.

The map/sequence containers (`seq`, `kvlist`, `kvtree`) and the shared
shape-conformance suite are still SKELETON placeholders — planned but not yet
implemented; they land with those modules. See doc/layers/l2_containers.md.
