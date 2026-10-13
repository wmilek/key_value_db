# blobfs sample — a file system from devicetree, with no init code

The smallest program that uses blobfs through Zephyr's file system API, with
the whole storage stack declared in devicetree instead of brought up in code.
It keeps a boot counter in a file, leaves a note next to it, and lists the
directory.

## What it shows

There is no `blob_db_mount()`, `rootreg_init()` or `fs_mount()` in
[`src/main.c`](src/main.c). The board overlay
([`boards/native_sim.overlay`](boards/native_sim.overlay)) says what the
storage is made of, layer by layer:

```dts
&storage_partition {
	compatible = "kvdb,ubi", "zephyr,mapped-partition";  /* a UBI device */
	reg = <0x00800000 DT_SIZE_M(8)>;

	volumes {
		blobdb_vol: ubi-volume-blobdb {             /* one volume */
			volname = "blobdb";
		};
	};
};

/ {
	chosen {
		kvdb,blob-db = &blobdb0;
	};

	blobdb0: blob-db {                                  /* blob_db on it */
		compatible = "kvdb,blob-db";
		storage = <&blobdb_vol>;
	};

	fstab {
		compatible = "zephyr,fstab";

		blobfs0: blobfs {                           /* blobfs on that */
			compatible = "kvdb,fstab,blobfs";
			mount-point = "/blob";
			db = <&blobdb0>;
			automount;
		};
	};
};
```

This is the shape Zephyr uses for littlefs (`zephyr,fstab,littlefs`). With
`automount`, blobfs is mounted during boot, which mounts the blob_db store
under it and bootstraps the root registry on the way. `prj.conf` only asks for
blobfs and its VFS glue; blob_db itself is switched on by the `kvdb,blob-db`
node.

Two things are worth knowing:

- **How to tell that the automount worked.** It only logs a failure, as
  littlefs's does. The sample's first step is `fs_stat()` on the mount point.
- **The mount point lives in devicetree.** The sample reads it with
  `DT_PROP(DT_NODELABEL(blobfs0), mount_point)` rather than repeating `"/blob"`.
  To mount or unmount it yourself, drop `automount` and use
  `FS_FSTAB_DECLARE_ENTRY(DT_NODELABEL(blobfs0))` and
  `fs_mount(&FS_FSTAB_ENTRY(DT_NODELABEL(blobfs0)))`.

The background is in
[`doc/proposals/2026-10-12-devicetree-configuration.md`](../../doc/proposals/2026-10-12-devicetree-configuration.md),
and the bindings are under [`dts/bindings/`](../../dts/bindings).

## Run it

```shell
west build -p always -b native_sim samples/blobfs
./build/zephyr/zephyr.exe --flash=blobfs.bin
./build/zephyr/zephyr.exe --flash=blobfs.bin    # the counter goes up
```

Expected output of the first run:

```
blobfs sample 1.0.0 — a file system from devicetree, no init code

[1] nothing to bring up: devicetree mounted /blob at boot
    fs_stat("/blob") -> 0

[2] read the boot counter
    fs_open("/blob/boot.cnt", READ) -> -ENOENT: first boot

[3] write it back, one higher, and leave a note
    fs_write("/blob/boot.cnt", 4 bytes) -> 4, fs_close -> 0
    boot count is now 1
    fs_write("/blob/note.txt", 18 bytes) -> 18, fs_close -> 0

[4] list /blob
    boot.cnt     file 4 bytes
    note.txt     file 18 bytes

sample complete (0)
```

On the nRF5340-DK the same stack sits on the on-board MX25R64 QSPI NOR
([`boards/nrf5340dk_nrf5340_cpuapp.overlay`](boards/nrf5340dk_nrf5340_cpuapp.overlay)):

```shell
west build -p always -b nrf5340dk/nrf5340/cpuapp samples/blobfs
west flash
```
