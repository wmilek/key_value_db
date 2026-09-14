# Implementation design — `kvhash` container

Status: v1 · **Non-normative implementation design.** This document describes
how `kvhash` satisfies the L2 Map shape (`doc/layers/l2_containers.md`) and
what that costs. Everything here — record layout, magics, step ordering — may
change as long as the shape's contract holds. Callers must not depend on
anything in this document (P6); `include/app/lib/containers/shape_map.h` is
what they may rely on.

---

## 1. On-flash layout

A kvhash instance is a *directory* blob — the map's root id — plus one blob per
non-empty bucket. A map is one or two levels deep; the directory header says
which, so a reader never has to guess:

```
directory blob (the root, and at depth 2 each sub-directory):
  [u32 magic] [u16 n] [u8 version] [u8 depth] [u64 child_id]*n
bucket blob (packed pair list):
  ( [u16 klen] [u16 vlen] [key bytes] [val bytes] )*
```

At `depth 1` each `child_id` is a bucket. At `depth 2` each is a sub-directory —
itself a well-formed `depth 1` directory, so one loader serves both levels — and
the buckets hang off those. The geometry is fixed at create and never changes.

A key is placed by one CRC-32 of the key, split into two independent indices:
the top 16 bits choose the sub-directory, the low 16 the bucket within it. One
hash, two levels, no second pass over the key. At depth 1 only the low half is
used. Each bucket is a short linear scan, so a lookup costs two flash reads one
level deep and three at two. A bucket blob is created lazily on first insert —
id 0 in a directory means "empty".

Two magics are defined, and the second is what §3 is about:

| Magic | Meaning |
|---|---|
| `'KVHA'` | a live map |
| `'KVHD'` | `destroy` has committed; the buckets are still being released |

Bounds: a directory must fit one blob payload, so `n` is capped at
`(MAX_PAYLOAD - 8) / 8` — call it `MAX_BUCKETS`, 31 at the 256 B default and
4091 at 32 KB. One level therefore addresses `MAX_BUCKETS` buckets and two
address `MAX_BUCKETS²`, which is where the ceiling now sits; there is no third
level. A single bucket's packed list must also fit one payload; an insert that
would overflow it returns `-ENOSPC`. Both are v1 limits, not fundamental — a
future revision can add depth or chain overflow blobs.

### 1.1 Choosing the geometry

The caller does not pick a bucket count. It declares what it is about to store —
`expected_entries`, and optionally `typical_entry_bytes` and `max_entry_bytes` —
and `create` derives the shape. An all-zero config is legal and means "I do not
know": it builds a small one-level map (`DEFAULT_BUCKETS`), which is the right
answer for the many maps that hold a handful of keys.

The derivation, in order:

1. **Contradictions are refused.** `max_entry_bytes` past what a record can
   hold, or `typical > max`, is `-EINVAL` before anything is written — it is
   arithmetic, so it costs nothing to check and the caller learns at create.
2. **One level while one level fits.** Aim for `SMALL_MAP_LOAD` (4) entries per
   bucket, reduced if `max_entry_bytes` says four would not fit under the
   near-full threshold. If that wants no more than `ONE_LEVEL_MAX_BUCKETS`
   (255) buckets, build it flat.
3. **Otherwise two levels**, sized at about one entry per bucket but floored so
   that a bucket is worth its own blob: `MIN_BUCKET_BYTES` (4096) of expected
   content per bucket. Below that floor the per-blob overhead — an i-node, a
   directory slot, a read — costs more than the shorter scan saves. The two
   levels are made equal (`n = ceil(sqrt(buckets))`), which minimises the
   metadata a lookup touches for a given bucket count.
4. **A declaration that cannot be honoured is refused**, not quietly built
   smaller: if even a square two-level geometry needs more than `MAX_BUCKETS`
   per level, `create` returns `-EINVAL`.

Step 4 is the point worth defending, because the previous behaviour was to
clamp. Clamping is the worse failure: the caller is told nothing at create,
gets a map an order of magnitude too small, and meets the consequence much
later as `-ENOSPC` on whichever key happens to land in a full bucket — with
nothing connecting that back to the sizing decision that caused it
(`app_cbor_persondb/FINDINGS.md` K9). Refusing the declaration puts the error
at the point where the caller can still act on it.

These five constants are policy, not format: a map records the geometry it was
built with, so moving them changes only maps created afterwards.

`stat()` reports what was built — depth, fan-out, bucket count, and the entry
size limit the caller was measured against — so a caller that declared a
population can see what it got.

## 2. Concurrency

Single-threaded, per the blob_db v1 contract. Two file-scope scratch buffers are
reused across calls — one holds the directory, the other the bucket being read
or rewritten — so the caller must serialize. Two levels need no third buffer:
the top directory is consumed to find the sub-directory's id and the same
buffer then holds the sub-directory, because nothing in the top is needed
afterwards. `destroy` is the exception and says why in §3.4. Note the
consequence for a future
`iterate`: calling any op from inside a callback would clobber the bucket
buffer mid-walk, so collect-then-mutate is the only supported pattern.

## 3. `destroy`

The caller's contract is `l2_containers.md` §2.4. This is how it is met.

### 3.1 The commit is a stamp, not a delete

`destroy` writes `'KVHD'` over the directory's magic — one atomic update, with
every bucket id left in place — and only then releases the buckets, the root
last.

Both halves of that are forced, and it is worth recording why, because the two
obvious orderings each fail:

- **Delete the root first.** It looks like the correct commit: the root is the
  single step that flips reachability, which is what §2.2 asks for. But the
  directory is the only record of *which buckets exist*, so a crash between the
  root delete and the bucket deletes takes the ids with it and strands the
  buckets with nothing left anywhere to name them. Not residue reclaimed by a
  later format — unrecoverable.
- **Delete the buckets first, unstamped.** The ids survive, but the window
  leaves a live directory naming tombstoned ids. A reader in that window is
  answered from a bucket that no longer resolves, and `set` on such a bucket
  returns `-ENOENT` instead of re-creating it — a container that never repairs
  itself, which is the one outcome §2.2 rules out.

Stamping gives both properties at once. The container is out of view from the
instant the stamp commits, so no reader observes a partial map; and the ids are
still there, so an interrupted release can finish.

### 3.2 Why no intent blob

§2.2's watermark exists because *prepare* creates i-nodes that nothing
references yet: after a crash they are unfindable, so their ids must be recorded
out of band. `destroy` is the mirror — everything it releases is named by the
root, and the root is still there. The delete set is re-read rather than
journalled, which is why `destroy` needs no staging and no `W`, and why the root
has to die last for that to hold.

### 3.3 Resumability and errors

`dir_load_raw()` accepts both magics and reports which it found, so a repeated
`destroy` on a stamped root skips the commit and resumes the release. The data
path (`dir_load()`) turns the dying state into `-ENOENT`, so `get`/`set`/`del`
are refused at the *root* — the whole container answers, never per key, and no
caller sees the part of the data still on flash.

Within the release loop:

- a bucket answering `-ENOENT` is the resumed case, not a failure;
- a real failure is remembered but does not stop the loop — every bucket
  released is progress the repeat need not redo;
- if any bucket failed, the root is **not** deleted and the error is returned,
  leaving the container stamped so the caller's repeat picks it up.

The root is deleted only once every bucket is gone, which is what keeps §2.4's
"repeating is always safe" true at every point.

### 3.4 Two levels: a tree, released bottom-up

At depth 2 the same three rules apply, one level down as well, and the order is
what keeps a repeat safe:

- **The top stamp is still the only commit.** It is written first and nothing
  else is stamped. From that instant the whole map is `-ENOENT` to readers, so
  no caller observes the sub-maps still on flash, and a repeat re-enters with
  `dying` already true and simply carries on.
- **Children die before their parents.** A sub-directory's buckets are released,
  then the sub-directory, then — once every sub-directory is gone — the root.
  A surviving parent therefore always still names whatever has not been released
  yet, which is §3.1's argument applied at each level. The reverse order would
  strand blobs that nothing points at.
- **A partial level is not an error to unwind.** A sub-directory that answers
  `-ENOENT` was already released; a sub-directory whose buckets did not all go
  is left in place, still naming the rest, and the first real error is returned
  with the root still stamped. Every blob released is progress the repeat need
  not redo.

This is the one place a second scratch buffer is needed (§2): walking a
sub-directory loads it over the top directory, so the sub-directory ids are
copied into the bucket buffer first. That buffer is free during a destroy —
nothing is being packed — and a directory that fits one payload fits it by
construction.

## 4. Open implementation items

Known deltas against the layer documents, each pinned by a test in
`tests/lib/containers` so it cannot regress silently.

### 4.1 Wrong-type root returns `-EIO`, not `-EINVAL`

`l2_containers.md` §2.3 specifies `-EINVAL` for a root of the wrong type.
`dir_load_raw()` returns `-EIO` on a magic mismatch.

The cause is structural rather than a slip: with no `open` in `map_ops`, the
function that answers "this is not a kvhash root" is the same one that answers
"the flash read failed", so the two collapse into one return. Restoring `open`
(the designated home for type validation) fixes it properly; short of that it is
a hand-written special case.

### 4.2 `create` on a populated root orphans its buckets

`create` re-initialises the directory unconditionally. Called on a root that
already holds a map, it makes every existing bucket unreachable without
releasing it — a permanent leak, which P7 forbids. There is no `-EEXIST`.

The correct sequence today is `destroy` then `create` at a fresh id. Either
`create` should learn `-EEXIST`, or the shape should state that the caller
guarantees create-once.

### 4.3 A bucket that fills has no recovery

A bucket that outgrows one payload returns `-ENOSPC`, and the map cannot grow
to escape it — the geometry is fixed at create, and K6 (no iteration) means the
contents cannot be copied into a larger map either. The only point at which
this is preventable is the declaration passed to `create`.

An earlier revision tried to soften that with a near-full warning: `set`
already knows how full the record it just rewrote is, so it returned a positive
value once a bucket passed 60 % of the payload. It was removed, and the reason
is worth keeping.

The warning was free to *produce* but not free to *have*. A positive success
return is an exception to "non-zero means trouble" that every caller must know
about: the one application that handled it (`app_cbor_persondb`) had already
aborted a fill by reading `rc != 0` as failure, the L3 wrapper `kvdb_set()`
forwarded the value while its own header documented only `0`, and 26 assertions
in this suite plus 19 in `tests/lib/kvdb` read it as a failure. Against that,
the single consumer did nothing with the value beyond incrementing a counter
for a printed line — discarding the one thing an in-band return offers over a
log line, which is *which key* tripped it — and that counter read `0` in every
recorded run, because the derived geometry of §1.1 sizes buckets so it does
not fire.

So the mitigation was carried by 45 call sites that could misread it, for one
that aggregated it away. The real cure is online resize, which v1 does not
have; `set` is `0`-or-negative like every other op in the shape.

The 60 % figure survives in `BUCKET_FILL_PCT`, where it does a different and
load-bearing job: it is the margin §1.1 sizes one-level buckets against, so a
population whose entries run larger than declared still fits.

*(The former item here — `initial_capacity` read as a bucket count, the source
of FINDINGS.md K9 — is resolved: the field is gone, replaced by the declared
population of §1.1, and an unhonourable declaration is now `-EINVAL` at create
rather than a silent clamp.)*
