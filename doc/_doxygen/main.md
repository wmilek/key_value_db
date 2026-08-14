# API Reference

This is the API documentation for [key_value_db] — a crash-safe, layered
key-value storage stack for Zephyr.

The public headers live under `include/app/lib/` and are grouped by layer:

- **blob_db** (L1) — stable `uint64_t` id → blob, crash-atomic per operation.
  The always-present core; everything else is built on it.
- **rootreg** (L1½) — the root registry: compile-time key → structure root id,
  owner of id = 1.
- **Container shapes** (L2) — `map_ops` / `seq_ops`, the abstract shapes an
  interface binds to, plus the concrete containers implementing them.
- **kvdb** (L3) — string key → value store over any Map container.

The design documents behind these APIs — layer contracts, atomicity model,
on-flash formats and their rationale — are published next to this reference;
see `doc/architecture.md` in the repository, or the [design documentation].

[key_value_db]: https://github.com/wmilek/key_value_db
[design documentation]: https://wmilek.github.io/key_value_db
