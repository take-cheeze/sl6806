/*
 * sl6806_pwm.c - the backlight. See sl6806_pwm.h for the recipe and for the
 * two status bits that will mislead you if you use them as tests.
 */

#include "sl6806_pwm.h"
#include "sl6806_module.h"
#include "sl6806_padctl.h"

#define BL_CHAN 3

/*
 * Latch a new period/duty. Returns 1 if the commit retired, 0 if it timed out.
 *
 * The vendor exposes exactly this as a pair of accessors - 0x00811E62 sets
 * CTRL bit 8 and 0x00811E6C polls it clear - which is the shape of a shadow
 * register commit. Without it the registers hold the new duty and the output
 * keeps the old one, which is what "the backlight is on but the brightness
 * never changes" looked like.
 *
 * [M] THE RETURN VALUE IS A WITNESS, and the only one this block has that is
 * mechanically distinct from CTRL bit 28. Bit 28 is a state flag - it says the
 * block believes it is clocked. Bit 8 retiring requires the counter to
 * actually REACH A PERIOD BOUNDARY, which a stopped counter cannot do however
 * it feels about its own state. When the two disagree, this one is the
 * stronger evidence.
 *
 * Bounded, because this can run inside the boot ROM's USB handler.
 */
int sl6806_pwm_commit(unsigned ch)
{
    uint32_t ctrl = SL6806_PWM_CHAN(ch) + SL6806_PWM_CTRL;
    unsigned i;

    sl6806_mmio_write(ctrl, sl6806_mmio_read(ctrl) | SL6806_PWM_CTRL_UPDATE);

    for (i = 0; i < 1000u; i++)
        if (!(sl6806_mmio_read(ctrl) & SL6806_PWM_CTRL_UPDATE))
            return 1;
    return 0;
}

#define pwm_commit(ch) ((void)sl6806_pwm_commit(ch))

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

/*
 * Arm a channel's interrupt so its status bit can latch.
 *
 * [M] Required. The status at 0x40084004 stayed 0 through both of the
 * configurations whose counter was running, so this block does not latch a
 * masked event. Writing the enable is all this does - the NVIC is untouched,
 * so nothing needs a handler and nothing can be taken off the bus.
 */
void sl6806_pwm_irq_arm(unsigned ch)
{
    if (ch < SL6806_PWM_CHANNELS)
        sl6806_mmio_set(SL6806_PWM_IRQ_ENABLE, SL6806_PWM_IRQ_BITS(ch));
}

/*
 * The block's own interrupt status: a positive "a period boundary happened",
 * which is the one thing a stopped counter cannot fake.
 *
 * The ISR at 0x0080EE44 reads 0x40084004, masks it with the enable at
 * 0x40084000 and writes the result back to clear. Call sl6806_pwm_irq_arm()
 * first or this reports nothing and means nothing.
 *
 * Clears what it saw, so a caller can poll it in a loop. Returns the bits.
 */
uint32_t sl6806_pwm_irq_fired(void)
{
    uint32_t pend = sl6806_mmio_read(SL6806_PWM_IRQ_STATUS)
                  & SL6806_PWM_IRQ_MASK;

    if (pend)
        sl6806_mmio_write(SL6806_PWM_IRQ_STATUS, pend);
    return pend;
}

/*
 * The functional clock: romclk 39, 0x400800F4 bit 0.
 *
 * Transcribed from ROM 0x2338 rather than called, because the ROM's handler
 * for this id is one OR and a payload has no veneer to it. `src_high` is the
 * vendor's clk_setsrc(39, 16) - bit 4 of the same register - which the
 * backlight does not use. See sl6806_pwm.h.
 */
void sl6806_pwm_clock_begin(int src_high)
{
    if (src_high)
        sl6806_mmio_set(SL6806_PWM_CLK_REG, SL6806_PWM_CLK_SRC_HIGH);
    sl6806_mmio_set(SL6806_PWM_CLK_REG, SL6806_PWM_CLK_ENABLE);
}

int sl6806_backlight_begin(unsigned percent)
{
    uint32_t base = SL6806_PWM_CHAN(BL_CHAN);
    uint32_t pair = SL6806_PWM_PAIR(BL_CHAN);

    if (!sl6806_module_enable(SL6806_PWM_MODULE_ID))
        return 0;

    /* The vendor's second step, and the one this driver went without for
     * every hardware run so far. Module 68 makes the registers writable;
     * this is what clocks the counter behind them. 0x00D99C34 does it here,
     * before it touches a single PWM register, so it is done here too. */
    sl6806_pwm_clock_begin(0);

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

    /*
     * [M] THIS WRITE IS THE BACKLIGHT. Do not remove it again.
     *
     * It was removed on 2026-08-14 on the theory that romclk 39 had started
     * the counter and that bit 8 was an output force hiding the duty. The
     * first half was wrong and the second half does not matter: with bit 8
     * clear the panel is DARK, in every configuration, clocked or not.
     * examples/PwmClock, third session - keys 1/3/5 (bit 8 set) lit the panel
     * and 2/4 (bit 8 clear) blacked it out, tracking this bit alone and
     * nothing else. The counter is still not running; see sl6806_pwm.h.
     *
     * Nothing in the vendor's image writes this register, which is what made
     * it look spurious. It is not spurious. It is the only reason this board
     * has ever had a backlight.
     */
    sl6806_mmio_set(pair, SL6806_PWM_PAIR_FORCE);

    sl6806_backlight_set(percent);
    sl6806_pwm_enable(BL_CHAN, 1);
    sl6806_pwm_run(BL_CHAN, 1);
    return 1;
}
