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
non-empty bucket:

```
root blob (the directory):
  [u32 magic] [u16 n_buckets] [u16 version] [u64 bucket_id]*n
bucket blob (packed pair list):
  ( [u16 klen] [u16 vlen] [key bytes] [val bytes] )*
```

`n_buckets` is fixed at create and never changes; a key lands in bucket
`fnv1a(key) % n_buckets`. Each bucket is a short linear scan, so a lookup costs
two flash reads. A bucket blob is created lazily on first insert — id 0 in the
directory means "empty".

Two magics are defined, and the second is what §3 is about:

| Magic | Meaning |
|---|---|
| `'KVHA'` | a live map |
| `'KVHD'` | `destroy` has committed; the buckets are still being released |

Bounds: the directory must fit one blob payload, so `n_buckets` is capped at
`(MAX_PAYLOAD - 8) / 8`. A single bucket's packed list must also fit one
payload; an insert that would overflow it returns `-ENOSPC`. Both are v1 limits,
not fundamental — a future revision can chain overflow blobs or rehash.

## 2. Concurrency

Single-threaded, per the blob_db v1 contract. Two file-scope scratch buffers are
reused across calls — one holds the directory, the other the bucket being read
or rewritten — so the caller must serialize. Note the consequence for a future
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

### 4.3 `initial_capacity` is read as a bucket count

`shape_map.h` documents `map_config.initial_capacity` as an "expected entry
count"; `buckets_for()` uses the number as the bucket count directly, clamped to
`[2, MAX_BUCKETS]`. A caller asking for 100 expected entries therefore requests
100 buckets and silently gets 31 at the default payload — no `-ENOSPC`, no
diagnostic.

Either the field's documented meaning or its interpretation has to move, and the
clamp should be stated wherever it lands.
