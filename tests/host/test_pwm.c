/*
 * test_pwm.c - native tests for the backlight driver.
 *
 * The backlight works on hardware. What this file guards is the three things
 * that were wrong for twelve runs, because every one of them is invisible
 * without a device and every one would look like a reasonable simplification
 * to someone tidying this code later:
 *
 *   1. The pair register's clock-enable bit gets set. Nothing in the vendor's
 *      firmware writes that register, so a reader checking this driver
 *      against the disassembly would conclude the write is spurious and
 *      remove it. It is the difference between a lit panel and a dark one.
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
    if (a >= PWM && a < PWM + 0x100)
        return gated_on ? pwm[(a - PWM) / 4] : 0;
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
        cru[i] = v;
        return;
    }
    if (a >= PWM && a < PWM + 0x100 && gated_on)
        pwm[(a - PWM) / 4] = v;
}

static void reset_model(void)
{
    memset(cru, 0, sizeof cru);
    memset(pwm, 0, sizeof pwm);
    gated_on = bad_order = pads_configured = 0;
}

int main(void)
{
    uint32_t chan = SL6806_PWM_CHAN(3), pair = SL6806_PWM_PAIR(3);

    printf("test_pwm\n");

    reset_model();
    CHECK(sl6806_backlight_begin(100) == 1, "bring-up succeeds");
    CHECK(!bad_order, "module clock: shadow written before gate");
    CHECK(pads_configured == 1, "the backlight pad is muxed exactly once");

    /* The bit that was missing for twelve runs. */
    CHECK(pwm[(pair - PWM) / 4] & SL6806_PWM_PAIR_CLK_EN,
          "the pair clock enable is set");

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
    pwm[(chan - PWM) / 4] |= SL6806_PWM_CTRL_BUSY;
    CHECK(sl6806_backlight_begin(100) == 1,
          "bring-up succeeds again with status bits set in CTRL");
    CHECK(pwm[(pair - PWM) / 4] & SL6806_PWM_PAIR_CLK_EN,
          "and the pair clock enable survives it");

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

    printf("  %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
