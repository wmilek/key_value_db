# What the driver and the datasheet say about the numbers

`RESULTS.md` measures. This file explains, from source: the nRF5340-DK's
`nordic,qspi-nor` driver (`zephyr/drivers/flash/nrf_qspi_nor.c`) and the
Macronix MX25R6435F datasheet (Rev. 1.6, 2022-08-08). Each finding names the
measurement it explains and the line of source or the datasheet section that
accounts for it, so a claim can be checked rather than believed.

Zephyr revision referenced: `5058917ea61b` (paths and line numbers from that
tree).

---

## 1. Why an unaligned read costs more: the driver splits it into three

**Measured** (`§5` full-resolution capture, 256 B read, size held constant,
only the start offset moved):

| start offset | µs/op | vs aligned |
|---:|---:|---:|
| 0 | 132.261 | — |
| 1 | 187.409 | +42 % |
| 2 | 218.504 | **+65 %** |
| 3 | 187.409 | +42 % |

**Cause**: `read_non_aligned()`, `nrf_qspi_nor.c:809`. The QSPI DMA requires
word-aligned addresses and lengths (`WORD_SIZE 4`, line 199), so a transfer
that is not word-aligned at both ends is decomposed into **up to three
separate `nrfx_qspi_read()` transactions plus a bulk `memmove`**:

```c
/* read from aligned flash to aligned memory */
if (flash_middle != 0) {
        res = nrfx_qspi_read(dptr + dest_prefix, flash_middle, addr + flash_prefix);
        ...
        /* perform shift in RAM */
        if (flash_prefix != dest_prefix) {
                memmove(dptr + flash_prefix, dptr + dest_prefix, flash_middle);
        }
}
/* read prefix */   -> nrfx_qspi_read(buf, WORD_SIZE, addr - offset);  memcpy
/* read suffix */   -> nrfx_qspi_read(buf, suffix_size, ...);          memcpy
```

So the guess that "there is some copy of read added" is right, and it is worse
than a copy: for a 256 B read at any non-zero offset the split is
`prefix + 252 + suffix`, which is **1 transaction becoming 3, plus a 252-byte
`memmove` of the payload already in RAM**.

**The arithmetic checks out**, using a second, independent measurement to price
one extra transaction. From the length probe, where the extra transaction is a
*suffix* rather than an offset effect:

| size | transactions | µs/op |
|---:|---:|---:|
| 16 B | 1 (middle only) | 70.257 |
| 17 B | 2 (middle 16 + suffix) | 94.701 |

One extra `nrfx_qspi_read()` costs **≈24.4 µs**. The per-call cost of
`flash_area_read()` — the model's 65.7 µs fixed term — is paid once per
`qspi_nor_read()` (device lock, DPD wake, completion wait), not per internal
transaction, so an unaligned read should cost aligned + 2 × 24.4 µs + memmove:

```
predicted  132.3 + 48.8 + memmove(252 B)  ≈  183 µs + a few
measured   187.4 µs                          (offsets 1 and 3)
```

That is the mechanism, quantified.

**Length matters independently of offset**, for the same reason: a length that
is not a multiple of 4 forces a suffix transaction even from an aligned start.
Word multiples are cheapest — 16 B costs 70.3 µs where 17 B costs 94.7 µs, and
the pattern repeats at 64 and 256.

### What is *not* explained

**Offset 2 costs 31 µs more than offsets 1 and 3, and the source does not
account for it.** The decomposition is identical in all three cases —
`flash_middle` is 252, the prefix and suffix reads are both `WORD_SIZE`, and
the `memmove` length is the same; only the *split* between prefix and suffix
differs (3+1, 2+2, 1+3). If anything the halfword-aligned `memmove` destination
at offset 2 should be the fastest of the three.

Treat it as unconfirmed: `read_offset` is a **single-pass** probe (unlike the
main sweeps, which run `CONFIG_APP_PERF_L0_PASSES` times), so a 31 µs excess is
within reach of one disturbed batch. Re-run it multi-pass before theorising.

### Consequence for the stack

`blob_db` reads at arbitrary offsets inside slots, and the sweep's read curve
is measured **aligned**. So 253.8 ns/B is a best case the stack does not get,
which is the correction `RESULTS.md` §5 already carries: part of what was
attributed to "CPU above L0" is this split happening below it. Settling the
split needs an alignment histogram at the store seam — this app cannot see
`blob_db`'s offset distribution.

**Cheapest available fix, and it is at L1, not L0:** align slot payloads so
`blob_db`'s reads start and end on 4-byte boundaries. The geometry already
gives `write_align = 4`; nothing forces the *read* offsets to respect it.

---

## 2. Which power mode: Ultra Low Power, and nothing can currently change it

**The driver never writes Configuration Register 2.** `qspi_wrsr()`
(`nrf_qspi_nor.c:524`) has exactly one caller — the Quad-Enable path at line
703. Which register that touches depends on the part's QER, taken from SFDP
DW15:

```
DK sfdp-bfp DW15 = 0xff29be00, bits 22:20 = 2  ->  JESD216 QER = S1B6
```

S1B6 means QE is **bit 6 of Status Register 1**, so the driver issues a WRSR
with **one data byte**. The datasheet (§10-9, p.32) is explicit that this
matters:

> The CS# must go high exactly at the 8 bits, 16 bits or 24 bits data boundary;
> otherwise, the instruction will be rejected and not executed.

An 8-bit WRSR writes the Status Register only. CR1 and CR2 need a 16- or 24-bit
write, which nothing in this driver issues. **CR2 bit 1 — the L/H switch — is
therefore never touched, and since it is a volatile bit, the part runs in
whatever mode it powers up in.**

**Zephyr implements this switch — for a different driver.**
`drivers/flash/spi_nor.c:815` has `mxicy_configure()`:

```c
/* Low-power/high perf mode is second bit in configuration register 2 */
/* lh_switch enum index: 0: Ultra low power, 1: High performance mode */
const bool use_high_perf = cfg->mxicy_mx25r_power_mode;
...
WRITE_BIT(new_cr, LH_SWITCH_BIT, use_high_perf);
ret = mxicy_wrcr(dev, new_cr);
```

gated on the `mxicy,mx25r-power-mode` devicetree property
(`enum: low-power | high-performance`). That property belongs to
`jedec,spi-nor`. The DK's part is
`mx25r64: mx25r6435f@0 { compatible = "nordic,qspi-nor"; ... }`, and the
`nordic,qspi-nor` binding has no such property — so on this board the mode is
**not reachable through devicetree at all**.

**Which mode is it, then?** Rev 1.6 removed the generic power-on default and
points at the ordering code (revision history, p.85), so the datasheet cannot
answer it. The measurement can, and both parameters agree:

| | measured | ULP typ | HP typ |
|---|---:|---:|---:|
| 256 B page program | 3.09–3.11 ms | 3.2 ms → **0.97×** | 0.85 ms → 3.66× |
| 64 KB block erase | 1086 ms | 800 ms → **1.36×** | 480 ms → 2.26× |

**Ultra Low Power.** A page program within 3 % of the ULP typical, at the room
temperature "typ" is defined at, is not a coincidence.

### What switching would buy, and what it costs

| | ULP typ | HP typ | change |
|---|---:|---:|---|
| 64 KB block erase | 0.8 s | 0.48 s | **−40 %** |
| 256 B page program | 3.2 ms | 0.85 ms | **−73 %** |
| 4 KB sector erase | 58 ms | 40 ms | −31 % |

The switch itself is cheap: a 24-bit WRSR, with its own AC parameter —
**tWMS = 20 µs** (Table 17). It costs current, not time, which is the trade the
part exists to offer.

On the measured numbers, `blob_db_prepare()` of 100 buckets is ~110 s of erase;
the same work in High Performance mode is specified at ~66 s. **That trade is
not currently being made — it is being defaulted into**, and it would take a
driver change to make it at all.

---

## 3. Erase: the part has far more to offer than the driver uses

### 3a. Erase suspend/resume — supported, and unused

The MX25R6435F supports **Program/Erase Suspend** (§10-29, p.55):

| | opcode | |
|---|---|---|
| PGM/ERS Suspend | `75h` or `B0h` | interrupts a Page Program, Sector Erase or Block Erase |
| PGM/ERS Resume | `7Ah` or `30h` | |

> After the program or erase operation has entered the suspended state, the
> memory array can be read except for the page being programmed or the sector
> or block being erased.

State is observable: Security Register **bit 3 (ESB)** is set while an erase is
suspended, bit 2 (PSB) while a program is. Latency to reach the suspended
state is **tESL = 60 µs** (ULP; 40 µs in HP).

**The forward-progress constraint is the design-critical part** (Table 17,
note 5):

> Erase operation may be interrupted as often as system request. The minimum
> timing of tERS must be observed […] **tERS ≥ 280 µs must be included in
> resume-to-suspend loop(s).**

So an erase can be interrupted arbitrarily often, but each resume must be given
≥280 µs of erase time or the erase may never complete. (Program: tPRS ≥ 100 µs.)

**What that is worth here.** The measured cost of blocking on a 64 KB erase is
~1086 ms of unavailable flash. Servicing one 132 µs aligned read per 280 µs
erase slice:

```
erase stretch  = (280 + 60 + 132) / 280   ≈ 1.7x   ->  ~1.8 s per block
read latency   = tESL + read + resume     ≈ 200 us  (was up to 1086 ms)
```

**A ~5400× improvement in worst-case read latency for ~70 % more erase time.**
For anything that must stay responsive while `blob_db_prepare()` runs, that is
the difference between usable and not.

**Zephyr cannot express it.** The flash API has no suspend/resume operation;
`drivers/flash/spi_nor.h` defines `SPI_NOR_FLSR_ERASE_SUSPEND` (bit 6) and
`SPI_NOR_FLSR_PROGRAM_SUSPEND` (bit 2) as status-register bits and nothing
uses them, and `nrf_qspi_nor.c` has no suspend path at all. Implementing this
means a driver extension plus either a flash-API addition or a
`flash_ex_op()` vendor operation — a real piece of work, and the payoff above
is why it might be worth it.

### 3b. Erase granularity — the driver already does 4 KB; the 64 KB choice is ours

The part supports three erase sizes. **The nRF QSPI peripheral can issue only
two of them**, plus chip erase — this is a hardware limit, not a driver choice.
`modules/hal/nordic/nrfx/hal/nrf_qspi.h:179` enumerates the `ERASE.LEN`
register field:

```c
typedef enum {
    NRF_QSPI_ERASE_LEN_4KB  = QSPI_ERASE_LEN_LEN_4KB,  /**< Erase 4 kB block (flash command 0x20). */
    NRF_QSPI_ERASE_LEN_64KB = QSPI_ERASE_LEN_LEN_64KB, /**< Erase 64 kB block (flash command 0xD8). */
    NRF_QSPI_ERASE_LEN_ALL  = QSPI_ERASE_LEN_LEN_All   /**< Erase all (flash command 0xC7). */
} nrf_qspi_erase_len_t;
```

| | opcode | reachable here | ULP typ | per byte | latency per call |
|---|---|---|---:|---:|---:|
| Sector erase 4 KB | `20h` | **yes** | 58 ms | 14.2 µs/B | **58 ms** |
| Block erase 32 KB | `52h` | **no — no `ERASE.LEN` encoding** | 1.0 s | 30.5 µs/B | 1.0 s |
| Block erase 64 KB | `D8h` | **yes** | 0.8 s | 12.2 µs/B | 800 ms |
| Chip erase | `C7h` | yes | 120 s | 14.3 µs/B | 120 s |

Reaching `52h` at all would mean going around the erase path entirely, through
the QSPI custom-instruction (CINSTR) interface. **It is not worth it**: in
Ultra Low Power a 32 KB erase is specified at 1.0 s against the 64 KB erase's
0.8 s, so the one granularity the peripheral cannot reach is also the one with
no reason to be used. The hardware limit costs nothing on this part.

**Of the two it can issue, the driver already picks whichever fits.**
`nrf_qspi_nor.c:604`:

```c
if (size == params->size) {                 /* chip erase           */
} else if ((size >= QSPI_BLOCK_SIZE) && QSPI_IS_BLOCK_ALIGNED(addr)) {
        res = nrfx_qspi_erase(NRF_QSPI_ERASE_LEN_64KB, addr);
} else if ((size >= QSPI_SECTOR_SIZE) && QSPI_IS_SECTOR_ALIGNED(addr)) {
        res = nrfx_qspi_erase(NRF_QSPI_ERASE_LEN_4KB, addr);
} else { /* minimal erase size is at least a sector size */ }
```

So `flash_area_erase(fa, off, 4096)` at a 4 KB-aligned offset issues a 4 KB
sector erase **today, with no driver change and no Kconfig change**. This
corrects the obvious reading of the situation: nothing is stopping a 58 ms
erase except that nobody asks for one.

What `CONFIG_NORDIC_QSPI_NOR_FLASH_LAYOUT_PAGE_SIZE` (default 65536,
`Kconfig.nordic_qspi_nor:27`) controls is only the layout the driver
*reports* — and `blob_db` derives its bucket size from exactly that, so the
64 KB granularity is inherited from a default, not imposed by the hardware.

The trade is **throughput against latency**, and they point opposite ways:
64 KB block erase is ~16 % more efficient per byte, while 4 KB sector erase is
**13.8× lower latency per operation**. For a store that erases one bucket at a
time — which is what `blob_db_prepare()` does — latency is what is felt. The
costs are real too: 16× more buckets, 16× the bucket-header overhead, and a
payload cap that falls with the sector size.

So the practical answer to "which erase sizes does the driver have" is
**two: 4 KiB and 64 KiB** (plus chip erase). 32 KiB is absent, and absent from
the silicon rather than the software — which on this part is no loss, since
32 KiB is the slower of the two block erases in Ultra Low Power.

### 3c. Chip erase is a trap, and `blob_db_erase_all()` may already be taking it

Chip erase is 120 s typ (ULP) for 8 MB against 128 × 0.8 = 102 s of block
erases — slower, not faster. Scaled by the 1.36× the measured block erase runs
above typ, ~163 s against ~139 s measured block-by-block.

That is not hypothetical here. The driver takes the chip-erase branch when
`size == params->size`, i.e. when the erase covers the whole *device*; the DK's
`storage_partition` **is** the whole 8 MB device, so `blob_db_erase_all()` over
the full partition lands on `nrfx_qspi_chip_erase()`. The sweep never reached
it — the erase span stops at 32 blocks — so this is a prediction, and a cheap
one to check: erase the whole partition and see whether it takes ~139 s or
~163 s.

Consistent with all of it, the sweep found per-block cost falling only 4.7 %
between a 1-block call (1111 ms) and a 32-block call (1059 ms), and that saving
comes from amortising the per-call overhead, not from a cheaper erase command.

### 3d. Where the 51.6 ms per-erase-call overhead partly goes

The fit found an erase intercept of ~51.6 ms — cost paid per call before any
block is erased. One named contributor is the completion poll,
`qspi_wait_while_writing()` (`nrf_qspi_nor.c:501`), which the erase loop calls
with `K_MSEC(10)`:

```c
do {
        k_sleep(poll_period);          /* 10 ms, BEFORE the first check */
        rc = qspi_rdsr(dev, 1);
} while ((rc >= 0) && ((rc & SPI_NOR_WIP_BIT) != 0U));
```

It sleeps *before* reading status, so completion is detected with 10 ms
granularity and the call returns on average ~5 ms after the part is actually
idle. On a 1086 ms erase that is under 1 %, but it is pure latency on a 58 ms
sector erase — **~9 %** — which matters if 3b is ever acted on. A shorter poll
period near the expected end would cut it, at the cost of more RDSR traffic.

---

## Summary — what is on the table

| | measured today | available | needs |
|---|---|---|---|
| unaligned read | +42–65 % on a 256 B read | align `blob_db`'s read offsets to 4 B | an L1 change; no driver work |
| power mode | Ultra Low Power | erase −40 %, program −73 % | 24-bit WRSR in `nrf_qspi_nor`, or a `mxicy,mx25r-power-mode` property on the `nordic,qspi-nor` binding |
| erase latency | blocks ~1086 ms | ~200 µs read latency during erase, for ~70 % longer erase | suspend/resume in the driver + a flash-API or ex-op path |
| erase granularity | 64 KB blocks | 58 ms per erase instead of 800 ms typ | **nothing in the driver** — it already picks 4 KB when asked. `blob_db` sized for a 4 KB bucket, via the reported layout |
| whole-partition erase | untested | avoid the chip-erase branch | `blob_db_erase_all()` on an 8 MB partition = the whole device, which the driver maps to chip erase (~163 s predicted) instead of 128 block erases (~139 s) |

None of these is a defect. They are choices that were made by default, and the
measurements are what makes them visible as choices.
