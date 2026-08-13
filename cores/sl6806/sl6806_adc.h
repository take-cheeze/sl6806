/*
 * sl6806_adc.h - the SL6806 ADC at 0x40096000, and the keys hanging off it.
 *
 * =====================================================================
 *  THIS ONE WORKS ON HARDWARE
 * =====================================================================
 * Unlike most of the maps in this tree, every address and bit below has been
 * exercised on a real P20 Player and produces sensible conversions. The two
 * volume buttons read reliably through it. Measured 2026-08-07.
 *
 * WHERE IT CAME FROM. The base is the only MMIO literal in the vendor's ADC
 * HAL (0x00D994F8). The per-channel stride and the init sequence are
 * transcriptions of 0x00D993A0 and 0x00D994EC. The channel enables are
 * 0x00D994BC and 0x00D9948C.
 *
 * THE PART THAT COST FOUR RUNS. The block reads structured values while it is
 * gated off and silently ignores writes, so a bare read cannot tell "working"
 * from "dead". It needs its module clock enabled first, and the only way that
 * works is the mask ROM's order (see sl6806_module_enable below): shadow
 * register, then gate register, then poll the gate until the bit reads back.
 * Writing both at once and moving on leaves the block exactly as dead as
 * writing neither, which is how an earlier sweep concluded the right bit was
 * the wrong one.
 *
 * AND THE OTHER TRAP: sl6806_adc_init() switches every channel off - it
 * zeroes the low bits of CTRL and all of +0x0C - so a channel has to be
 * enabled *after* init, not before. Miss that and the block sits perfectly
 * configured and perfectly idle.
 *
 * Provenance markers as elsewhere: [V] verified against the dump,
 * [M] measured on hardware, [I] inferred, [?] unknown.
 */
#ifndef SL6806_ADC_H
#define SL6806_ADC_H

#include <stdint.h>
#include "sl6806_mmio.h"
#include "sl6806_module.h"

#ifdef __cplusplus
extern "C" {
#endif

/* [M] The ADC's module id, found by walking 0..127. */
#define SL6806_ADC_MODULE_ID    84

/* ------------------------------------------------------------------ */
/* The block                                                           */
/* ------------------------------------------------------------------ */

#define SL6806_ADC_BASE         0x40096000u
#define SL6806_ADC_CHANNELS     10          /* [V] 0x00D993A0 dispatches ten */

#define SL6806_ADC_CTRL         0x00        /* [V] init writes 0x80180000;
                                             *     low bits are per-channel
                                             *     enables (0x00D994BC)      */
#define SL6806_ADC_CFG          0x04        /* [V] init writes 0x0002A800    */
#define SL6806_ADC_IRQ          0x0C        /* [V] per-channel (0x00D9948C)  */
#define SL6806_ADC_CFG10        0x10        /* [V] 4 bits per channel, 0..7  */
#define SL6806_ADC_CFG18        0x18        /* [V] 4 bits per channel, 8..9  */

/* [V] Per-channel block, 0x10 apart from +0x20. */
#define SL6806_ADC_CHAN(ch)     (0x20u + (uint32_t)(ch) * 0x10u)
/* [M] The conversion result is +0x04 inside that block. */
#define SL6806_ADC_RESULT(ch)   (SL6806_ADC_CHAN(ch) + 0x04u)

#define SL6806_ADC_INIT_CTRL    0x80180000u /* [V] */
#define SL6806_ADC_INIT_CFG     0x0002A800u /* [V] */

/* Bring the block up. Enables the module clock, then runs 0x00D994EC's
 * sequence. Returns 1 if the block accepts writes afterwards. Does NOT enable
 * any channel - see the header comment. */
int sl6806_adc_begin(void);

/* Enable or disable one channel. Must come after sl6806_adc_begin(). */
void sl6806_adc_channel(unsigned ch, int on);

/* One conversion result. No conversion is started explicitly: with the
 * channel enabled the block free-runs and this reads the latest value.
 * [M] It jitters about 5 LSB. */
uint32_t sl6806_adc_read(unsigned ch);

/* Does the block accept writes? Write-then-read-back, because a gated block
 * reads plausible values and a bare read proves nothing. */
int sl6806_adc_writable(void);

#ifdef __cplusplus
}
#endif

#endif /* SL6806_ADC_H */
