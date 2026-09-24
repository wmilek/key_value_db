# L0 — Flash Translation Layer

Status: v2 (the L0 boundary is the `blob_db_store` seam; two providers ship,
UBI is the default)
· Part of the stack in `doc/architecture.md` · Governed by `doc/principles.md`
· Provider implementations (non-normative): `doc/impl/l0_backends.md`

---

## 1. Role

L0 owns raw flash: erase blocks, write alignment, and — where the substrate
needs it — wear leveling, bad-block handling, and logical-to-physical block
mapping. It presents the layer above (L1, `blob_db`) with a **uniform array of
equally-sized, individually-erasable blocks**, and nothing else.

The **interface is the design decision; the provider behind it is
interchangeable.** That interface is `lib/blob_db/blob_db_store.h`, and a
build selects one provider through the `BLOB_DB_BACKEND` Kconfig choice.

```
┌──────────────────────────────────────────────┐
│ L1  blob_db                                  │
└──────────────────────────────────────────────┘
                 │  lib/blob_db/blob_db_store.h
                 │  blob_db_store_{open,close,read,write,erase}
                 │  struct blob_db_store_geom {peb_size, write_align, n_pebs}
                 ▼
┌──────────────────────────────────────────────┐
│ L0 provider (exactly one, Kconfig choice):   │
│   a) flash_area over a fixed DT partition    │   BLOB_DB_BACKEND_FLASH_AREA
│   b) a dynamic UBI volume                    │   BLOB_DB_BACKEND_UBI ← default
└──────────────────────────────────────────────┘
```

### 1.1 Why the boundary is not `flash_area` (supersedes the v1 plan)

v1 of this document put the boundary at Zephyr's `flash_area` API and specified
UBI integration as a *virtual `flash_area` provider* — "a pointer swap via
`CONFIG_BLOB_DB_PARTITION_LABEL`, zero code in this repository". That plan was
abandoned during implementation: a synthesized `flash_area` would have had to
hide block identity behind a flat offset and then reconstruct it, which costs
`blob_db` its append-in-place write and buys nothing.

The boundary therefore moved up one level, to a **narrower** contract than
`flash_area` (P6): five calls and three geometry fields, none of which assume a
Zephyr flash map. `flash_area` is now one provider behind the seam rather than
the boundary itself.

## 2. The interface contract

L1 consumes **only** the following, and every L0 provider must honor exactly
this — nothing more is ever assumed (P6):

| Primitive | Contract |
|---|---|
| `blob_db_store_open` | Opens the substrate and reports geometry: `peb_size` (usable bytes per block), `write_align`, `n_pebs`. |
| `blob_db_store_read` | Returns the last committed bytes at that offset. |
| `blob_db_store_write` | Offset and length aligned to `write_align`; a completed write is durable across power loss. Never spans two blocks. |
| `blob_db_store_erase` | Returns whole blocks to the erased state; afterwards the region reads back as `0xff`. |
| `blob_db_store_close` | Releases the substrate. Idempotent. |
| **Torn-write blast radius** | A power loss mid-write/mid-erase may leave arbitrary bytes **in the affected block only**. Other blocks are untouched. |
| **Geometry stability** | The same store reports the same `peb_size` and `n_pebs` on every boot. |

Two rows carry the weight.

**Torn-write blast radius.** L1's crash model absorbs any single-block
corruption but assumes corruption never leaks across block boundaries. A
provider that cannot promise this cannot carry `blob_db` — including one that
relocates blocks internally, which must commit the move atomically rather than
leave two partial copies.

**Geometry stability.** L1 places blobs by block count, so a store that reports
a different `n_pebs` on a later boot is a *different store*: every id resolves
elsewhere. A provider whose geometry depends on anything but the device must
derive it deterministically.

Two properties are deliberately **not** promised, and L1 must never grow a
dependency on either: that a block's usable size equals the physical erase-block
size, and that flat offsets correspond to physical addresses. Neither holds
under a translating provider.

## 3. The providers

Both are implemented. What distinguishes them, at this altitude, is only what
they guarantee about the substrate:

| | `flash_area` | UBI (default) |
|---|---|---|
| Wear leveling | none | yes |
| Bad-block handling | none | yes |
| Torn write during an internal move | n/a — no moves | contained (§2) |
| `peb_size` vs physical erase block | equal | smaller (per-block headers) |
| Cost | faster, smaller | slower reads, more flash |

`flash_area` maps 1:1 onto a device-tree partition and manages nothing, which
suits NOR and simulation and keeps blob_db's structures at known partition
offsets — useful to tests that inject faults there. UBI is the default on the
grounds that a store which survives a torn write is worth more than a faster
one.

Measured costs, geometry overhead, the operation mapping and the
per-board configuration each provider needs are implementation matters:
`doc/impl/l0_backends.md`.

## 4. Store compatibility

The two providers write **incompatible on-flash layouts**. Switching an
existing device between backends requires erasing the partition deliberately.

Mount does not reliably refuse the mismatch. One direction is refused cleanly
before `blob_db` mounts; the other is currently **destructive** — a
`flash_area` build reformats a UBI store rather than rejecting it, because a
foreign substrate is indistinguishable from a corrupt store at the point where
mount decides. Until that is closed, production builds should set
`CONFIG_BLOB_DB_AUTOFORMAT_ON_CORRUPT=n`, which turns the destructive case into
`-EIO` with nothing written.

This is a gap against the L1 contract's D1 rule — "distinct on-flash magic so a
mismatched mount fails cleanly with `-ENOTSUP`" — which was written for the
allocator axis and does not reach the backend axis. The measured behavior in
both directions, the reason, and the candidate fix are in
`doc/impl/l0_backends.md` §4 and `doc/impl/l1_bucketlog.md` §13.7.

## 5. Kconfig

L0 is selected through one choice, owned by L1's Kconfig
(`lib/blob_db/Kconfig`) because the seam is L1-internal:

```
choice BLOB_DB_BACKEND
    BLOB_DB_BACKEND_FLASH_AREA    raw partition via flash_area
    BLOB_DB_BACKEND_UBI           dynamic UBI volume        (default)
```

The partition is named by `CONFIG_BLOB_DB_PARTITION_LABEL` under either
backend.

## 6. What an L0 provider must never do

- Cache writes in RAM and acknowledge before they are durable (breaks P7).
- Reorder a write after a later write's acknowledgment (breaks L1's master
  generation ordering).
- Let a torn operation corrupt a block other than the one being
  written/erased (§2).
- Report a different `peb_size` or `n_pebs` for the same store on a later boot
  without an explicit reformat story (§2).
