# Building an Application on the Stack

Status: recommendations — nothing in the stack enforces them
· Part of the stack in `doc/architecture.md` · Builds on L3:
`doc/layers/l3_interfaces.md` · Worked example: `app_cbor_persondb/`

---

key_value_db gives an application blobs, a root registry, containers and
access interfaces. It does not prescribe how an application should arrange its
own data on top of them, and nothing below checks that it does. This page says
how it should be done. §1 is what every application should follow; §2 is
further recommendations for an application whose data model will change across
firmware versions. `app_cbor_persondb` follows both, and each point names where.

## 1. Recommendations

1. **The application should be built like persondb.** One part is the access
   interface, providing functions to operate on the data model, possibly seen
   as an application-specific L3 interface. The other part is the actual logic
   actions, which call this interface and do not call any layers below
   directly. All internals about i-nodes, hash keys etc. should not leak out
   from below L3.

   *In persondb:* `persondb/persondb.h` speaks persons, credentials,
   permissions and decisions — no keys, blob ids or CBOR. The scenario layer
   and both frontends are written against it (`app_cbor_persondb/README.md`
   practice 1).

2. **Register only one item in the root registry: the superblock.** Such a
   data model or access interface should preferably register only one item in
   the root registry, an object which is called the superblock. Its actual
   format is not specified, but it is supposed to have references to all
   persistent objects which are in use.

   Technically an application may register anything. But the registry is
   shared by every application on the partition and holds only
   `CONFIG_ROOTREG_MAX_ROOTS` entries, so the recommended approach is one
   structure describing a whole usage or subsystem, rather than a key per
   table.

   *Application* here means a program designed to perform a specific task for
   the user, not the firmware image. If one image contains persondb and a
   certificate database, they can be seen as two applications; as they are
   independent, each should have its own superblock.

   *In persondb:* `rootreg[ROOTREG_KEY('PADB', 0)] → superblock → people,
   credentials` (`persondb/persondb.c`, `DESIGN.md` §12).

3. **Use the serialization the rest of the system uses.** At this moment we
   probably know whether the application is using some serialization, such as
   CBOR, so CBOR can be used if the rest of the system is using it. Again, this
   is only a recommendation.

   *In persondb:* the superblock and every record are CBOR
   (`persondb/person_cbor.c`).

4. **Make the superblock ready for upgrade and downgrade.** At minimum it
   should provide some way to check if it matches the firmware (a version
   field). In some more advanced situations, it should provide a way to add
   more persistent containers, to allow an old → new upgrade (§2).

   *In persondb:* the superblock's first field is its version; a version 1
   store is upgraded at open by rewriting the superblock alone (`DESIGN.md`
   §12.2).

5. **An old version must not start a newer data model.** In case of a
   downgrade (intentional, or unintentional as in a development environment),
   the old version should avoid starting such a data model.

   *In persondb:* `attach_store()` refuses a superblock version it does not
   read with `-ENOTSUP` and writes nothing, so the newer firmware still finds
   the store intact.

## 2. Further recommendations: a table directory with classes

Recommendations 4 and 5 can go further. A plain version field makes every
downgrade a refusal. A table directory lets an older firmware keep working on a
store a newer one has extended, and tells it what to do with what it does not
know. This is persondb's model (`DESIGN.md` §12.3–12.4).

### 2.1 The table directory

The superblock lists the application's containers as a directory,
`table_id → { root, class, schema }`. A newer firmware adds a table by adding
an entry; the tables an older firmware knows are where they were.

### 2.2 Classes — what an older build does with a table it does not know

The build that creates a table records its class next to it, because an older
build cannot tell from an id it has never seen whether the table is safe to
ignore:

| class | A build that does not know the table… | For |
|---|---|---|
| **required** | refuses to open the store | data an older build would get *wrong* without it |
| **preserve** | keeps it, and keeps its entry on every superblock rewrite | data no older build's write can make inconsistent — an audit log, a configuration table |
| **derived** | destroys it and drops its entry at open, before its first write | indexes and caches over other tables, which the newer build rebuilds |

A class value newer than the build is treated as **required**: refusing is the
only answer that cannot lose or corrupt anything. Each table also carries the
record format it holds (`schema`); a build refuses a known table whose schema
it does not read rather than misread it.

### 2.3 Decide before writing

Classify every table first; only then destroy the derived ones. A store the
build refuses is left byte-identical, so the newer firmware that wrote it
still finds it intact. *In persondb:* `tables_resolve()`, then
`tables_drop()`.

### 2.4 Order of writes

- **Create:** the tables first, the superblock **last**. Its write is the
  single commit point; a crash before it leaves nothing reachable, and the
  next open builds again. *In persondb:* `create_store()`.
- **Drop a table:** destroy the container before rewriting the directory, so a
  power cut leaves at worst an entry naming a destroyed container, which the
  next open destroys again.
- **Destroy the store:** the tables while the superblock still names them,
  then the superblock, then the registry entry; each step repeatable after a
  power cut. Destroy only what the superblock references — never
  `blob_db_erase_all()` on a partition other applications share. *In
  persondb:* `persondb_destroy()`, `DESIGN.md` §12.4.

### 2.5 How far this is proven

In persondb, no table is derived yet, so nothing rebuilds one; what ships is
the older build's side, exercised by `tests/app_cbor_persondb` planting the
tables a newer build would leave. The crash-safety of the orders in §2.4 is
argued in persondb's comments and `DESIGN.md`, not tested by power-cut
injection.
