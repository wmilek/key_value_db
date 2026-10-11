/*
 * Relocated trigger: burst A (trace[41070..43296]) with every block number
 * shifted by k (mod 128), then targets in the shifted 0x10..0x1e region.
 * Also tries shifting by 0x08 (half a region) and 0x01.
 */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>

struct tr {
	uint32_t a;
	uint16_t len;
};

#include "trace.inc"

#define B0  41070
#define B1  43296
#define BLK 0x10000

static const struct flash_area *fa;
static uint8_t buf[2048] __aligned(4);

static uint32_t sh(uint32_t a, uint32_t k)
{
	return ((((a >> 16) + k) % 128) << 16) | (a & 0xffff);
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

int main(void)
{
	static const uint32_t ks[] = { 0x00, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x08, 0x01 };

	flash_area_open(FIXED_PARTITION_ID(storage_partition), &fa);
	for (unsigned int kk = 0; kk < ARRAY_SIZE(ks); kk++) {
		const uint32_t k = ks[kk];
		char map[16] = { 0 };
		unsigned int hit = 0, n = 0;

		for (uint32_t t0 = 0x10; t0 <= 0x1e; t0++) {
			const uint32_t ta = sh(t0 << 16 | 0x80, k);
			uint8_t want[16] __aligned(4), got[16] __aligned(4);

			(void)flash_area_read(fa, ta, want, 16);
			if (t0 == 0x17 || all_ff(want)) {
				map[t0 - 0x10] = '.';
				continue;
			}
			for (unsigned int i = B0; i <= B1; i++) {
				(void)flash_area_read(fa, sh(trace[i].a, k),
						      buf + ((trace[i].len % 4) ? 2 : 0), trace[i].len);
			}
			(void)flash_area_read(fa, ta, got, 16);
			const bool bad = memcmp(want, got, 16) != 0;

			map[t0 - 0x10] = bad ? 'X' : '-';
			hit += bad;
			n++;
		}
		printk("S5 shift +0x%02x: targets 0x%02x-0x%02x %2u/%-2u [%s]\n", k, (0x10 + k) % 128,
		       (0x1e + k) % 128, hit, n, map);
	}
	printk("S5 DONE\n");
	return 0;
}
