/*
 * Replay a captured flash read trace and report any bucket header that
 * fails its CRC. Trace entries are physical partition offsets.
 */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/crc.h>
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

#ifndef HP_MODE
#define HP_MODE 0
#endif

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

#ifndef DELAYS_US
#define DELAYS_US 0, 20, 100
#endif
#ifndef PASSES
#define PASSES 3
#endif

static const struct flash_area *fa;
static uint8_t buf[2048] __aligned(4);

static bool hdr_bad(const uint8_t *h)
{
	uint32_t want;

	if (memcmp(h, "BDBH", 4) != 0) {
		return false;
	}
	memcpy(&want, h + 12, 4);
	return crc32_ieee(h, 12) != want;
}

static unsigned int run(unsigned int delay_us)
{
	unsigned int bad = 0;

	for (size_t i = 0; i < ARRAY_SIZE(trace); i++) {
		const struct tr *t = &trace[i];
		uint8_t *d = buf + ((t->len % 4) ? 2 : 0);

		if (delay_us) {
			k_busy_wait(delay_us);
		}
		(void)flash_area_read(fa, t->a, d, t->len);
		if (t->len == 16 && (t->a & 0xffff) == 0x80 && hdr_bad(d)) {
			bad++;
			printk("REPLAY bad header #%u at entry %u/%u a=0x%06x:", bad, (unsigned)i,
			       (unsigned)ARRAY_SIZE(trace), t->a);
			for (int k = 0; k < 16; k++) {
				printk(" %02x", d[k]);
			}
			printk("\n");
		}
	}
	return bad;
}

#ifdef REPLAY_MINIMIZE
/*
 * Delta-debug the trace: find the shortest suffix before entry TGT that still
 * corrupts TGT, then drop chunks of it. Prints the surviving reads as MINSEQ.
 */
#define TGT 43297
static uint8_t keep[TGT];

/* Run the kept entries before TGT, then TGT; true if TGT reads bad. */
static bool fails(void)
{
	for (size_t i = 0; i < TGT; i++) {
		if (keep[i]) {
			const struct tr *t = &trace[i];

			(void)flash_area_read(fa, t->a, buf + ((t->len % 4) ? 2 : 0), t->len);
		}
	}
	(void)flash_area_read(fa, trace[TGT].a, buf, trace[TGT].len);
	bool bad = hdr_bad(buf);

	/* settle: a good read clears any pending state */
	(void)flash_area_read(fa, trace[TGT].a, buf, 16);
	return bad;
}

static void minimize(void)
{
	unsigned int lo = 0, hi = TGT, tests = 0;

	memset(keep, 1, sizeof(keep));
	printk("MIN full trace fails: %d\n", fails());
	/* largest start that still fails */
	while (lo < hi) {
		unsigned int mid = (lo + hi + 1) / 2;

		memset(keep, 0, sizeof(keep));
		memset(keep + mid, 1, TGT - mid);
		tests++;
		if (fails()) {
			lo = mid;
		} else {
			hi = mid - 1;
		}
	}
	memset(keep, 0, sizeof(keep));
	memset(keep + lo, 1, TGT - lo);
	printk("MIN suffix start %u (%u entries), fails=%d, %u tests\n", lo, TGT - lo, fails(),
	       tests);
	/* chunk removal, halving granularity */
	for (unsigned int c = (TGT - lo) / 2; c >= 1; c /= 2) {
		for (unsigned int s = lo; s < TGT; s += c) {
			unsigned int e = MIN(s + c, TGT);
			bool any = false;

			for (unsigned int i = s; i < e; i++) {
				any |= keep[i];
			}
			if (!any) {
				continue;
			}
			uint8_t sav[64];
			static uint8_t savbig[TGT];
			uint8_t *sv = (e - s) <= sizeof(sav) ? sav : savbig;

			memcpy(sv, keep + s, e - s);
			memset(keep + s, 0, e - s);
			tests++;
			if (!fails()) {
				memcpy(keep + s, sv, e - s);
			}
		}
		unsigned int n = 0;

		for (unsigned int i = lo; i < TGT; i++) {
			n += keep[i];
		}
		printk("MIN chunk %u: %u entries left, %u tests\n", c, n, tests);
		if (c == 1) {
			break;
		}
	}
	printk("MIN final fails=%d fails=%d\n", fails(), fails());
	for (unsigned int i = lo; i < TGT; i++) {
		if (keep[i]) {
			printk("MINSEQ %u 0x%06x %u\n", i, trace[i].a, trace[i].len);
		}
	}
	printk("MINSEQ target 0x%06x %u\n", trace[TGT].a, trace[TGT].len);
}
#endif

int main(void)
{
	static const unsigned int delays[] = { DELAYS_US };

	flash_area_open(FIXED_PARTITION_ID(storage_partition), &fa);
	printk("REPLAY %u entries\n", (unsigned)ARRAY_SIZE(trace));
	{
		uint8_t b[4];

		(void)flash_area_read(fa, 0, b, 4); /* make sure QSPI is up */
	}
	set_lh(HP_MODE);
#ifdef REPLAY_MINIMIZE
	minimize();
	printk("REPLAY DONE\n");
	return 0;
#endif
	for (unsigned int p = 0; p < PASSES; p++) {
		for (unsigned int k = 0; k < ARRAY_SIZE(delays); k++) {
			uint32_t t0 = k_uptime_get_32();
			unsigned int bad = run(delays[k]);

			printk("REPLAY pass %u delay %u us: %u bad, %u ms\n", p, delays[k], bad,
			       k_uptime_get_32() - t0);
		}
	}
	set_lh(false); /* leave the chip in its default mode */
	printk("REPLAY DONE\n");
	return 0;
}
