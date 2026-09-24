# Note — what changes, and what it means for `kvhash`

2026-08-09 · companion to `2026-08-09-large-payloads.md` and its cost addendum

---

## What would change in `blob_db`

**Stage 1 — housekeeping (no new features).**

1. Frozen 12-byte compatibility prefix on the master (`magic`,
   `format_major`, `format_minor`, `hdr_len`, `prefix_crc16`); mount refuses an
   unrecognized store with `-ENOTSUP` instead of reformatting it. **Breaking
   change to the on-flash format — and the last undetectable one.**
2. `append_slot()` stages the slot in the existing `.bss` scratch instead of a
   `MAX_PAYLOAD`-sized stack frame.
3. The real payload bound (`⌊(peb−16)/2⌋−14`) is checked at mount, where the
   sector size is known, instead of by a fixed Kconfig `range`.

**Stage 2 — large objects, behind `CONFIG_BLOB_DB_LARGE_PAYLOADS` (default n).**

4. A payload larger than one slot is stored as K segment slots plus an index
   slot at the user's id; the index write is the atomic commit.
5. New API: `blob_db_size()`, `blob_db_read()` (pread), `blob_db_write()`
   (pwrite).
6. New Kconfig: `BLOB_DB_MAX_OBJECT_LEN`, `BLOB_DB_SEGMENT_LEN`,
   `BLOB_DB_MAX_SEGMENTS`.

`CONFIG_BLOB_DB_MAX_PAYLOAD_LEN` **keeps its exact current meaning, default and
value** throughout.

---

## Effect on `kvhash`: none required

No source change, no Kconfig change, no behaviour change.

| `kvhash.c` | Today | After |
|---|---|---|
| `MAX_PAYLOAD` (`:45`) | `CONFIG_BLOB_DB_MAX_PAYLOAD_LEN` | same symbol, same value |
| `MAX_BUCKETS` (`:47`) | `(MAX_PAYLOAD − 8) / 8` = **31** at the 256 B default | unchanged |
| `dir_buf`, `bkt_buf` (`:54-55`) | 256 B each | unchanged |
| Directory / bucket blob layout | one blob payload each | unchanged |
| `app_perf_kvdb/prj.conf` (`MAX_PAYLOAD_LEN=1024` → 127 buckets) | works | unchanged |

This is the whole reason the proposal keeps the old symbol's meaning. An
earlier draft promoted `MAX_PAYLOAD_LEN` to mean "largest logical object"; that
would have compiled cleanly and then given `kvhash` 512 KB of `.bss`, a
32 767-bucket cap instead of 31, and hash buckets packing 256 KB apiece —
silently. See `2026-08-09-large-payloads.md` §7.

**One real consequence:** Stage 1 breaks the on-flash format, so existing
stores — including their `kvhash` maps — are not readable afterwards and must
be reformatted. Nothing is deployed, so this costs nothing today.

---

## What `kvhash` could gain later (opt-in, not part of this proposal)

`kvhash`'s documented v1 limits (`kvhash.c:22-26`) both come from "must fit one
blob payload". At the 256 B default that means:

- at most **31 buckets** per map;
- at most **~244 B per value** (256 − 4 − key length);
- at most **~7.9 KB** of key+value data in an entire map.

The pread/pwrite API removes the first limit almost for free, because the
directory is only ever accessed by index:

```c
dir_bucket(idx)     ->  blob_db_read (root, 8 + idx*8, &id, 8)
dir_set_bucket(idx) ->  blob_db_write(root, 8 + idx*8, &id, 8)
```

`dir_buf` disappears entirely — the directory never needs to be resident. That
**removes the bucket cap and gives back 256 B of `.bss`**, so `kvhash` gets
*smaller* while getting bigger maps.

Bucket blobs should stay small regardless: a bucket is linear-scanned, so
growing it trades lookup cost for capacity. More buckets is the right way to
scale a `kvhash`, and that is exactly what this unlocks.

Worth doing as a follow-up — `kvhash` is the natural first consumer of the new
API — but it is a separate change and not required by anything above.
