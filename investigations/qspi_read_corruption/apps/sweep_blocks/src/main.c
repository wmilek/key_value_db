/*
 * Sweep: for every 64 KB block, run a read burst, then read 16 B at
 * block + OFF and compare with clean re-reads. On a mismatch, find the
 * block whose data at the same offset ORs into the bad result.
 */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>

struct tr {
	uint32_t a;
	uint16_t len;
};

#include "trace.inc"

#define BURST_FIRST 41070
#define BURST_LAST  43296 /* inclusive; 43297 was the target */
#define NBLK        128
#define BLK         0x10000

static const struct flash_area *fa;
static uint8_t buf[2048] __aligned(4);

static void burst(int kind)
{
	for (unsigned int i = BURST_FIRST; i <= BURST_LAST; i++) {
		if (kind == 'B') {
			(void)flash_area_read(fa, 0x600000 + ((i - BURST_FIRST) * 0x40) % 0x20000, buf,
					      16);
			continue;
		}
		if (kind == 'C') {
			k_busy_wait(100);
		}
		(void)flash_area_read(fa, trace[i].a, buf + ((trace[i].len % 4) ? 2 : 0),
				      trace[i].len);
	}
}

static bool all_ff(const uint8_t *p)
{
	for (int i = 0; i < 16; i++) {
		if (p[i] != 0xff) {
			return false;
		}
	}
	return true;
}

static unsigned int test(int kind, uint32_t off, uint32_t blk, bool verbose)
{
	uint8_t got[16] __aligned(4), ok1[16] __aligned(4), ok2[16] __aligned(4);
	const uint32_t a = blk * BLK + off;

	burst(kind);
	(void)flash_area_read(fa, a, got, 16);
	(void)flash_area_read(fa, a, ok1, 16);
	(void)flash_area_read(fa, a, ok2, 16);
	if (memcmp(got, ok1, 16) == 0) {
		if (verbose) {
			printk("SWEEP %c off 0x%x blk 0x%02x ok%s\n", kind, off, blk,
			       all_ff(ok1) ? " (erased)" : "");
		}
		return 0;
	}
	printk("SWEEP %c off 0x%x blk 0x%02x BAD stable=%d got", kind, off, blk,
	       memcmp(ok1, ok2, 16) == 0);
	for (int i = 0; i < 16; i++) {
		printk(" %02x", got[i]);
	}
	printk(" want");
	for (int i = 0; i < 16; i++) {
		printk(" %02x", ok1[i]);
	}
	printk("\n");
	for (uint32_t b = 0; b < NBLK; b++) {
		uint8_t p[16] __aligned(4), o[16];
		bool match = true;

		(void)flash_area_read(fa, b * BLK + off, p, 16);
		for (int i = 0; i < 16; i++) {
			o[i] = ok1[i] | p[i];
			match &= o[i] == got[i];
		}
		if (match && b != blk && !all_ff(p)) {
			printk("SWEEP   partner blk 0x%02x (xor 0x%02x)\n", b, b ^ blk);
		}
	}
	return 1;
}

int main(void)
{
	static const uint32_t offs[] = { 0x80, 0x90, 0x100 };
	static const int kinds[] = { 'A', 'B', 'C' };

	flash_area_open(FIXED_PARTITION_ID(storage_partition), &fa);
	printk("SWEEP control 0x180080:\n");
	(void)test('A', 0x80, 0x18, true);
	(void)test('C', 0x80, 0x18, true);

	for (unsigned int k = 0; k < ARRAY_SIZE(kinds); k++) {
		for (unsigned int o = 0; o < ARRAY_SIZE(offs); o++) {
			unsigned int bad = 0, erased = 0;

			for (uint32_t blk = 0; blk < NBLK; blk++) {
				uint8_t v[16] __aligned(4);

				(void)flash_area_read(fa, blk * BLK + offs[o], v, 16);
				erased += all_ff(v);
				bad += test(kinds[k], offs[o], blk, false);
			}
			printk("SWEEP summary burst %c off 0x%x: %u/%u blocks bad (%u erased)\n",
			       kinds[k], offs[o], bad, NBLK, erased);
		}
	}
	printk("SWEEP DONE\n");
	return 0;
}
