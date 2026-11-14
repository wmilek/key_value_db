# Implementation design — `seq` sparse indexed array (L2)

Status: draft · **Not implemented** · proposed layout and algorithms for
`lib/containers/seq/`. Normative contract: `doc/layers/l2_containers.md` §4.1
and `include/app/lib/containers/shape_seq.h`. Upper layers depend only on that
contract, never on the layout below (P6), so everything here may change when
`seq` is built.

---

## 1. Shape

A fixed-capacity array of slots; each occupied slot names one element blob.
One or two levels of id arrays, like the `kvhash` directory
(`l2_containers.md` §4.3), and like it the geometry is derived at create from
`seq_config.max_elems` and fixed thereafter.

## 2. On-flash layout (draft)

```
root  { magic 'CSEQ', version, depth, flags, capacity, len,
        trim_from, trim_to, child_id[] }
  depth 1: child_id[i] is slot i — an element id, or 0
  depth 2: child_id[i] is a leaf, or 0 (no leaf yet: all its slots empty)
leaf  { slot_id[] }                                   one i-node, created lazily
element = the caller's bytes, verbatim                 one i-node per element
```

`flags` carries the destroying mark (§6). `trim_from`/`trim_to` hold a pending
release range (§5).

A slot is **occupied** when its id resolves (`blob_db_get` succeeds) and
**empty** otherwise: id 0, an id reserved but never bound, or a deleted id all
read as empty. Slots at or past `len` read as empty whatever they hold.

## 3. Capacity

With a ~24 B root header and 8 B per id, one level addresses about 29 slots at
the 256 B default payload and about 249 at 2016 B (the
`BLOB_DB_LARGE_PAYLOADS` default); two levels about 29 × 32 ≈ 900 and
249 × 252 ≈ 62 000. A `max_elems` past two levels is `-EINVAL` at create. An
element is its own blob, so its size limit is blob_db's — with large payloads
on, an element can itself be segmented.

## 4. Cost

Depth 2; depth 1 saves the leaf read and write.

| op | reads | writes |
|---|---|---|
| `get`, `len`, `stat` | root + leaf + element / root / root | 0 |
| `set`, slot occupied | root + leaf + element | **1** — the element, updated in place, id kept |
| `set`, empty slot below `len` | root + leaf + probe | 2 — leaf, element |
| `set`/`append` at or past `len` | root + leaf + probe | 3 — leaf, element, root |
| `del` | root + leaf + probe | 2 — element tombstone, leaf |
| `truncate` | root + leaves in range | root, then one tombstone per element and one write per leaf, then root |
| `next` | leaves in range + one probe per non-zero slot (+ element if asked) | 0 |

Replacing an element is the common case for slot tables and costs one write
with no stage: the element blob belongs to the sequence alone, so it is
rewritten in place (`l1_model_container.md` §3.4) and nothing that names it
changes.

## 5. Mutation order and crash analysis

`seq` does not use the generic stage/prepare/commit/cleanup/clear protocol of
`l2_containers.md` §2.2; per that section's last paragraph, this is its own
crash analysis against P7. Every mutation has one commit and needs no intent
blob, because blob_db reserves ids without writing (`alloc_id` → bind) and
reads an unbound or deleted id as `-ENOENT`:

| op | order | commit | residue of a crash |
|---|---|---|---|
| `set`, slot occupied | update the element | that update | none |
| `set`, empty slot below `len` | `alloc_id`; write the id arrays on the path holding the new id, bottom-up; bind the element | the bind | a slot naming an unbound id — reads empty, nothing on flash |
| `set`/`append` at or past `len` | as above, then write the root with the new `len` | the root write | a bound element just past `len` — reachable from its id array, reused by the next `set`/`append` there, released by `truncate`/`destroy` |
| `del` | delete the element; clear the slot | the delete | a slot naming a deleted id — reads empty |
| `truncate`, shrinking | write the root with the new `len` and `trim_from/trim_to`; delete each element in the range, then clear it from its leaf; delete leaves wholly past `len`; write the root clearing the range and their entries | the first root write | elements in the trim range, still reachable; finished by the next mutation |
| `truncate`, growing | write the root with the new `len` | that write | none |

The generic protocol says a dangling reference is impossible and, if seen,
an anomaly to repair. Here it is deliberate: a slot is written naming an id
*before* the id is bound, and an element is deleted *before* its slot is
cleared, so a slot whose id does not resolve is the ordinary encoding of
"empty". That is still P7 — a reference that does not resolve is absent — and
it is what keeps one commit per mutation.

Two rules make the table hold:

- **`set` binds only an id it has just reserved, or one that resolves.** A
  slot whose id answers `-ENOENT` gets a fresh `alloc_id`, because the
  sequence cannot tell an id reserved but never bound from a deleted one, and
  binding a deleted id is undefined (L1 D3).
- **A pending trim is finished before any other mutation.** The trim range is
  inline in the root, as `logring` keeps its eviction range
  (`doc/impl/l2_logring.md` §5), so an interrupted `truncate` is completed by
  whichever mutation comes next, and is bounded by the range.

## 6. Destroy

Destroy (`l2_containers.md` §2.4) writes the root with the destroying flag —
every op answers `-ENOENT` from then on — deletes each leaf's elements and then
the leaf, and deletes the root last. `-ENOENT` from any delete counts as done,
so a repeat resumes where the last attempt stopped.

## 7. Invariant checklist (`l2_containers.md` §5)

1. Single-integer reachability — every element and leaf is named from the root.
2. Typed root — `'CSEQ'` magic.
3. One commit point per mutation — §5.
4. O(1) RAM — no handle state; every op starts from the root.
5. Bounded stack — one payload-sized buffer for an id array.
6. Resumable destruction — §6.

## 8. Open items

- **Depth-1 double root write.** At depth 1 the root is also the id array, so
  a fill at or past `len` writes it twice — once naming the reserved id, once
  with the new `len`. Folding those into one write is to be decided when `seq`
  is built.
- **Inline small elements.** Elements of up to 7 bytes could live in the slot
  itself, saving a read and a blob each. The contract allows it; not planned
  for v1.

## 9. History

Until 2026-11 `seq` was specified as a byte stream — a chain of chunk nodes
behind `len/read/append/truncate` on bytes — meant for file bodies. L1
decision D4 (amended; proposal D10) moved large values into L1 itself, with
O(1) seek and pread/pwrite, and blobfs bodies are L1 blobs, so that role is
gone. The shape was redefined around what L1 cannot do: many separately
replaced values under one root.
