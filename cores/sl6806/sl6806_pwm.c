/*
 * sl6806_pwm.c - the backlight. See sl6806_pwm.h for the recipe and for the
 * two status bits that will mislead you if you use them as tests.
 */

#include "sl6806_pwm.h"
#include "sl6806_module.h"
#include "sl6806_padctl.h"

#define BL_CHAN 3

/*
 * Latch a new period/duty.
 *
 * The vendor exposes exactly this as a pair of accessors - 0x00811E62 sets
 * CTRL bit 8 and 0x00811E6C polls it clear - which is the shape of a shadow
 * register commit. Without it the registers hold the new duty and the output
 * keeps the old one, which is what "the backlight is on but the brightness
 * never changes" looked like.
 *
 * Bounded, because this can run inside the boot ROM's USB handler.
 */
static void pwm_commit(unsigned ch)
{
    uint32_t ctrl = SL6806_PWM_CHAN(ch) + SL6806_PWM_CTRL;
    unsigned i;

    sl6806_mmio_write(ctrl, sl6806_mmio_read(ctrl) | SL6806_PWM_CTRL_UPDATE);

    for (i = 0; i < 1000u; i++)
        if (!(sl6806_mmio_read(ctrl) & SL6806_PWM_CTRL_UPDATE))
            return;
}

void sl6806_backlight_set(unsigned percent)
{
    if (percent > 100)
        percent = 100;
    sl6806_pwm_set(BL_CHAN, SL6806_PWM_BL_PERIOD,
                   (uint16_t)SL6806_PWM_BL_DUTY(percent));
    pwm_commit(BL_CHAN);
}

/*
 * Does the block take writes? Probe period/duty, not CTRL.
 *
 * CTRL carries read-only status - bit 28 among them - so writing 0x40 and
 * comparing the read-back for equality fails on a chip where the block is
 * already running, which is every re-upload without a replug. That bug made
 * bring-up report "the module clock refused" on a perfectly working board.
 */
static int pwm_writable(void)
{
    uint32_t reg = SL6806_PWM_CHAN(BL_CHAN) + SL6806_PWM_PERIOD_DUTY;
    uint32_t saved = sl6806_mmio_read(reg);
    int ok;

    sl6806_mmio_write(reg, 0x12345678u);
    ok = (sl6806_mmio_read(reg) == 0x12345678u);
    sl6806_mmio_write(reg, saved);
    return ok;
}

int sl6806_pwm_counter_ticking(unsigned ch)
{
    uint32_t base = SL6806_PWM_CHAN(ch);
    uint32_t first[8];
    unsigned i, k;

    for (i = 0; i < 8; i++)
        first[i] = sl6806_mmio_read(base + i * 4);

    /* Give a counter time to move without blocking the USB handler. */
    for (k = 0; k < 4; k++)
        for (i = 0; i < 8; i++)
            if (sl6806_mmio_read(base + i * 4) != first[i])
                return 1;
    return 0;
}

int sl6806_backlight_begin(unsigned percent)
{
    uint32_t base = SL6806_PWM_CHAN(BL_CHAN);
    uint32_t pair = SL6806_PWM_PAIR(BL_CHAN);

    if (!sl6806_module_enable(SL6806_PWM_MODULE_ID))
        return 0;

    /* A gated block drops writes silently, so check rather than assume - and
     * check somewhere without status bits. Safe to run twice: everything
     * below is written unconditionally, so a warm chip is reconfigured
     * rather than skipped. */
    if (!pwm_writable())
        return 0;

    sl6806_pad_configure(SL6806_PWM_BL_PAD);
    sl6806_mmio_write(base + SL6806_PWM_CTRL, SL6806_PWM_CTRL_INIT);

    sl6806_mmio_write(base + SL6806_PWM_CTRL,
                      sl6806_mmio_read(base + SL6806_PWM_CTRL) | 0x3Fu);

    /* The bit twelve hardware runs went without. Everything else can be
     * perfect and the counter will not move until this is set. */
    sl6806_mmio_write(pair, sl6806_mmio_read(pair) | SL6806_PWM_PAIR_CLK_EN);

    sl6806_backlight_set(percent);
    sl6806_pwm_enable(BL_CHAN, 1);
    sl6806_pwm_run(BL_CHAN, 1);
    return 1;
}
