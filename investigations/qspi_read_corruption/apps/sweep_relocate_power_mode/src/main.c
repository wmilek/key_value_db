/*
 * Relocated trigger: burst A (trace[41070..43296]) with every block number
 * shifted by k (mod 128), then targets in the shifted 0x10..0x1e region.
 * Also tries shifting by 0x08 (half a region) and 0x01.
 */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <nrfx_qspi.h>
#include <hal/nrf_clock.h>

/* Same clock setup the Zephyr driver applies around each QSPI op (anomaly 159). */
static nrf_clock_hfclk_div_t clk_save;
static void clk_enter(void)
{
	clk_save = nrf_clock_hfclk_div_get(NRF_CLOCK);
	nrf_clock_hfclk_div_set(NRF_CLOCK, NRF_CLOCK_HFCLK_DIV_2);
	nrf_clock_hfclk192m_div_set(NRF_CLOCK, NRF_CLOCK_HFCLK_DIV_1);
	k_busy_wait(7);
}
static void clk_exit(void)
{
	nrf_clock_hfclk192m_div_set(NRF_CLOCK, NRF_CLOCK_HFCLK_DIV_4);
	nrf_clock_hfclk_div_set(NRF_CLOCK, clk_save);
}


/* MX25R6435F: RDSR 0x05 -> SR; RDCR 0x15 -> CR1, CR2; CR2 bit 1 = L/H switch
 * (0 = ultra low power, 1 = high performance). WRSR 0x01 writes SR, CR1, CR2. */
static int flash_regs(const char *tag, uint8_t *sr, uint8_t cr[2])
{
	nrf_qspi_cinstr_conf_t c = NRFX_QSPI_DEFAULT_CINSTR(0x05, NRF_QSPI_CINSTR_LEN_2B);

	c.io2_level = true;
	c.io3_level = true;
	clk_enter();
	int r1 = nrfx_qspi_cinstr_xfer(&c, NULL, sr);

	c.opcode = 0x15;
	c.length = NRF_QSPI_CINSTR_LEN_3B;
	int r2 = nrfx_qspi_cinstr_xfer(&c, NULL, cr);
	clk_exit();

	printk("REGS %s rc=%d,%d SR=%02x CR1=%02x CR2=%02x -> %s mode\n", tag, r1, r2, *sr, cr[0],
	       cr[1], (cr[1] & 0x02) ? "high-performance" : "ultra-low-power");
	return r1 ? r1 : r2;
}

static void set_lh(bool hp)
{
	uint8_t sr, cr[2];

	if (flash_regs("before", &sr, cr)) {
		return;
	}
	uint8_t w[3] = { sr, cr[0], hp ? (cr[1] | 0x02) : (cr[1] & ~0x02) };
	nrf_qspi_cinstr_conf_t c = NRFX_QSPI_DEFAULT_CINSTR(0x01, NRF_QSPI_CINSTR_LEN_4B);

	c.io2_level = true;
	c.io3_level = true;
	c.wren = true;
	c.wipwait = true;
	clk_enter();
	int rw = nrfx_qspi_cinstr_xfer(&c, w, NULL);
	clk_exit();
	printk("REGS write %02x %02x %02x rc=%d\n", w[0], w[1], w[2], rw);
	k_msleep(50); /* tW for WRSR is up to 30 ms */
	(void)flash_regs("after", &sr, cr);
}


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
	{
		uint8_t b[4];

		(void)flash_area_read(fa, 0, b, 4);
	}
	set_lh(true);
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
	set_lh(false);
	printk("S5 DONE\n");
	return 0;
}
