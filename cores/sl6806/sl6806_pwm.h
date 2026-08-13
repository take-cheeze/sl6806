/*
 * sl6806_pwm.h - the SL6806 PWM at 0x40084000, and the panel backlight.
 *
 * =====================================================================
 *  THE PANEL LIGHTS. IT DOES NOT DIM.
 * =====================================================================
 * Verified on a P20 Player, 2026-08-07. sl6806_backlight_begin() turns the
 * backlight on, reliably, from cold or warm. What it does NOT do is vary the
 * brightness, and the reason matters:
 *
 * [M] The counter is not running. Duty 0 and duty 100 look identical, CTRL
 * bit 8 (the update/commit bit) never self-clears once set, and CTRL bit 28
 * is permanently set. All three say the same thing: bit 8 of the pair
 * register is driving the output pad to a static level, not producing a
 * waveform, and this is an on/off backlight until the counter's clock is
 * found.
 *
 * [!] CONSEQUENCE FOR ANYONE READING THE OLD NOTES: every sweep in §14a that
 * used CTRL bit 28 as its success test is void, including the walk over all
 * 128 module ids. They were asking a bit that never answers. A valid test is
 * whether any word in the block changes between two reads - a running counter
 * ticks, a stopped one does not.
 *
 * THE RECIPE, in order, because every step of it cost something:
 *
 *   1. sl6806_module_enable(SL6806_PWM_MODULE_ID) - module 68. The order of
 *      the two gate writes matters; see sl6806_module.h.
 *   2. Mux the pad: SL6806_PWM_BL_PAD, bank 1 pin 0 on function 4.
 *   3. CTRL <- 0x40, then OR in 0x3F.
 *   4. Period and duty: (period << 16) | duty, period 48000.
 *   5. MODE bit 0.
 *   6. THE PAIR CLOCK ENABLE - bit 8 of 0x40084010 + (ch >> 1) * 4. This is
 *      the one that was missing, and nothing in flash or in the SRAM blob
 *      ever writes it, so no amount of reading the vendor's code would have
 *      produced it. It was found by holding each of the register's 32
 *      reachable settings in turn and watching the panel.
 *
 * TWO THINGS THAT WILL MISLEAD YOU, both of which did:
 *
 *   CTRL bit 28 is NOT a busy or "counter stopped" flag. It is set while the
 *   backlight is running perfectly well. 0x00811E74 does spin on it before
 *   writing period and duty, so it means something - but using it as a
 *   success test produces confident, wrong negatives, and produced several.
 *
 *   The block reads as zeros before its module clock is enabled and silently
 *   drops writes, so a bare read cannot tell "gated" from "idle". Test with a
 *   write and a read back.
 *
 * Provenance: [V] verified against the dump, [M] measured on hardware,
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
 *
 * [M] SUPERSEDED 2026-08-13. 0x400E0000 is reachable now: after
 * sl6806_periph_group_begin() - the vendor's own sequence, module 46 and all
 * - it holds bits from a payload and the PLL locks (0x40080008 reads back
 * 0xD0010C04). examples/AudioWall walked all 32 of its bits. So "dead
 * forever" is retired; what is still true is that the PWM does not need it.
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
 * [M] 2026-08-13: performed, twice, and survivable - examples/BtProbe and
 * examples/AudioWall both run it and the console lives. It is no longer "the
 * last step nothing here performs", and it is no longer a candidate
 * explanation for the stalled PWM counter either: 0x400E0000 opens and the
 * counter is still not the thing this unlocked. §18 has a better lead - the
 * vendor's system clock init at 0x00806800, which a payload never runs.
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
 * [M] Bit 8 is the pair's CLOCK ENABLE, and without it the channel is
 * configured, gated, muxed and completely dead. Measured by holding each of
 * the 32 settings the register accepts: the panel lit at 0x100 and stayed lit
 * through 0x10F, so bits [3:0] - what 0x00811EC0 assembles as a source
 * select - make no difference to whether it runs.
 *
 * [M] The writable mask is 0x10F. Writing 0x3F0F reads back 0x10F, so the
 * "divider" implied by `div << 8` in 0x00811EC0 is a single bit, and that
 * bit is this enable.
 *
 * [V] CONFIRMED 2026-08-13, against docs/sl6806_re_notes.md §14a, which files
 * that same write under the channel's +0x00 control register instead. It is
 * this one. Every PWM accessor fetches its target from a cached pointer table
 * at SRAM 0x0082B3F8, and they index it differently: the update trigger takes
 * T[ch + 4], the src|div<<8 writer takes T[ch + 1]. Three slots apart, so
 * different registers - and with six channels and three pairs the layout is
 * T[0] global, T[1..3] the pair registers, T[4..9] the channel controls, so
 * the src|div<<8 caller is indexed by PAIR. §18 has the working; §14a's row
 * is struck through there.
 *
 * Nothing in flash or in the SRAM blob writes this register. It reads 0 on a
 * cold chip and stays 0, which is why reading the vendor's code was never
 * going to yield it.
 */
#define SL6806_PWM_PAIR(ch)     (SL6806_PWM_BASE + 0x10u + ((ch) >> 1) * 4u)
#define SL6806_PWM_PAIR_CLK_EN  (1u << 8)   /* [M] the missing bit */

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

/* ------------------------------------------------------------------ */
/* The backlight                                                       */
/* ------------------------------------------------------------------ */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Bring the backlight up and set it to `percent`. Does the whole recipe at
 * the top of this file. Returns 1 on success, 0 if the PWM's module clock
 * refused to come up.
 */
int sl6806_backlight_begin(unsigned percent);

/*
 * Set brightness, 0..100. Clamped exactly as 0x00D102F4 clamps it, writes the
 * duty the vendor would write, and pulses the commit bit.
 *
 * [M] AND HAS NO VISIBLE EFFECT YET, because the counter does not run - see
 * the top of this file. It is kept because the register values are right and
 * because the day the counter starts, this is already correct.
 */
void sl6806_backlight_set(unsigned percent);

/*
 * Is the block's counter actually running? Reads the channel block twice and
 * reports whether anything ticked.
 *
 * This exists because the obvious status bits lie: CTRL bit 28 looks like a
 * busy flag and is permanently set, CTRL bit 8 looks like a commit and never
 * clears. Several confident negatives in the notes came from trusting them.
 * A counter that runs changes something; one that does not, does not.
 */
int sl6806_pwm_counter_ticking(unsigned ch);

#ifdef __cplusplus
}
#endif

#endif /* SL6806_PWM_H */
