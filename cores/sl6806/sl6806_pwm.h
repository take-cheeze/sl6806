/*
 * sl6806_pwm.h - the SL6806 PWM block at 0x40084000, and the backlight.
 *
 * =====================================================================
 *  NOTHING HERE HAS BEEN SEEN TO WORK
 * =====================================================================
 * Every address and bit below was read out of the stock firmware and none of
 * it has been confirmed against the hardware. The block reads as zeros in
 * bootloader mode - but so does unmapped space, so that measurement cannot
 * tell a clock-gated peripheral from an imaginary one. Treat a dark panel
 * after using this as "no information", not as "the map is wrong".
 *
 * WHY THIS BLOCK
 *
 * docs/sl6806_re_notes.md 7b used to say the PWM registers were not
 * recoverable, because the device-name strings had no code referencing them
 * and the driver was SRAM-resident. Both halves of that turned out to be
 * wrong: the SRAM-resident code is in the flash image after all (13), and the
 * block's base appears exactly once, in 0x00D99C34:
 *
 *     r0 = 0x40084020 + (ch << 5)          ; the channel's registers
 *     [0x0082B3F8 + (ch + 4) * 4] = r0     ; cached in a table
 *     [0x0082B3F8] = 0x40084000
 *
 * Everything afterwards reaches a channel through that table, which is why no
 * literal scan ever found it. The accessors that use the table are at
 * 0x00811E48..0x00811EC0 and are where the bit assignments below come from.
 *
 * THE BACKLIGHT IS CHANNEL 3, and the numbers are the vendor's own.
 * 0x00D102F4 clamps a percentage to 100 and asks for period 48000, duty
 * percent * 480 - so 100% is duty == period. 0x00D10354 opens the channel
 * with 48000/24000 and then immediately calls set_brightness(60), which is
 * where the "default is 60%" in the notes comes from.
 *
 * WHAT IS STILL UNKNOWN. Which pad the channel comes out on. The driver's
 * device record carries 0x00030000, which decodes as bank 3 pin 0, and 7g
 * independently lists that pad as an output driven high - but the record's
 * field order has not been read, so that is a guess. If the block turns out
 * to be right and the panel still does not light, the pad mux is the next
 * suspect, not this file.
 *
 * Provenance markers as elsewhere: [V] verified against the dump,
 * [I] inferred, [?] unknown.
 */
#ifndef SL6806_PWM_H
#define SL6806_PWM_H

#include <stdint.h>
#include "sl6806_mmio.h"
#include "sl6806_module.h"

/* ------------------------------------------------------------------ */
/* Module gating                                                       */
/* ------------------------------------------------------------------ */

/*
 * [V] A module enable register, one bit per module. 0x00D9A734(n) sets bit n
 * and then delays 10 ms; 0x00D9A74C(n) clears it. 0x00D9A768(n) pulses bit n
 * of +0x08 low for 10 ms as a reset.
 *
 * ---------------------------------------------------------------------
 * MEASURED: THIS WHOLE BLOCK IS DEAD UNTIL THE PLL IS UP.
 *
 * 0x400E0000 reads as zero and ignores writes - from a payload (the run of
 * examples/Backlight on 2026-08-07) and from the host over the vendor read
 * command in bootloader mode, which rules out anything payload-specific.
 * 0x400E2000 is the same. Meanwhile the CRU at 0x40080000 and the LCDC at
 * 0x400D9000 both read live values in exactly the same conditions, so MMIO
 * itself is fine and the region is simply unclocked.
 *
 * The firmware agrees: 0x00D9A7FC brings the PLL up and only then makes the
 * first call into 0x400E0000. See SL6806_PLL_* below.
 * ---------------------------------------------------------------------
 */
#define SL6806_MODCTL_BASE      0x400E0000u
#define SL6806_MODCTL_ENABLE    0x00       /* [V] 1 = module clocked        */
#define SL6806_MODCTL_RESET     0x08       /* [V] 0 = held in reset         */

/*
 * [?] WHICH BIT OF 0x400E0000 THE PWM IS, is still not known, and no longer
 * matters: that register takes no writes from a payload at all, and the PWM
 * turns out not to need it.
 *
 * (It used to say bit 2, "[V] from the teardown path". That was wrong - the
 * teardown it was read from belongs to the module ending at 0x00D99C0C, not
 * to the PWM channel init that starts 0x20 later.)
 */

/*
 * [V, MEASURED ON HARDWARE] What the PWM actually needs is a CRU gate:
 * bit 4 of 0x40080068 and 0x40080078.
 *
 * Found by sweeping (examples/Backlight, 2026-08-07). With that bit set the
 * block answers a write and reads it back - CTRL took 0x7F and read
 * 0x1000007F, and period/duty took (48000 << 16) | 28800 exactly. Before it,
 * every register reads zero and ignores writes.
 *
 * Bit 28 of CTRL appearing on its own is the busy flag that 0x00811E74 polls.
 */
/*
 * [M] That gate is module id 68 under the mask ROM's scheme - ids 64..95 use
 * CRU +0x68 as the gate and +0x78 as the shadow, and bit 4 is 68 - 64. It was
 * found by a sweep that wrote both registers at once, which happens to be
 * survivable for this one peripheral and is not in general: see
 * sl6806_module.h. Use sl6806_module_enable(68) rather than poking the bits.
 */
#define SL6806_PWM_MODULE_ID    68

/*
 * [V] The pad. The PWM's configure op (0x00D45394) calls ROM 0x93C - pad
 * configure from a packed id - with the id its constructor stored at
 * dev+0x48, and that id is 0x00010200: bank 1, pin 0, alternate function 4.
 *
 * This is the part examples/BacklightHunt could never have found. It drove
 * pads as function 1, plain output, and muxing a pin to function 4 is a
 * different thing entirely; its candidate list also skipped bank 1's low
 * pins as "the panel's own bus", which pins 1-8 are - but pin 0 is not.
 */
#define SL6806_PWM_BL_PAD       0x00010200u

/* ------------------------------------------------------------------ */
/* The PLL, which everything above depends on                          */
/* ------------------------------------------------------------------ */

/*
 * [V] From 0x00D9A7FC, which is the first thing the vendor's module bring-up
 * does. Write CONFIG, spin until LOCK, then set bit 16.
 *
 * This also answers 12.6, which recorded that the PLL had not been found and
 * "is not at the CRU base: 0x40080000 has dividers but no multiplier". It is
 * at the CRU base, at +0x08. In bootloader mode it reads 0x00000801 with the
 * lock bit clear, i.e. stopped.
 */
#define SL6806_PLL_CTRL         0x40080008u
#define SL6806_PLL_CONFIG       0xC0000C04u /* [V] the vendor's word         */
#define SL6806_PLL_LOCK         (1u << 28)  /* [V] polled until set          */
#define SL6806_PLL_OUT_ENABLE   (1u << 16)  /* [V] set after lock            */

/*
 * [V] Written 0x31 right after the PLL locks and before the first module is
 * enabled (0x00D9A82A), and 0x30 on the way down (0x00D9A89A). One bit apart,
 * so **bit 0 is an enable** and 0x30 is a field that stays put - this is a
 * clock enable, not the clock-source reparent it was first taken for. That
 * matters, because a reparent could have taken the core or USB with it and a
 * plain enable cannot.
 *
 * It is the last step of the vendor's PLL sequence that nothing here performs,
 * and it is the leading explanation for two things at once: a PWM whose
 * registers answer but whose counter never runs, and a 0x400E0000 that reads
 * zero forever.
 */
#define SL6806_CRU_CLK_ENABLE   0x4008011Cu
#define SL6806_CRU_CLK_ON       0x31u
#define SL6806_CRU_CLK_OFF      0x30u

/* ------------------------------------------------------------------ */
/* The block                                                           */
/* ------------------------------------------------------------------ */

#define SL6806_PWM_BASE         0x40084000u
#define SL6806_PWM_CHANNELS     6          /* [V] the driver loops 0..5     */

/* [V] Written to the table as entry 0. Role unknown - nothing in the paths
 * read so far touches it after init. */
#define SL6806_PWM_GLOBAL       (SL6806_PWM_BASE + 0x00u)

/*
 * [V] One register per channel *pair*, 0x40084010 + (ch >> 1) * 4.
 *
 * 0x00811EC0 writes it as `src | (div << 8)`, reached through the thunk at
 * 0x00811D50 which indexes the pair table rather than the channel table. That
 * is the shape of a counter clock select, and it is the one register on this
 * block that nothing in flash or in the SRAM blob ever writes - it reads 0 on
 * a cold chip and stays 0.
 *
 * [M] SWEPT, AND IT IS NOT THE ANSWER. Sixteen sources against six dividers,
 * 2026-08-07: the counter stayed stopped throughout.
 *
 * [M] The sweep did measure the register's shape, which is worth keeping. It
 * only retains `0x10F`: writing `0x3F0F` reads back `0x10F`. So `src` is
 * [3:0] and the divider is a single bit at [8] - not the eight-bit field the
 * `div << 8` in 0x00811EC0 suggested.
 */
#define SL6806_PWM_PAIR(ch)     (SL6806_PWM_BASE + 0x10u + ((ch) >> 1) * 4u)
#define SL6806_PWM_PAIR_VALUE(src, div) ((uint32_t)(src) | ((uint32_t)(div) << 8))

/* [V] Per-channel register block, 0x20 apart starting at +0x20. */
#define SL6806_PWM_CHAN(ch)     (SL6806_PWM_BASE + 0x20u + (uint32_t)(ch) * 0x20u)

/*
 * Channel registers, as offsets from SL6806_PWM_CHAN(ch).
 */
#define SL6806_PWM_CTRL         0x00
#define SL6806_PWM_PERIOD_DUTY  0x04       /* [V] (period << 16) | duty     */
#define SL6806_PWM_MODE         0x10
#define SL6806_PWM_REG14        0x14       /* [V] (a << 16) | b   [?] role  */
#define SL6806_PWM_REG18        0x18       /* [V] a plain word    [?] role  */
#define SL6806_PWM_REG1C        0x1C       /* [V] (a << 16) | b   [?] role  */

/* CTRL bits. */
#define SL6806_PWM_CTRL_RUN     (1u << 4)  /* [V] 0x00811CE4 sets/clears it */
#define SL6806_PWM_CTRL_UPDATE  (1u << 8)  /* [V] 0x00811D3C sets it,
                                            *     0x00811D4C polls it clear */
#define SL6806_PWM_CTRL_BUSY    (1u << 28) /* [V] 0x00811D2C polls it clear */
#define SL6806_PWM_CTRL_INIT    0x40u      /* [V] written bare at init      */

/* MODE bits, from 0x00811E9A and 0x00811CF4. */
#define SL6806_PWM_MODE_ENABLE  (1u << 0)  /* [V] toggled on its own        */
#define SL6806_PWM_MODE_SHIFT   1          /* [V] a 3-bit field at [3:1]    */

/*
 * [V] The control word the stock firmware builds for the backlight.
 *
 * 0x00D99C34 assembles it from four config bytes as
 *   ((c0 & 1) << 4) | ((c1 & 1) << 7) | (c3 & 0x0F) | ((c2 & 1) << 5)
 * and the backlight's open passes c0=3, c1=0, c2=1, c3=0x0F, giving 0x3F,
 * which is OR'd on top of the bare 0x40. So CTRL ends up 0x7F: RUN set, the
 * low nibble all ones, bit 5 and bit 6 set.
 */
#define SL6806_PWM_CTRL_BACKLIGHT  (SL6806_PWM_CTRL_INIT | 0x3Fu)

/* [V] The vendor's period, and its duty for a given percentage. */
#define SL6806_PWM_BL_PERIOD    48000u
#define SL6806_PWM_BL_DUTY(pct) ((uint32_t)(pct) * 480u)

/* ------------------------------------------------------------------ */
/* The little that can be called a driver                              */
/* ------------------------------------------------------------------ */

/*
 * The old ungate helper that lived here is gone. It poked 0x400E0000, which a
 * payload cannot write at all, and took a bit rather than a module id.
 * Use sl6806_module_enable(SL6806_PWM_MODULE_ID) from sl6806_module.h - the
 * mask ROM's mechanism, and the one that works.
 */

/*
 * Set period and duty. The vendor's setter (0x00811D04) returns without
 * writing anything when duty > period, so this does too - a silent clamp
 * would hide a caller's arithmetic bug behind a screen that is merely dim.
 * Returns 0 if it wrote, -1 if it refused.
 */
static inline int sl6806_pwm_set(unsigned ch, uint16_t period, uint16_t duty)
{
    uint32_t base;

    if (ch >= SL6806_PWM_CHANNELS || duty > period)
        return -1;

    base = SL6806_PWM_CHAN(ch);
    sl6806_mmio_write(base + SL6806_PWM_PERIOD_DUTY,
                      ((uint32_t)period << 16) | duty);
    return 0;
}

/* CTRL bit 4. */
static inline void sl6806_pwm_run(unsigned ch, int on)
{
    uint32_t base = SL6806_PWM_CHAN(ch);
    uint32_t r = sl6806_mmio_read(base + SL6806_PWM_CTRL);

    sl6806_mmio_write(base + SL6806_PWM_CTRL,
                      on ? (r | SL6806_PWM_CTRL_RUN)
                         : (r & ~SL6806_PWM_CTRL_RUN));
}

/* MODE bit 0. */
static inline void sl6806_pwm_enable(unsigned ch, int on)
{
    uint32_t base = SL6806_PWM_CHAN(ch);
    uint32_t r = sl6806_mmio_read(base + SL6806_PWM_MODE);

    sl6806_mmio_write(base + SL6806_PWM_MODE,
                      on ? (r | SL6806_PWM_MODE_ENABLE)
                         : (r & ~SL6806_PWM_MODE_ENABLE));
}

#endif /* SL6806_PWM_H */
