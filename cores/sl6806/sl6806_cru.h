/*
 * sl6806_cru.h - the SL6806 clock and reset unit at 0x40080000.
 *
 * =====================================================================
 *  THIS IS A MAP, NOT A DRIVER
 * =====================================================================
 * Nothing in the framework programs these registers. They are written down
 * because two separate pieces of the reverse engineering ran into them, and
 * because the peripheral map previously recorded 0x40080000 as the LCD
 * controller, which it is not. Getting that wrong sends anyone looking for
 * the display into the clock tree.
 *
 * WHY THIS IS THE CLOCK UNIT
 *
 *   - The HLKJ bootloader treats +0x40 and +0x48 as divider registers: it
 *     inserts a value into a bitfield, waits, and then polls bit 31 until it
 *     clears. That is the "update in progress" handshake every clock divider
 *     on every SoC has, and nothing else has.
 *   - Both the bootloader and the application bring a peripheral up by
 *     clearing bit 15 of +0x64 and +0x74, waiting, and setting them again -
 *     the standard gate-off / gate-on reset idiom.
 *   - The application's whole display path (0x00D3E1A4 .. 0x00D3F980) touches
 *     0x40080000 at exactly four offsets - 0x64, 0x74, 0x10C, 0x120 - and
 *     never reads or writes anything that could carry pixels. A controller
 *     you only ever clock-gate is not the controller you are looking for.
 *
 * The LCD controller is at 0x400D9000; see sl6806_lcdc.h.
 *
 * Provenance markers as elsewhere: [V] verified against the dump,
 * [I] inferred, [?] unknown.
 */
#ifndef SL6806_CRU_H
#define SL6806_CRU_H

#include <stdint.h>

/* [V] Base. Referenced from the bootloader and from the application. */
#define SL6806_CRU_BASE        0x40080000u

#define SL6806_CRU_REG(off)    (*(volatile uint32_t *)(SL6806_CRU_BASE + (off)))

/*
 * [V] Divider register A, 0x40080040.
 *
 * Three fields, each written by a small setter in the bootloader
 * (0x0082171A, 0x00821736, 0x00821752). Each setter does
 * read / clear field / insert value / write / delay 15 / poll bit 31.
 *
 * [I] The low two bits are read on their own by 0x0082076C, which maps
 * 0/1/2/3 to 10/2/15/42 - so they select something enumerated rather than a
 * divisor. Which clock, and what those four values mean, is not known.
 */
#define SL6806_CRU_DIV_A            0x40
#define SL6806_CRU_DIV_A_F0_SHIFT   6      /* [V] 6-bit field, mask 0xFC0     */
#define SL6806_CRU_DIV_A_F1_SHIFT   15     /* [V] 5-bit field, mask 0xF8000   */
#define SL6806_CRU_DIV_A_F2_SHIFT   23     /* [V] 5-bit field, mask 0xF800000 */
#define SL6806_CRU_BUSY             (1u << 31)  /* [V] set while updating */

/* [V] Divider register B, 0x40080048. One 6-bit field at bit 8, same
 * handshake (bootloader 0x0082176E). */
#define SL6806_CRU_DIV_B            0x48
#define SL6806_CRU_DIV_B_SHIFT      8      /* [V] mask 0x3F00 */

/*
 * [V] Module gates, 0x40080064 and 0x40080074, bit 15 in each.
 *
 * The bring-up sequence, identical in the bootloader (0x00827FAC) and in the
 * application's LCD path (0x00D3E8F4 / 0x00D3E92C):
 *
 *     clear bit 15 in both; wait ~100 ticks; set bit 15 in both
 *
 * Both users are the LCD controller, so whether these gates are per-module
 * or shared cannot be told from one peripheral alone. [?]
 */
#define SL6806_CRU_GATE0            0x64
#define SL6806_CRU_GATE1            0x74
#define SL6806_CRU_GATE_BIT         (1u << 15)   /* [V] */

/*
 * [V] Module clock select, 0x4008010C.
 *
 * Field 0xF10 is cleared and rewritten, then bit 0 is set. The application
 * writes 0x910 and the bootloader writes 0x310 for the same peripheral, so
 * the field is a source or divisor choice and the two disagree deliberately
 * - the bootloader runs the panel slower.
 */
#define SL6806_CRU_MODCLK           0x10C
#define SL6806_CRU_MODCLK_SEL_MASK  0xF10u   /* [V] */
#define SL6806_CRU_MODCLK_ENABLE    (1u << 0) /* [V] */

/* [V] Also written in the same paths, contents not decoded. [?] */
#define SL6806_CRU_UNKNOWN_120      0x120

/*
 * [V] 0x40080120 is also used by 0x00D3F07C, which takes two arguments and
 * inserts them at bits 4 and 8 before setting bit 0 - so a second
 * two-field-plus-enable register of the same shape as MODCLK.
 */

/*
 * WHAT THIS DOES NOT TELL YOU: the CPU frequency.
 *
 * F_CPU is still a guess (see README.md). None of the registers above is a
 * PLL multiplier, and the crystal frequency is not written down anywhere in
 * the flash image. Calibrating with examples/ClockCalibrate and a stopwatch
 * remains the practical answer; finding the PLL block - which is not at this
 * base - would be the exact one.
 */

#endif /* SL6806_CRU_H */
