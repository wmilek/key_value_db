# L2 containers — test suites

- **`logring/`** — implemented. Bounded circular log: append/iterate order,
  oldest-first eviction, monotonic/durable sequence numbers, over-capacity
  rejection, wrong-type `open`, persistence across remount. Runs under
  `west twister`.

The map/sequence containers (`seq`, `kvlist`, `kvhash`, `kvtree`) and the
shared shape-conformance suite are still SKELETON placeholders — planned but
not yet implemented; they land with those modules. See
doc/layers/l2_containers.md.
