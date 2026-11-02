# blob_db export / import — the line format

Status: **as built** (`lib/blob_db/blob_db_shell_xfer.c`,
`scripts/blob_db_export.py`) · 2026-11-02
· Part of [the kvdb tools proposal](../proposals/2026-11-02-kvdb-tools.md)

`blob_db export` writes the live objects of a mounted store as text lines that
a host collects from a console. `blob_db import` takes the same lines back,
one per command, into a freshly formatted store. A dump is **logical**. It
holds every live id with its payload, and the id counter. It does not hold the
layout on flash: slots, garbage, generations, master state, fuses, UBI erase
counters. For those, take a raw image (`flash read`, or a debug probe).

Because it is logical, a dump moves a store between geometries, backends and
format versions: the target build places each object itself.

## 1. Lines

```
%BDB1 <type> <field> ... *<crc16>
```

- **Prefix** `%BDB1`: the format, version 1. Neither a Zephyr log line nor the
  shell prompt starts with it. A reader searches for it anywhere in a line,
  after removing ANSI escape sequences, because a prompt or a partial log line
  can precede it.
- **Fields** are separated by one space. No field contains `*` or a control
  character.
- **Checksum:** ` *` and four hex digits, the CRC-16/CCITT-FALSE (poly 0x1021,
  init 0xffff, not reflected, no final xor) of the line's text from the `%`
  up to the character before ` *`. On the device that is
  `crc16_itu_t(0xffff, ...)`; in Python, `binascii.crc_hqx(text, 0xffff)`.
  A line that fails it is dropped: a log message printed in the middle of it,
  or a garbled byte, costs that line only.
- Numbers are decimal, except a CRC-32 (8 hex digits) and data (hex). Hex
  digits are written lower case and read in either case.

## 2. Records

| Type | Fields | Meaning |
|---|---|---|
| `H` | `key=value` ... | Header: `fmt=<major>.<minor>`, `backend=ubi\|flash_area`, `sector=`, `buckets=`, `max_payload=`, `max_object=`, `next_id=`. A reader ignores keys it does not know. |
| `O` | `<id> <len>` | An object begins. |
| `D` | `<id> <off> <hex>` | `len(hex)/2` bytes of the object at byte `off`; 32 per line, the last one shorter. An empty object has none. |
| `E` | `<id> <len> <crc32>` | The object ends; CRC-32 (IEEE, zlib's `crc32`) of all its bytes. |
| `X` | `id <id> <errno>` | The device could not read that object; no `E` follows. |
| `X` | `bucket <bid> <errno>` | The device could not read that bucket; its objects are missing. |
| `T` | `objects= bytes= errors= lines=` | Trailer: objects exported, their bytes, `X` records, and the records before `T`. |

`D` lines name their object and offset, so a lost, repeated or reordered line
is visible and the data can be put together without trusting line order.
Objects come in bucket order, not id order. Segments of a large object are
not objects: its content comes as one object, read through its id.

```
%BDB1 H fmt=5.0 backend=ubi sector=65408 buckets=118 max_payload=256 max_object=256 next_id=500 *6850
%BDB1 O 1 5 *e415
%BDB1 D 1 0 726f6f7421 *3d8b
%BDB1 E 1 5 62c48356 *6664
%BDB1 O 17 0 *a48c
%BDB1 E 17 0 00000000 *434e
%BDB1 T objects=2 bytes=5 errors=0 lines=6 *9d9a
```

## 3. Commands

| Command | Output |
|---|---|
| `blob_db export` | `H`, every live object, `T` |
| `blob_db export <id>` | `H`, that object, `T` |
| `blob_db export <id> <off> <len>` | the `D` lines of that range, nothing else: to fetch again what arrived damaged |
| `blob_db import <line>` | one reply line (§4) |
| `blob_db import` | progress, in words |

`export` is read-only and needs a mounted store. Ids are enumerated with
`blob_db_inspect_bucket_get()`, and read with `blob_db_size()` and
`blob_db_read()` outside its callback. `blob_db_iterate()` cannot do it: its
callback must not read, and it hands a large object's segment table as the
payload. An application writing to the store meanwhile is not stopped: each
object is consistent, the dump as a whole is not a snapshot.

`import` writes. It is built with `CONFIG_BLOB_DB_SHELL_ADMIN`.

## 4. Import

`blob_db import` takes the rest of the command line as one line of a dump and
answers with one line:

```
%BDB1 A <type> [<id> [<off>]] [key=value ...] *....     accepted
%BDB1 N <type> [<id> [<off>]] err=<errno> [...] *....   refused, nothing changed
```

A refusal has a plain line with the reason above it. `N -` is a line that
failed its checksum. A refused `D` or `E` carries `expect=<off>`, the next
byte offset the device wants.

A host sends a line and waits for its reply, so the serial link needs no flow
control. On `N` or no reply it sends the line again. If the line just
accepted comes again (its reply was lost), the device repeats the reply and
applies nothing.

What each record does:

- **`H`** needs a mounted, freshly formatted store: only the root's empty
  slot in it (`blob_db format yes`). Anything else may hold a deleted id, and
  binding a deleted id is undefined (contract D3). Then it allocates ids until
  the store's `next_id` reaches the header's, so every object can be bound at
  its own id, and the ids the source had deleted stay unused. A target whose
  counter is already further keeps it. The reply gives the resulting
  `next_id`. At most 2^24 ids are allocated (about one master record per 256).
- **`O`** starts an object. Its id must be below the header's `next_id`. An
  object larger than `CONFIG_BLOB_DB_SHELL_IMPORT_BUF_SIZE` is refused with
  `-EFBIG`: each object is staged whole and written with one
  `blob_db_update()`, as the original was. The host build sets the buffer to
  128 KB; elsewhere it defaults to one slot's payload (4 KB with large
  payloads).
- **`D`** must continue the object at exactly the offset received so far.
- **`E`** checks the length and the CRC-32, writes the object, and reads it
  back to compare the CRC-32 again. A CRC-32 mismatch abandons the object; the
  host sends it again from `O`.
- **`X`** is counted. The restored store lacks what the source could not read.
- **`T`** compares objects, bytes and `X` records with what was restored. It
  closes the import only when they agree.

An import is not atomic: one that stops part-way leaves a partly restored
store. `blob_db format yes` and start again.

Segments take ids of their own, so after importing large objects the store's
`next_id` is past the source's.

## 5. Host side

`scripts/blob_db_export.py`:

```bash
# check a capture; name the `blob_db export <id> <off> <len>` commands that
# fetch what was damaged
python3 scripts/blob_db_export.py verify capture.log
# merge the resends in, and write manifest.json plus <id>.bin per object
python3 scripts/blob_db_export.py extract capture.log resend.log -o store/
# a dump from such a directory (edited, or made by hand)
python3 scripts/blob_db_export.py pack store/ > dump.txt
# the `blob_db import` commands for a capture or a directory
python3 scripts/blob_db_export.py commands --format store/ > restore.txt
```

On `native_sim`, the restore is a pipe (§ Host tool in `app_diag/README.md`).
Driving a device's serial console line by line is not in the script yet.
