/*
 * One-shot partition eraser — throwaway.
 *
 * The QSPI holds a store whose format major the target build does not know, and
 * blob_db deliberately refuses to mount it. blob_db_format() cannot help here:
 * it requires st.mounted, so it is unreachable for exactly the store that needs
 * discarding. So go under the library and erase the raw partition, leaving the
 * blank slate a virgin board would present.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>

#include <app/lib/blob_db.h>

#define STORAGE_ID FIXED_PARTITION_ID(storage_partition)

int main(void)
{
	const struct flash_area *fa;
	int rc;

	rc = flash_area_open(STORAGE_ID, &fa);
	if (rc != 0) {
		printk("fmt_once: flash_area_open FAILED rc=%d\n", rc);
		return 0;
	}
	printk("fmt_once: partition at 0x%lx, %zu B — erasing (~1 s per 64 KB "
	       "sector, be patient)\n",
	       (unsigned long)fa->fa_off, (size_t)fa->fa_size);

	rc = flash_area_erase(fa, 0, fa->fa_size);
	flash_area_close(fa);
	if (rc != 0) {
		printk("fmt_once: erase FAILED rc=%d\n", rc);
		return 0;
	}
	printk("fmt_once: erase ok\n");

#ifdef VERIFY_MOUNT
	/* A blank partition must mount as a fresh store. Prove it here rather
	 * than discovering otherwise in the middle of the benchmark.
	 *
	 * Off by default: mounting *writes* a blob_db store, so the partition is
	 * no longer blank afterwards. The UBI backend formats on first use only
	 * if it finds an erased partition — hand it one with blob_db metadata on
	 * it and ubi_device_init() reports "no active reserved PEBs" and fails
	 * -EIO, which is correct of it and confusing to debug.
	 */
	rc = blob_db_mount();
	if (rc != 0) {
		printk("fmt_once: mount-after-erase FAILED rc=%d\n", rc);
		return 0;
	}
	blob_db_unmount();
	printk("fmt_once: DONE - store is mountable\n");
#else
	printk("fmt_once: DONE - partition is blank\n");
#endif
	return 0;
}
