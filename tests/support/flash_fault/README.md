# flash_fault — fail-after-N-writes flash_area shim (deferred)

SKELETON placeholder for a shared crash-injection shim: a flash_area provider
that fails (or tears) after a configurable number of writes, used by
allocator-level torn-write suites.

Not needed by the current model-container acceptance suite, which injects
crashes at blob_db-operation boundaries (stop mid-sequence + remount + run
recovery) — see tests/lib/blob_db_contract/. This shim lands when a suite
needs sub-operation torn-write injection beyond the existing L1 CRC tests.
