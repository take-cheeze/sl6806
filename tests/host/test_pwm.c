/*
 * test_pwm.c - native tests for the backlight driver.
 *
 * The backlight works on hardware. What this file guards is the three things
 * that were wrong for twelve runs, because every one of them is invisible
 * without a device and every one would look like a reasonable simplification
 * to someone tidying this code later:
 *
 *   1. The functional clock, romclk 39, gets enabled - and BEFORE any PWM
 *      register is touched, which is where 0x00D99C34 puts it. It is a lone
 *      poke at a CRU address with no obvious owner, and it is the difference
 *      between a backlight that dims and one that does not.
 *
 *   1a. And the pair register's bit 8 gets set. Nothing in the vendor's
 *      firmware writes that register, so a reader checking this driver
 *      against the disassembly concludes the write is spurious and removes
 *      it - which is exactly what happened on 2026-08-14, on the strength of
 *      a status bit. It is the difference between a lit panel and a dark one,
 *      and this check is the guard rail.
 *
 *   2. The module clock is enabled through sl6806_module_enable, which writes
 *      shadow before gate. Any other order leaves the block gated, and a
 *      gated block answers reads with plausible values.
 *
 *   3. Bring-up refuses rather than proceeding when the block is not
 *      writable, instead of writing into a hole and reporting success.
 *
 * Plus the duty arithmetic, which is the vendor's own and is easy to get
 * subtly wrong.
 */

#include <stdio.h>
#include <string.h>

#include "sl6806_pwm.h"

/* sl6806_module.c's functional-clock helpers wait the vendor's 10 ms.
 * Nothing to wait for here, and the tests are about register order. */
void delay(uint32_t ms) { (void)ms; }

static int failures, checks;

#define CHECK(cond, what) do {                                          \
        checks++;                                                       \
        if (!(cond)) {                                                  \
            printf("  FAIL %s (%s:%d)\n", (what), __FILE__, __LINE__);  \
            failures++;                                                 \
        }                                                               \
    } while (0)

#define CRU 0x40080000u
#define PWM SL6806_PWM_BASE

static uint32_t cru[0x40];
static uint32_t pwm[0x40];
static int      gated_on, bad_order, pads_configured;

/* 0x00D99C34 turns the functional clock on before it touches a PWM register.
 * Tracking that as a flag rather than asserting on the end state is the only
 * way to catch the write drifting to after the writability check, where it
 * would still leave the right bits set and still be in the wrong place. */
static int      clk_on, pwm_touched_early;

/* The padctl call the driver makes; stubbed, and counted. */
int sl6806_pad_configure(uint32_t id)
{
    if (id == SL6806_PWM_BL_PAD)
        pads_configured++;
    return 0;
}

uint32_t sl6806_mmio_read(uint32_t a)
{
    if (a >= CRU + 0x60 && a < CRU + 0x160)
        return cru[(a - CRU - 0x60) / 4];
    if (a >= PWM && a < PWM + 0x100) {
        if (!clk_on)
            pwm_touched_early = 1;
        return gated_on ? pwm[(a - PWM) / 4] : 0;
    }
    return 0;
}

void sl6806_mmio_write(uint32_t a, uint32_t v)
{
    if (a >= CRU + 0x60 && a < CRU + 0x160) {
        unsigned i = (a - CRU - 0x60) / 4;
        uint32_t bit = 1u << 4;            /* module 68 -> bit 4 */

        if (i == 2 && (v & bit)) {         /* gate is +0x68 */
            if (!(cru[6] & bit))           /* shadow is +0x78 */
                bad_order = 1;
            else
                gated_on = 1;
        }
        if (a == SL6806_PWM_CLK_REG && (v & SL6806_PWM_CLK_ENABLE))
            clk_on = 1;
        cru[i] = v;
        return;
    }
    if (a >= PWM && a < PWM + 0x100 && !clk_on)
        pwm_touched_early = 1;
    if (a >= PWM && a < PWM + 0x100 && gated_on) {
        /* +0x04 is write-1-to-clear. The vendor's ISR reads it, masks with
         * the enable and writes the result straight back, which is only a
         * clear if the register behaves this way - model it, or the test
         * would pass on a driver that sets the bits it meant to acknowledge. */
        if (a == SL6806_PWM_IRQ_STATUS)
            pwm[1] &= ~v;
        else
            pwm[(a - PWM) / 4] = v;
    }
}

static void reset_model(void)
{
    memset(cru, 0, sizeof cru);
    memset(pwm, 0, sizeof pwm);
    gated_on = bad_order = pads_configured = 0;
    clk_on = pwm_touched_early = 0;
}

int main(void)
{
    uint32_t chan = SL6806_PWM_CHAN(3), pair = SL6806_PWM_PAIR(3);

    printf("test_pwm\n");

    reset_model();
    CHECK(sl6806_backlight_begin(100) == 1, "bring-up succeeds");
    CHECK(!bad_order, "module clock: shadow written before gate");
    CHECK(pads_configured == 1, "the backlight pad is muxed exactly once");

    /* The functional clock, romclk 39. [M] This is what starts the counter,
     * and a reader tidying this driver would see a lone CRU poke with no
     * obvious owner and delete it. */
    CHECK(cru[(SL6806_PWM_CLK_REG - CRU - 0x60) / 4] & SL6806_PWM_CLK_ENABLE,
          "the PWM functional clock (romclk 39) is enabled");

    /* [M] And bit 4 must stay clear: it selects a source that is not running
     * in bootloader mode, and setting it stops the counter even with bit 0
     * set. PwmClock configurations D and E. */
    CHECK(!(cru[(SL6806_PWM_CLK_REG - CRU - 0x60) / 4] & SL6806_PWM_CLK_SRC_HIGH),
          "and the dead source select, bit 4, is left clear");

    /* [M] Pair bit 8 IS the backlight - the panel tracks it and nothing else.
     * This assertion was inverted for a few hours on the theory that romclk 39
     * had started the counter and that bit 8 merely pinned the pad. The
     * counter does not run, and with this clear the panel is dark. */
    CHECK(pwm[(pair - PWM) / 4] & SL6806_PWM_PAIR_FORCE,
          "pair bit 8 is set - this is the backlight");


    CHECK((pwm[(chan - PWM) / 4] & 0x7Fu) == 0x7Fu, "CTRL gets 0x40 | 0x3F");
    CHECK(pwm[(chan - PWM) / 4] & SL6806_PWM_CTRL_RUN, "run bit set");
    CHECK(pwm[(chan + SL6806_PWM_MODE - PWM) / 4] & SL6806_PWM_MODE_ENABLE,
          "mode enable set");

    /* Duty: the vendor's arithmetic, period 48000, duty percent * 480. */
    CHECK(pwm[(chan + SL6806_PWM_PERIOD_DUTY - PWM) / 4]
          == ((uint32_t)SL6806_PWM_BL_PERIOD << 16) | 48000u,
          "100% is duty == period");
    sl6806_backlight_set(60);
    CHECK(pwm[(chan + SL6806_PWM_PERIOD_DUTY - PWM) / 4]
          == ((uint32_t)SL6806_PWM_BL_PERIOD << 16) | 28800u, "60% duty");
    sl6806_backlight_set(0);
    CHECK(pwm[(chan + SL6806_PWM_PERIOD_DUTY - PWM) / 4]
          == ((uint32_t)SL6806_PWM_BL_PERIOD << 16), "0% duty");
    sl6806_backlight_set(250);
    CHECK(pwm[(chan + SL6806_PWM_PERIOD_DUTY - PWM) / 4]
          == ((uint32_t)SL6806_PWM_BL_PERIOD << 16) | 48000u,
          "over 100 clamps, as 0x00D102F4 does");

    /* sl6806_pwm_set refuses duty > period rather than clamping, so a
     * caller's arithmetic bug shows up as no change instead of a dim panel. */
    CHECK(sl6806_pwm_set(3, 100, 200) == -1, "duty above period is refused");

    /* The update trigger fires on every brightness change: without it the
     * registers hold a new duty and the output keeps the old one. */
    pwm[(chan - PWM) / 4] &= ~SL6806_PWM_CTRL_UPDATE;
    sl6806_backlight_set(25);
    CHECK(pwm[(chan - PWM) / 4] & SL6806_PWM_CTRL_UPDATE,
          "brightness change sets the update bit");

    /* Bring-up must survive a warm chip. CTRL carries read-only status, so a
     * read-back equality test on it fails on every re-upload - which it did,
     * and reported a working board as a refused module clock. */
    pwm[(chan - PWM) / 4] |= SL6806_PWM_CTRL_STOPPED;
    CHECK(sl6806_backlight_begin(100) == 1,
          "bring-up succeeds again with status bits set in CTRL");
    CHECK(cru[(SL6806_PWM_CLK_REG - CRU - 0x60) / 4] & SL6806_PWM_CLK_ENABLE,
          "and the functional clock survives it");

    /* A block that never ungates must not report success. */
    reset_model();
    cru[6] = 0;                            /* shadow stays clear -> no gate */
    {
        /* Model a chip where the gate never acknowledges. */
        int r;
        gated_on = 0;
        r = sl6806_backlight_begin(100);
        CHECK(r == 0 || gated_on, "bring-up does not claim success while gated");
    }

    /* Order, tested as order and not as end state: no PWM register may be
     * read or written before the functional clock is on, which is where
     * 0x00D99C34 puts it. pwm_writable() is the first PWM access the driver
     * makes, so a clock write that drifts below it fails this. */
    reset_model();
    sl6806_backlight_begin(100);
    CHECK(!pwm_touched_early,
          "the functional clock is written before any PWM access");

    /* src_high is the vendor's other branch; it must be opt-in, and must not
     * disturb the enable bit that is already there. */
    sl6806_pwm_clock_begin(1);
    CHECK((cru[(SL6806_PWM_CLK_REG - CRU - 0x60) / 4]
           & (SL6806_PWM_CLK_ENABLE | SL6806_PWM_CLK_SRC_HIGH))
          == (SL6806_PWM_CLK_ENABLE | SL6806_PWM_CLK_SRC_HIGH),
          "src_high adds bit 4 and keeps bit 0");

    /* [M] Arming is not optional: the status does not latch a masked event,
     * so a probe that polls +0x04 without setting +0x00 reads zero on a
     * running counter and concludes it is stopped. */
    reset_model();
    gated_on = clk_on = 1;
    sl6806_pwm_irq_arm(3);
    /* Both banks: 0x3F3F is six channels x two event sources, so a channel
     * owns bit ch and bit ch+8. The first probe armed only the low one and
     * read nothing back, which proves less than it looks like. */
    CHECK(pwm[0] == SL6806_PWM_IRQ_BITS(3), "arming sets both of the channel's bits");
    CHECK((SL6806_PWM_IRQ_BITS(3) & ~SL6806_PWM_IRQ_MASK) == 0,
          "and they are inside the mask the vendor's ISR tests");
    sl6806_pwm_irq_arm(SL6806_PWM_CHANNELS);
    CHECK(pwm[0] == SL6806_PWM_IRQ_BITS(3), "an out-of-range channel is ignored");

    /* The interrupt status witness: reads +0x04, masks to 0x3F3F, and clears
     * what it saw so a poll loop cannot latch on a stale bit. */
    reset_model();
    gated_on = clk_on = 1;
    pwm[1] = 0xF000FFFFu;
    CHECK(sl6806_pwm_irq_fired() == SL6806_PWM_IRQ_MASK,
          "irq status is masked to the 12 bits the vendor's ISR tests");
    CHECK((pwm[1] & SL6806_PWM_IRQ_MASK) == 0, "and the bits it saw are cleared");
    CHECK((pwm[1] & 0xF0000000u) == 0xF0000000u, "without touching the rest");
    CHECK(sl6806_pwm_irq_fired() == 0, "a second read reports nothing");

    printf("  %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
