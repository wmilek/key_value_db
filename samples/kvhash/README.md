# kvhash sample — the Map shape, one operation at a time

The smallest complete program that uses `kvhash`, the L2 hash-map container:
it opens the storage stack, creates a map, and then performs each `map_ops`
operation once, printing the call and its return code.

Nothing here is timed. For throughput numbers see
[`app_perf_kvdb/`](../../app_perf_kvdb).

## What it shows

`kvhash` exports exactly one symbol — `kvhash_map_ops`, the op vector
`create / get / set / del` declared in
[`shape_map.h`](../../include/app/lib/containers/shape_map.h). Every op takes
the map's **root id** as its first argument. There is no handle to keep alive,
nothing to close, no RAM state between calls:

```c
static const struct map_ops *const map = &kvhash_map_ops;

map->set(root, key, strlen(key), val, strlen(val));
map->get(root, key, strlen(key), buf, sizeof(buf), &len);
map->del(root, key, strlen(key));
```

That leaves the caller two jobs beyond the four operations, and they are the
first two steps of the sample:

1. **Where the root id comes from.** `rootreg_get_or_create()` maps a
   compile-time key (`ROOTREG_KEY('SMPL', 0)`) to a root id, so the next boot
   re-finds the same map. One `uint64_t` bootstraps the whole structure.
2. **When to call `create()`.** Once, when that root does not hold a structure
   yet — `blob_db_exists(root) == false`. Never again on a later boot, which is
   why the sample branches on it rather than calling `create()` unconditionally.

The remaining steps each isolate one behavior worth knowing before you write
against the API:

| Step | Point |
|---|---|
| 3 | `set()` inserts *or* replaces — one call, and values are opaque bytes (one is a packed struct) |
| 4 | `get()` copies the value out and reports its length |
| 5 | a short buffer returns `-ENOMEM` with `*out_len` set to the true length, so you can size and retry; `out = NULL, out_sz = 0` is the existence probe (present iff `0` **or** `-ENOMEM`) |
| 6 | replacing a value is the same `set()` call as inserting one |
| 7 | `del()` on a missing key returns `-ENOENT` — the answer, not a failure |
| 8 | re-resolving the root id yields the same map: the id *is* the map |
| 9 | the v1 bound — a bucket is packed into one blob payload, so a pair that cannot fit is refused with `-ENOSPC` and the map is left untouched |

Keys and values are byte slices (pointer + length) at this layer; the sample
passes string keys *without* their NUL terminator. If you want NUL-terminated
C-string keys with this same container underneath, use `kvdb`
([L3](../../include/app/lib/kvdb.h)) instead — it is a thin veneer over exactly
these ops.

## Run it

```shell
west build -p always -b native_sim samples/kvhash
./build/zephyr/zephyr.exe
```

Expected output (abridged):

```
kvhash sample 1.0.0 — the L2 Map shape, one operation at a time

[1] bring the storage stack up
    blob_db_mount() -> 0
    blob_db_erase_all() -> 0  (start from an empty store)
    rootreg_init() -> 0

[2] find the map's root id, and create the map the first time
    rootreg_get_or_create('SMPL':0) -> 0, root id 2
    create(root, .initial_capacity=16) -> 0   [first run]

[3] set — insert pairs
    set("device/name", "kv-demo-01") -> 0
    ...
[5] get — sizing a buffer, and probing for existence
    get("net/ssid", out_sz=4) -> -12 (-ENOMEM), true length 11
    get("net/ssid", out_sz=32) -> 0, 11 bytes: "workshop-2g"
    probe("net/ssid")   -> -12 -> present: yes
    probe("net/secret") -> -2 (-ENOENT) -> present: no
...
[9] the v1 bound — a bucket must fit one blob payload
    set("log/dump", 256 bytes) -> -28 (-ENOSPC; payload limit is 256)
    get("device/name") -> 0, 10 bytes: "kv-demo-01"

sample complete
```

On hardware:

```shell
west build -p always -b nrf5340dk/nrf5340/cpuapp samples/kvhash
west flash
```

### Seeing the map survive a reboot

By default the sample erases the store at boot (`CONFIG_SAMPLE_KVHASH_FRESH_START`)
so every run prints the same narration. Turn that off and give `native_sim` a
file-backed flash to watch step 2 take the *reopen* path — the map is found
through the root registry, and the value stored by the previous run is read
back before anything is written:

```shell
west build -p always -b native_sim samples/kvhash -- -DCONFIG_SAMPLE_KVHASH_FRESH_START=n
./build/zephyr/zephyr.exe --flash=kvhash.bin --flash_erase   # first run: creates
./build/zephyr/zephyr.exe --flash=kvhash.bin                 # later runs: reopens
```

```
[2] find the map's root id, and create the map the first time
    rootreg_get_or_create('SMPL':0) -> 0, root id 2
    root already holds a map, create() skipped   [reopen]
    get("device/name") -> 0, 10 bytes: "kv-demo-01"
```

On real hardware there is nothing to configure: flash persists, so just reset
the board.

## Things to try

- Raise `CONFIG_BLOB_DB_MAX_PAYLOAD_LEN` (256 by default): more and larger
  pairs fit in a bucket, and the bucket-directory ceiling rises with it (31
  buckets at 256 bytes). Step 9 sizes its oversized value from the same symbol,
  so it stays refused — the bound is a whole *bucket*, not one value.
- Change `N_BUCKETS` in [`src/main.c`](src/main.c) and rerun with
  `CONFIG_SAMPLE_KVHASH_FRESH_START=n`: the new value is ignored, because the
  bucket count is frozen into the map when it is created and there is no online
  resize in v1.
- Set `CONFIG_BLOB_CONTAINER_KVHASH_LOG_LEVEL_DBG=y` to see the container's own
  view of each operation next to the sample's narration.

## Further reading

- [`doc/layers/l2_containers.md`](../../doc/layers/l2_containers.md) §4.3 — the
  container contract and kvhash's on-flash layout
- [`doc/layers/l1_root_registry.md`](../../doc/layers/l1_root_registry.md) — the
  registry step 2 uses
- [`lib/containers/kvhash/kvhash.c`](../../lib/containers/kvhash/kvhash.c) — the
  implementation; the file header carries the byte layout
