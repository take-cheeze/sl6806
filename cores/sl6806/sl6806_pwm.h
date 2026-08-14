/*
 * sl6806_pwm.h - the SL6806 PWM at 0x40084000, and the panel backlight.
 *
 * =====================================================================
 *  [M] THE COUNTER RUNS AT EXACTLY 24.000 MHz. ROMCLK 39 IS THE CLOCK.
 * =====================================================================
 * Measured 2026-08-14 by examples/PwmSlow, by timing how long the update bit
 * takes to retire - which needs the counter to reach a period boundary, so
 * the interval IS one period:
 *
 *     period 48000 -> 2002 us      period 12000 ->  502 us
 *     period 24000 -> 1002 us      period  6000 ->  252 us
 *
 *     t = period / 24 MHz + 2 us,  ZERO residual on all four rows
 *
 * The 2 us is the probe's own read loop. Halving the period halves the time,
 * exactly, four times over - so this is a counter and its clock is 24.000 MHz.
 *
 * [M] And bit 4 of 0x400800F4 selects a SECOND SOURCE near 25 kHz, not a dead
 * one: its retire times scale with period too, they are just a thousand times
 * longer. At period 48000 that is a half-hertz output, which is the "blinking
 * backlight" that gave the whole thing away.
 *
 * [V] The vendor's period of 48000 against 24 MHz gives a 500 Hz output, which
 * is what a backlight PWM is supposed to be. That the decode lands on a
 * textbook number is a good independent check on all of it.
 *
 * [!] THREE ROUNDS OF RETRACTION GOT HERE, and the reason they were needed is
 * worth more than the result. Between them, four probes reported clean
 * negatives that were all one instrument reporting its own limit:
 *
 *   - CTRL read inside the clock's settling window: 168 identical rows.
 *   - sl6806_pwm_commit()'s 1000-iteration bound - tens of microseconds -
 *     against a period of 2 ms at best: "commits 0/4" everywhere, in every
 *     configuration, for months of bench time.
 *   - retire timed across loop() calls in RUN_MODE=poll: every answer an
 *     integer multiple of the host's 115 ms poll interval.
 *   - a blinking panel read as a dimming one.
 *
 * The tell was the same every time: AN OBSERVABLE THAT DOES NOT VARY ACROSS A
 * SWEEP IS USUALLY A BROKEN INSTRUMENT, NOT A DISCOVERY. Read a constant
 * column as a fault until proved otherwise, and print raw registers rather
 * than verdicts derived from them - that is what caught the one case that was
 * caught.
 *
 * ---------------------------------------------------------------------
 * The measurements below are still correct as measurements; what changed is
 * that "commits 0/4" no longer means "stopped", it means "slower than the
 * deadline that was used". The panel column stands.
 *
 *      0x400800F4  bit0  bit4   pair bit8   CTRL b28   commits   PANEL
 *   A   0x00000000   0     0        1           1        0/4     bright
 *   B   0x00000001   1     0        0           0        0/4     DARK
 *   C   0x00000001   1     0        1           0        0/4     bright
 *   D   0x00000011   1     1        0           1        0/4     DARK
 *   E   0x00000011   1     1        1           1        0/4     bright
 *
 *   [!] the `commits 0/4` column was the deadline, not the counter
 *   [!] "the panel tracks PAIR BIT 8 exactly and only" is ALSO retracted -
 *       every row above was watched at 500 Hz or at DC, and the panel turns
 *       out to follow the counter perfectly well when the waveform is slow
 *       enough to see. See the duty result below.
 *
 * [M] DUTY WORKS, AND IT IS ACTIVE-HIGH. examples/PwmFreq phase A, at 0.5 Hz
 * with pair bit 8 CLEAR: duty 10% gives a short flash, 50% an even on/off,
 * 90% a long flash. So the counter drives the pad without bit 8, the duty
 * field modulates the pulse width, and more duty is more on-time. With the
 * 24.000 MHz clock measurement that makes the PWM functionally complete, and
 * it means sl6806_backlight_set() has been writing correct values all along.
 *
 * [M] AND PAIR BIT 8 IS AN OUTPUT FORCE, confirmed behaviourally by
 * examples/PwmManual: setting it STOPS the blinking. It pins the pad
 * regardless of what the counter is doing, which is why every table in this
 * file that had bit 8 set showed a steady panel and why it looked for so long
 * like the only thing that mattered. Duty works whenever it is clear, and
 * duty == period stops the blink too, so the duty field reaches full scale.
 *
 * SO THE PICTURE IS COMPLETE AND SELF-CONSISTENT:
 *
 *   module 68           the registers answer
 *   romclk 39           the counter runs, at exactly 24.000 MHz
 *   pad b1p0 func 4     the counter reaches the pad
 *   CTRL 0x7F, MODE 1   the channel runs
 *   period / duty       active-high, full scale at duty == period
 *   pair bit 8          an OUTPUT FORCE that overrides all of the above
 *
 * [?] ONE NUMBER IS STILL MISSING, and it is the only thing the driver waits
 * on: the highest frequency at which flipping duty 0% against 100% still
 * changes the panel. Above ~60 Hz, dimming is usable and
 * sl6806_backlight_begin() should leave bit 8 CLEAR so duty has an effect.
 * At or below ~50 Hz only, every frequency where duty works is one where the
 * panel visibly flickers, so bit 8 stays and this is an on/off backlight.
 * examples/PwmManual walks it: `]` steps the ladder, `0` and `4` are the flip.
 *
 * `commits` is sl6806_pwm_commit(), whose poll is bounded at 1000 iterations.
 * That is tens of microseconds and one period is 2 ms at best, so it timed out
 * in every row of every configuration. It says nothing. The interrupt status
 * at +0x04 stayed at zero with the enable confirmed armed (`irqen 0808`),
 * which is the one part of this table still unexplained.
 *
 * CTRL BIT 28 IS STILL NOT "THE COUNTER IS RUNNING". It responds to romclk 39 -
 * it is the one thing that does - so it means something like "the block has a
 * clock" or "the register interface is ready". It is not evidence of a
 * waveform, and this file has now twice built a conclusion on it.
 *
 * AND PAIR BIT 8 IS THE BACKLIGHT. Set: the panel lights. Clear: dark, in
 * every configuration, clocked or not. sl6806_backlight_begin() writes it,
 * the write was removed for a few hours on the strength of the retracted
 * claim above, and removing it turns the backlight off. Do not remove it.
 *
 * WHERE THAT LEAVES THINGS:
 *
 *   [V][M] romclk 39 is the PWM's clock, read out of 0x00D99C34 and now
 *   measured at 24.000 MHz. Bit 4 selects a second source near 25 kHz.
 *
 *   [M] Fully measured and not in doubt: 0x400800F4 implements bits 0 and 4
 *   and nothing else (a 31-bit walk); MODE[3:1] and CTRL[19:16] are fully
 *   writable; the pair register's mask is 0x1FF, with [7:0] never written by
 *   anything and 0x00811EC0's `src` a byte-shaped fit for it.
 *
 *   [M] §14a's walk over 128 module ids used bit 28 and got "no module id
 *   moves it", which is true and is not the same question as what clocks the
 *   counter - romclk 39 answers that.
 *
 *   [!] examples/PwmMode's 168-row negative over MODE[3:1] and CTRL[19:16] is
 *   VOID: it scored on the same too-short commit deadline. Re-run it with the
 *   two-tier timer before treating that space as covered.
 *
 * THE ONE THING LEFT that matters to the driver: with the counter genuinely
 * running at 500 Hz, does the PANEL follow the duty, and is the pair
 * register's bit 8 still required? PwmClock's row B - fast clock, bit 8 clear
 * - was dark, but PwmSrc left bit 8 clear on the slow source and the panel
 * BLINKED, so the counter can reach the pad without it. Both cannot be the
 * whole story. examples/PwmDim settles it with one variable and a slow ramp,
 * which is what should have been done about four retractions ago.
 *
 * =====================================================================
 *  [V] THE COUNTER HAS A CLOCK, AND IT IS ROMCLK 39
 * =====================================================================
 * Static, 2026-08-14, from the XIP half of FIRM. NOT YET RUN ON HARDWARE.
 *
 * 0x00D99C34 - the channel init, and the only caller of it in the whole
 * image is the driver's configure op at 0x00D45394 - opens with three clock
 * steps, not one:
 *
 *     if (!module_is_enabled(68)) module_enable(68);   // ROM 0x1E54/0x1EC0
 *     clk_setsrc(39, use_high ? 16 : 10);              // 0x008050F2, ROM 0x2E5C
 *     romclk_enable(39);                               // 0x008051EC, ROM 0x20EC
 *
 * Both of the last two land on ONE register, 0x400800F4:
 *
 *   - ROM 0x2E5C dispatches id 39 (tbh index 39-3) to ROM 0x3174, whose
 *     entire body is: src 16 -> 0x400800F4 |= 0x10; src 59 -> &= ~0x10;
 *     anything else, 10 included, falls through to a bare `bx lr`.
 *   - ROM 0x20EC dispatches id 39 to ROM 0x2338: 0x400800F4 |= 0x01. That
 *     agrees with sl6806_romclk.h's generated entry {39, 0x400800F4, 0x1},
 *     which is a second, independent derivation of the same register.
 *
 * Clock id 39 appears NOWHERE else in the image. It is the PWM's alone.
 *
 * Nothing in this framework had ever written 0x400800F4. sl6806_pwm.c now
 * does, and if the counter starts this is why.
 *
 * [!] §18 said "the PWM does not ask for a clock of its own". It scanned the
 * SRAM blob and stage 1 for calls to the clock veneers. These two calls are
 * in XIP, outside that scan. The blob half of §18 is right - 0x00811CE4 to
 * 0x00811EC0 really has no clock call - but the conclusion drawn from it is
 * not, and it sent the search to 0x00806800 for nothing.
 *
 * [V] The backlight passes use_high = 0 (cfg[4] of the open's config block at
 * 0x00D10354 is zero), so the stock firmware takes the src-10 branch, which
 * is the no-op. It relies on bit 0 alone. Bit 4 is the thing to try second,
 * not first.
 *
 * THE RECIPE, in order, because every step of it cost something:
 *
 *   1. sl6806_module_enable(SL6806_PWM_MODULE_ID) - module 68. The order of
 *      the two gate writes matters; see sl6806_module.h.
 *   2. [M] The functional clock: 0x400800F4 |= 1, romclk 39, bit 4 left
 *      CLEAR. The vendor's second step. Necessary-looking, not sufficient.
 *   3. Mux the pad: SL6806_PWM_BL_PAD, bank 1 pin 0 on function 4.
 *   4. CTRL <- 0x40, then OR in 0x3F.
 *   5. Period and duty: (period << 16) | duty, period 48000.
 *   6. MODE bit 0.
 *   7. [M] PAIR BIT 8 - and this is the step that lights the panel. Nothing
 *      in the vendor's image writes this register, which is exactly why it
 *      looks removable. It is not. Removing it makes the backlight dark.
 *
 * WHAT WILL STILL MISLEAD YOU:
 *
 *   The block reads as zeros before its module clock is enabled and silently
 *   drops writes, so a bare read cannot tell "gated" from "idle". Test with a
 *   write and a read back.
 *
 *   [M] The interrupt status at +0x04 does NOT latch while the enable at
 *   +0x00 is clear - it read 0 through both running configurations. Arm it
 *   before you poll it. See SL6806_PWM_IRQ_STATUS.
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
 * [V] The functional clock, which is a SECOND and separate thing from the
 * module gate above. Module 68 makes the registers writable; this is what
 * clocks the counter behind them, and the two have always been confused here
 * because only one of them had been found.
 *
 * 0x00D99C34 enables it through the mask ROM's romclk family, id 39. The
 * whole of that family's handler for 39 is one OR, so it is transcribed here
 * rather than called - the same reasoning sl6806_romclk.h gives, and the
 * same shape sl6806_audio.c uses for id 19.
 *
 * [M] 2026-08-14: bit 0 is the whole answer. With it set and bit 4 clear the
 * counter runs; with it clear the counter is stopped no matter what else is
 * configured. examples/PwmClock, configurations A and B.
 *
 * [M] SRC_HIGH - the vendor's `clk_setsrc(39, 16)` branch - STOPS THE
 * COUNTER. Configurations D and E have bit 0 set as well and are stopped
 * anyway. So bit 4 reparents the block to a source that is not running in
 * bootloader mode - the PLL is the obvious candidate and has not been checked
 * - and the stock backlight passing use_high = 0 is not an accident. Do not
 * set this bit unless you have started whatever it selects;
 * sl6806_backlight_begin() never does.
 *
 * [!] An earlier reading of this also blamed bit 4 for CTRL coming back
 * 0x1000017F, the update bit latched and unable to retire. It is not bit 4's:
 * a second run showed the same 0x17F under configuration A, with bit 4 clear
 * and the clock off entirely, and showed D both with and without it. What is
 * true across every row is narrower - the update bit is only ever stuck while
 * the counter is stopped, never while it runs - and why it appears in some
 * stopped snapshots and not others is [?].
 */
#define SL6806_PWM_ROMCLK_ID    39
#define SL6806_PWM_CLK_REG      0x400800F4u
#define SL6806_PWM_CLK_ENABLE   (1u << 0)   /* [M] the counter's clock       */
#define SL6806_PWM_CLK_SRC_HIGH (1u << 4)   /* [M] a DEAD source - see above */

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

/*
 * [V] The block's two interrupt registers, decoded 2026-08-14 from the ISR.
 *
 * 0x00811D60 is the enable writer - it ORs 1 << ch into +0x00 and, the first
 * time that register goes non-zero, installs 0x0080EE45 on IRQ 73 and enables
 * it; when it goes back to zero it disables the IRQ again. The handler at
 * 0x0080EE44 is four lines:
 *
 *     pend = REG(+0x04) & REG(+0x00);
 *     if (pend & 0x3F3F) { REG(+0x04) = pend; if (cb) cb(pend); }
 *
 * So +0x00 is a per-channel enable mask, +0x04 is a write-1-to-clear status,
 * and 0x3F3F is two banks of six: six channels x two event sources.
 *
 * [!] Nothing in this build calls 0x00811D60, so the stock firmware never
 * enables the PWM interrupt. The callback slot it feeds (T[10], SRAM
 * 0x0082B420) is what /dev/pwm_ch3's `pwm1_event_callback` would have hung
 * off.
 *
 * [M] AND +0x04 DOES NOT LATCH WHILE +0x00 IS CLEAR. It read 0 through both
 * of the configurations whose counter was demonstrably running, so the guess
 * that a raw status register latches regardless of masking is wrong for this
 * block. sl6806_pwm_irq_arm() sets the channel's enable bit first; without
 * that, sl6806_pwm_irq_fired() reports nothing and means nothing.
 *
 * Arming only writes this register - it does not touch the NVIC - so no
 * handler is needed and nothing can be taken off the bus by it.
 */
#define SL6806_PWM_IRQ_ENABLE   (SL6806_PWM_BASE + 0x00u)
#define SL6806_PWM_IRQ_STATUS   (SL6806_PWM_BASE + 0x04u)
#define SL6806_PWM_IRQ_MASK     0x3F3Fu     /* [V] what the ISR tests        */
#define SL6806_PWM_IRQ_NUM      73          /* [V] 0x00811D60's vector       */

/*
 * Both of a channel's event bits. 0x3F3F is two banks of six, so a channel
 * owns bit `ch` in the low bank and bit `ch + 8` in the high one.
 *
 * [M] The first probe armed only the low bit and read nothing. That is not
 * yet evidence either way - it may be the wrong event, or the status may not
 * latch at all - so arm both and let the register say which.
 */
#define SL6806_PWM_IRQ_BITS(ch) ((1u << (ch)) | (1u << ((ch) + 8)))

/* [V] The name this used to have. It is the interrupt enable. */
#define SL6806_PWM_GLOBAL       SL6806_PWM_IRQ_ENABLE

/*
 * [V] One register per channel *pair*, 0x40084010 + (ch >> 1) * 4.
 *
 * [M] BIT 8 IS THE BACKLIGHT, and it is the only thing that is. Across the
 * five configurations the panel tracked this bit and nothing else: set in A,
 * C and E and the panel lit; clear in B and D and the panel went dark,
 * including in B, where CTRL bit 28 said the block was clocked. It is not a
 * clock enable - it is what drives the pad - and calling it either "the
 * counter's clock enable" or "an output force that hides the duty" produced
 * one wrong conclusion each. There is no duty to hide: the counter has never
 * run.
 *
 * [M] The 2026-08-07 sweep's other half stands: the panel lit at 0x100 and
 * stayed lit
 * through 0x10F, so bits [3:0] - what 0x00811EC0 assembles as a source
 * select - make no difference to whether it runs.
 *
 * [!] THE WRITABLE MASK IS 0x1FF, NOT 0x10F. Corrected 2026-08-14 by
 * examples/PwmSrc, which writes all ones and reads back. The 0x10F figure was
 * taken by writing 0x3F0F - which does not set bits [7:4] AT ALL - so it was
 * a consistent measurement of a register it had not fully probed. Nine bits
 * are implemented, [8:0], and **[7:4] have never been written by anything**:
 * not by this framework, not by the bootloader, not by the application.
 *
 * [I] Which finally makes sense of 0x00811EC0, `[pair] = src | (div << 8)`.
 * If `src` is a byte it is exactly [7:0] and `div` is the single bit 8 above
 * it - a perfect fit for the nine implemented bits. So [7:0] is a SOURCE
 * SELECT and every run this project has ever done has left it at zero. If
 * zero selects no source, that is the whole of the stopped counter: the
 * block's own gate is open (CTRL bit 28 clears with romclk 39), and there is
 * simply nothing arriving to count. examples/PwmPair sweeps all 256.
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
 *
 * [V] 2026-08-14: confirmed the hard way. The thunk that reaches 0x00811EC0
 * is 0x00811D50, and a scan of the blob, stage 1 and all of XIP finds NO
 * caller. So the vendor genuinely never writes this register, on any path.
 *
 * [M] The experiment that was supposed to separate "clock enable" from
 * "output force" - enable romclk 39 and leave bit 8 clear - has been run
 * three times. The counter does not run either way, and the panel is dark
 * whenever bit 8 is clear. sl6806_backlight_begin() sets it, and that is the
 * whole of why this board has a backlight.
 */
#define SL6806_PWM_PAIR(ch)     (SL6806_PWM_BASE + 0x10u + ((ch) >> 1) * 4u)
/* [M] An output force, not a clock enable. Lights the pad with the counter
 * stopped; irrelevant to whether the counter runs. Kept under the old name so
 * the 2026-08-07 result stays reproducible - see the note above. */
#define SL6806_PWM_PAIR_CLK_EN  (1u << 8)
#define SL6806_PWM_PAIR_FORCE   SL6806_PWM_PAIR_CLK_EN

/* [V] Per-channel register block, 0x20 apart starting at +0x20. */
#define SL6806_PWM_CHAN(ch)     (SL6806_PWM_BASE + 0x20u + (uint32_t)(ch) * 0x20u)

/*
 * Channel registers, as offsets from SL6806_PWM_CHAN(ch).
 */
#define SL6806_PWM_CTRL         0x00
#define SL6806_PWM_PERIOD_DUTY  0x04       /* [V] (period << 16) | duty     */
/*
 * [V] A read-only pair of halfwords, and the block's other live witness.
 * 0x00811EF6 is `return (REG(+0x08) >> (i * 16)) & 0xFFFF` - two 16-bit
 * values selected by an index, reached through the thunk at 0x00811DF8.
 *
 * [M] NOT A LIVE COUNTER. It read 0000/0000 in all five of PwmClock's
 * configurations, running or not, across two sessions. Whatever the two
 * halves are for, they are not a running count, and the guess that they were
 * is retracted. Nothing calls the thunk either.
 *
 * [!] AND NEITHER IS ANYTHING ELSE IN THE BLOCK. "Read the eight words twice
 * and see whether any changed" was reported here as the witness that worked.
 * It was not an independent one: naming the mover showed it every time as
 * `+00 1000007f -> 0000007f` - CTRL bit 28 settling, once, shortly after the
 * clock is enabled. One transient, not a tick, and the same bit the verdict
 * already reported. NO REGISTER IN THIS BLOCK COUNTS.
 *
 * What is independent is sl6806_pwm_commit()'s return: retiring the update
 * bit needs the counter to reach a period boundary.
 */
#define SL6806_PWM_COUNT        0x08
#define SL6806_PWM_MODE         0x10
/*
 * [V] Three more registers the vendor writes and this driver never has, all
 * through 0x00811E08 from a 14-byte config block:
 *
 *     [+0x14] = (cfg[0:1] << 16) | cfg[2:3]
 *     [+0x18] =  cfg[4:5]
 *     [+0x1C] = (cfg[8:9] << 16) | cfg[10:11]
 *
 * so six 16-bit values. Next to period/duty at +0x04 they have the shape of
 * further compare or threshold registers, and every one of them is left at
 * zero here. A compare of zero is a plausible way to get a counter that is
 * configured and does not move - which is why examples/PwmMode's last phase
 * loads them rather than leaving them.
 *
 * [!] The vendor's own values cannot be copied: 0x00D45394 calls 0x00811E08
 * with a stack frame in which only offset 13 is initialised, so the stock
 * firmware writes uninitialised memory into all six. Whatever they mean, the
 * backlight does not depend on them - which also means they are unlikely on
 * their own to be the answer.
 */
#define SL6806_PWM_REG14        0x14       /* [V] (a << 16) | b   [?] role  */
#define SL6806_PWM_REG18        0x18       /* [V] a plain word    [?] role  */
#define SL6806_PWM_REG1C        0x1C       /* [V] (a << 16) | b   [?] role  */

/* CTRL bits. */
#define SL6806_PWM_CTRL_RUN     (1u << 4)  /* [V] 0x00811CE4 sets/clears it */
#define SL6806_PWM_CTRL_UPDATE  (1u << 8)  /* [V] 0x00811D3C sets it,
                                            *     0x00811D4C polls it clear */
/*
 * [M] Bit 28 tracks romclk 39 and NOTHING ELSE THAT MATTERS. It clears when
 * 0x400800F4 bit 0 is set with bit 4 clear, and it is set otherwise - which
 * is why it looked like a run flag for one afternoon. It is not one: the
 * commit witness reports 0/4 in exactly the configurations where this bit
 * says "running", and the panel is dark in one of them.
 *
 * Read it as "the block has a clock", not "the counter is turning". This
 * file has now built two opposite wrong conclusions on this bit - first that
 * it lies, then that it is a run flag - and the lesson both times is the
 * same: it is one bit's opinion about itself, and it needs a witness that
 * observes an EFFECT. sl6806_pwm_commit() is that witness.
 *
 * 0x00811E74 spins on it before writing period and duty, so the vendor treats
 * it as readiness too.
 */
#define SL6806_PWM_CTRL_BUSY    (1u << 28)
#define SL6806_PWM_CTRL_NOCLK   SL6806_PWM_CTRL_BUSY
/* Kept: examples/PwmClock prints it under this name. Not a run flag. */
#define SL6806_PWM_CTRL_STOPPED SL6806_PWM_CTRL_BUSY
#define SL6806_PWM_CTRL_INIT    0x40u      /* [V] written bare at init      */

/*
 * MODE bits, from 0x00811E9A and 0x00811CF4.
 *
 * [!] THE MODE FIELD IS THE LEADING SUSPECT FOR THE STOPPED COUNTER, because
 * it is the one thing the vendor writes and this driver never has.
 * 0x00811E9A composes the whole register as
 *
 *     [chan+0x10] = (x << 16) | (mode << 1) | enable
 *
 * and clears bit 0 first if bits [3:1] are changing - which is the shape of a
 * field you are not allowed to reconfigure while running, i.e. one that
 * selects what the counter DOES. sl6806_pwm_enable() sets bit 0 and leaves
 * mode at 0. If 0 is an idle or hold-the-output mode, that alone accounts for
 * every measurement in §32: registers accept everything, the counter never
 * moves, and the pad answers only the pair register's force bit.
 *
 * examples/PwmMode sweeps it against the commit witness.
 */
#define SL6806_PWM_MODE_ENABLE  (1u << 0)  /* [V] toggled on its own        */
#define SL6806_PWM_MODE_SHIFT   1          /* [V] a 3-bit field at [3:1]    */
#define SL6806_PWM_MODE_MASK    (7u << SL6806_PWM_MODE_SHIFT)
#define SL6806_PWM_MODE_FIELD(m) \
    (((uint32_t)(m) << SL6806_PWM_MODE_SHIFT) & SL6806_PWM_MODE_MASK)

/*
 * [V] CTRL [19:16], the other never-written field. 0x00811ED0 takes five
 * arguments and ORs them in as
 *
 *     [chan+0x00] |= (arg5 << 16) | (r3 << 17) | (r2 << 18) | (r1 << 19)
 *
 * clearing CTRL bit 4 first when arg5 is set. Four single bits, so the whole
 * space is sixteen values.
 */
#define SL6806_PWM_CTRL_HI_SHIFT 16
#define SL6806_PWM_CTRL_HI_MASK  (0xFu << SL6806_PWM_CTRL_HI_SHIFT)
#define SL6806_PWM_CTRL_HI(v) \
    (((uint32_t)(v) << SL6806_PWM_CTRL_HI_SHIFT) & SL6806_PWM_CTRL_HI_MASK)

/*
 * [V] The control word the stock firmware builds for the backlight.
 *
 * 0x00D99C34 assembles it from four bytes as
 *   ((c0 & 1) << 4) | ((c1 & 1) << 7) | (c3 & 0x0F) | ((c2 & 1) << 5)
 * OR'd on top of a bare 0x40, giving 0x7F for the backlight: RUN set, the
 * low nibble all ones, bit 5 set.
 *
 * [!] CORRECTED 2026-08-14. This used to say "the backlight's open passes
 * c0=3". It does not. 0x00D10354's 20-byte config block starts {03 00 01 0F},
 * and that leading 3 is the CHANNEL NUMBER - 0x00D453E8 compares it against
 * the channel-id table at 0x00C7DE94 to find its slot. The byte that becomes
 * bit 4 is a hardcoded 1 stored by 0x00D453C0, and the (c1, c2, c3) the
 * config does supply are its bytes 1, 2 and 3. 0x7F is unchanged, so the
 * constant below was right by luck; the derivation was not, and it would
 * mislead anyone configuring a second channel from a different config.
 *
 * The rest of that config block, for reference: byte 4 is the use_high clock
 * flag (0), bytes 8..11 the completion callback (0x00D102D5), bytes 16..19
 * 0x5DC0BB80 = period 48000, duty 24000. Bytes 5..7 and 12..15 are never
 * written - 0x00D45394 hands 0x00811E08 a stack frame in which only offset
 * 13 is initialised, so the +0x10/+0x14/+0x18/+0x1C writes get junk. Whatever
 * those four registers do, the backlight does not depend on them, and neither
 * does this driver.
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

/* One half of the +0x08 readback, the way 0x00811EF6 takes it. */
static inline uint16_t sl6806_pwm_count(unsigned ch, unsigned half)
{
    uint32_t v = sl6806_mmio_read(SL6806_PWM_CHAN(ch) + SL6806_PWM_COUNT);

    return (uint16_t)(v >> ((half & 1u) * 16));
}

/* ------------------------------------------------------------------ */
/* The backlight                                                       */
/* ------------------------------------------------------------------ */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Turn on the PWM's functional clock - romclk 39, 0x400800F4 bit 0, and with
 * `src_high` also bit 4. sl6806_backlight_begin() calls this with 0, which is
 * what the vendor's backlight does; pass 1 only to try the other branch.
 *
 * [V] from 0x00D99C34 / ROM 0x2338 / ROM 0x3174. Not yet run on hardware.
 */
void sl6806_pwm_clock_begin(int src_high);

/*
 * Latch a staged period/duty and wait for it to retire. Returns 1 if it did,
 * 0 on timeout.
 *
 * [M] The return value is the block's one witness that is mechanically
 * distinct from CTRL bit 28: retiring needs the counter to reach a period
 * boundary, where bit 28 only reports what the block believes about its own
 * clock. Use it, not bit 28, when the question is "is it really running".
 */
int sl6806_pwm_commit(unsigned ch);

/*
 * Arm both of a channel's interrupt enable bits at 0x40084000 so its status
 * can latch.
 * [M] Required before sl6806_pwm_irq_fired() says anything. Touches only the
 * PWM register, never the NVIC.
 */
void sl6806_pwm_irq_arm(unsigned ch);

/*
 * Read and clear the block's interrupt status, masked to the 12 bits the
 * vendor's ISR tests. Non-zero means a channel raised an event, which a
 * stopped counter cannot do. Arm the channel first.
 */
uint32_t sl6806_pwm_irq_fired(void);

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
 * [M] With romclk 39 on, the counter runs and this does what it says. It was
 * kept through twelve sessions in which it did nothing, on the grounds that
 * the register values were right and would matter the day the counter
 * started. They were, and it did.
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
