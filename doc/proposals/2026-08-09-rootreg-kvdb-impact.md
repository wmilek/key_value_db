# Note — effect on `rootreg` and `kvdb`

2026-08-09 · companion to `2026-08-09-kvhash-impact.md`
· changes summarised in that note's first section

---

## `rootreg` — no change required

It touches the payload cap in exactly one place:

```c
#define RR_IMAGE_MAX (sizeof(struct rr_hdr) +                    /* 8  */    \
		      CONFIG_ROOTREG_MAX_ROOTS * sizeof(struct rr_entry))  /* 16 each */
BUILD_ASSERT(RR_IMAGE_MAX <= CONFIG_BLOB_DB_MAX_PAYLOAD_LEN, ...);   /* rootreg.c:47 */
```

| | Today | After |
|---|---|---|
| `RR_IMAGE_MAX` at the default 8 roots | 136 B | unchanged |
| The `BUILD_ASSERT` (`:47`) | compares against `MAX_PAYLOAD_LEN` | same symbol, same meaning |
| Max roots that fit | 15 at the 256 B default, 63 at 1024 B | unchanged |
| Registry image layout at id = 1 | one inline payload | unchanged |
| `rootreg_init` / `get_or_create` / `set` / `get` | — | unchanged |

### That `BUILD_ASSERT` is now load-bearing

It is what keeps **id = 1 an inline blob**. Under the symbol rename an earlier
draft proposed (see `2026-08-09-large-payloads.md` §7), it would have silently
become a check against 256 KB — and a large `ROOTREG_MAX_ROOTS` would have
turned the bootstrap anchor into a *segmented* blob, entangling id = 1 with the
index record, the orphan sweep, and mount's root-invariant check
(`blob_db.c:367-401`). Keeping `MAX_PAYLOAD_LEN`'s meaning keeps that assert
doing its job for free.

**Recommendation: keep the registry inline on purpose.** It is the one blob the
whole store bootstraps from, and "every mutation is a single atomic
`blob_db_update(1, …)` — no intent, no recovery machinery" (`rootreg.c:7-9`) is
its entire value. A build needing hundreds of roots should nest a container
under one root, not grow the root itself.

Two smaller observations, neither caused by this proposal:

- `ROOTREG_MAX_ROOTS` has `range 1 1000`, but 1000 roots needs a 16 008 B
  payload — unreachable at any current setting. Same over-promising `range` as
  `BLOB_DB_MAX_PAYLOAD_LEN`'s `1 4096`, except this one fails **loudly** at
  build time, which is the acceptable kind.
- `rr_load` and `rr_store` each put a `RR_IMAGE_MAX` buffer on the stack
  (`:64`, `:95`) on top of the caller's `struct rr_image`, so a call costs
  ~2 × `RR_IMAGE_MAX` (~272 B at the default, ~32 KB if the `range` were
  reachable). Worth a look whenever `MAX_ROOTS` is raised.

**Worth noting the other direction too:** `rr_load` already returns `-ENOTSUP`
for foreign bytes at id = 1 and refuses to touch it (`:76-79`). That is exactly
the discipline decision D0 adds to `blob_db_mount()`. `rootreg` is the model
here, not the follower.

---

## `kvdb` — no change required

`kvdb` never references `CONFIG_BLOB_DB_MAX_PAYLOAD_LEN`, and its own record is
a fixed 32 bytes:

```c
BUILD_ASSERT(sizeof(struct kvdb_meta) == 32, "kvdb_meta must be a tight 32 bytes");
```

| | Today | After |
|---|---|---|
| `struct kvdb_meta` | fixed 32 B | unchanged |
| `kvdb_open` / `set` / `get` / `delete` / `has` | — | unchanged |
| Kconfig (`BLOBDB_KVDB`, backend choice) | — | unchanged |
| Max value size | inherited from the backend | inherited from the backend |

### It is the biggest beneficiary — but not from Stage 2 alone

`kvdb` has no size limit of its own; it forwards to `map_ops`. So today's
user-visible ceiling — **~244 B per value** at the 256 B default, from
`kvhash`'s packed bucket — is a `kvhash` limit surfacing through `kvdb_set()`
as `-ENOSPC`.

Large `kvdb` values therefore need **three** changes, not one:

1. `blob_db` gains `read`/`write` (Stage 2) — proposed;
2. **`map_ops` gains partial access** — `get`/`set` are whole-value today
   (`shape_map.h:63-85`), so nothing below can expose a large value upward;
3. `kvdb` passes it through — trivial once (2) exists.

Step 2 is an **L2 contract change** and is not part of this proposal. Worth
stating plainly so nobody expects `kvdb` values to grow when Stage 2 lands:
they will not, until `map_ops` grows too.

The cheaper win arrives first and needs none of that: giving `kvhash` more
buckets via `blob_db_read` on its directory (see the `kvhash` note) raises how
many keys a `kvdb` instance holds, without changing how big one value can be.

---

## Summary

| Module | Source change | Kconfig change | Behaviour change | Note |
|---|---|---|---|---|
| `kvhash` | none | none | none | could later drop `dir_buf` and the 31-bucket cap via pread |
| `rootreg` | none | none | none | its `BUILD_ASSERT` is what keeps id = 1 inline — keep it that way |
| `kvdb` | none | none | none | biggest beneficiary, but blocked on a `map_ops` change |

The one consequence shared by all three: **Stage 1 breaks the on-flash format**,
so existing registries, maps and instances must be reformatted. Nothing is
deployed, so this costs nothing today.
