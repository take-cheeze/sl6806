/*
 * sl6806_pwm.c - the backlight. See sl6806_pwm.h for the recipe and for the
 * two status bits that will mislead you if you use them as tests.
 */

#include "sl6806_pwm.h"
#include "sl6806_module.h"
#include "sl6806_padctl.h"

#define BL_CHAN 3

void sl6806_backlight_set(unsigned percent)
{
    if (percent > 100)
        percent = 100;
    sl6806_pwm_set(BL_CHAN, SL6806_PWM_BL_PERIOD,
                   (uint16_t)SL6806_PWM_BL_DUTY(percent));
}

int sl6806_backlight_begin(unsigned percent)
{
    uint32_t base = SL6806_PWM_CHAN(BL_CHAN);
    uint32_t pair = SL6806_PWM_PAIR(BL_CHAN);

    if (!sl6806_module_enable(SL6806_PWM_MODULE_ID))
        return 0;

    /* A gated block drops writes silently, so check rather than assume. */
    sl6806_mmio_write(base + SL6806_PWM_CTRL, SL6806_PWM_CTRL_INIT);
    if (sl6806_mmio_read(base + SL6806_PWM_CTRL) != SL6806_PWM_CTRL_INIT)
        return 0;

    sl6806_pad_configure(SL6806_PWM_BL_PAD);

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
