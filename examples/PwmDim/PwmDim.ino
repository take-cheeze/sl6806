/*
 * PwmDim - the last question: does the panel dim, and does bit 8 have to go?
 *
 * [M] WHAT IS SETTLED. examples/PwmSlow timed the commit with microsecond
 * resolution and the fast source came out exact:
 *
 *     period 48000 -> 2002 us      period 12000 ->  502 us
 *     period 24000 -> 1002 us      period  6000 ->  252 us
 *
 *     t = period / 24 MHz + 2 us,  zero residual on all four rows
 *
 * So romclk 39 with bit 4 CLEAR clocks the PWM counter at exactly 24.000 MHz,
 * and the 2 us is this sketch's own read loop, not hardware. The vendor's
 * period of 48000 therefore makes a 500 Hz output - which is what a backlight
 * PWM is supposed to be, and is a good independent check that the whole decode
 * is right. Bit 4 selects a second source near 25 kHz; at period 48000 that is
 * a half-hertz blink, which is exactly what was seen on the bench.
 *
 * WHAT IS NOT SETTLED, and it is the only thing left that matters to the
 * driver: with the counter genuinely running at 500 Hz, does the PANEL follow
 * the duty - and is the pair register's bit 8 still required?
 *
 * The evidence pulls both ways. PwmClock's configuration B - fast clock, bit 8
 * clear - was reported DARK across a full duty sweep. But PwmSrc left the chip
 * with bit 8 clear on the slow source and the panel BLINKED, which means the
 * counter can reach the pad without bit 8. Those cannot both be the whole
 * story, and every previous attempt to settle it by inference has been wrong.
 *
 * [!] AND THE FIRST VERSION OF THIS SKETCH COULD NOT ANSWER IT. Its log came
 * back with twenty-four ramp headers and NOT ONE `done:` line, because any
 * keypress reset `step` to 0 and restarted the ramp. A ramp is 200 steps at
 * the host's ~115 ms poll, so it needs 23 uninterrupted seconds; it never got
 * more than a few. The duty therefore never climbed off zero, and "bit 8
 * clear is dark" is exactly what 0% duty looks like whether or not the pad
 * carries the waveform. Nothing was tested.
 *
 * Fixed here three ways: keys are ignored while a row runs, the ramp is short
 * enough to finish, and THE DUTY IS PRINTED AS IT GOES so the log itself is
 * evidence that the sweep happened. A test whose transcript cannot show it ran
 * is not a test.
 *
 * THREE ROWS, and the third is a POSITIVE CONTROL - the thing missing from
 * every measurement in this investigation:
 *
 *   1  fast 24 MHz, bit 8 CLEAR - the question. Dims if the pad follows.
 *   2  fast 24 MHz, bit 8 SET   - what the driver ships.
 *   3  slow 25 kHz, bit 8 CLEAR - MUST BLINK at about 0.5 Hz.
 *
 * Row 3 is the state examples/PwmSrc left behind when the panel was seen
 * blinking, so it is known to put the counter onto the pad. If it blinks, the
 * rig works and row 1 being dark is a real result. If it does not blink, this
 * sketch is measuring nothing, rows 1 and 2 mean nothing, and the earlier
 * blink had another cause.
 *
 *     make SKETCH=examples/PwmDim RUN_MODE=poll run
 *
 * Then '0'. Rows 1 and 2 at 24 MHz / 48000 = 500 Hz are far too fast to
 * flicker, so anything seen there is brightness, not blinking.
 *
 *   row 3 does not blink -> the test is broken; ignore the rest
 *   only 1 dims          -> bit 8 pins the pad; the driver should drop it
 *   only 2 dims          -> bit 8 is an output enable; the driver is right
 *   both dim             -> bit 8 is irrelevant once the counter runs
 *   neither dims         -> the pad is not carrying the waveform, and the
 *                           next question is the pad, not the PWM
 *
 * Nothing blocks: one duty step per loop() call, paced with millis().
 */

#include <Arduino.h>
#include "sl6806_pwm.h"
#include "sl6806_module.h"
#include "sl6806_mmio.h"
#include "sl6806_padctl.h"

#define CH 3
#define STEP_MS 100
#define STEPS   60           /* 30 up, 30 down: ~7 s at the host poll rate */

/*
 * Three configurations, and the third is the point.
 *
 * [!] EVERY TEST IN THIS INVESTIGATION HAS LACKED A POSITIVE CONTROL, and
 * that is why four of them reported clean negatives that were really the
 * instrument reporting its own limit. So one is included here: the slow
 * source with bit 8 clear is the state examples/PwmSrc left behind when the
 * panel was observed BLINKING. It is the one configuration known to put the
 * counter onto the pad.
 *
 *   if the control blinks, the rig works and row 1 being dark is a result;
 *   if the control does NOT blink, this whole sketch is measuring nothing and
 *   the earlier blink had another cause.
 *
 * At ~25 kHz over 48000 counts the control is a half-hertz blink, which is
 * unmistakable and needs no brightness comparison.
 */
struct cfgrow {
    const char *name;
    uint32_t    clk;
    int         force;       /* pair bit 8 */
    int         ramp;        /* ramp the duty, or hold it at 50% */
};

static const struct cfgrow cfgs[] = {
    { "1  fast 24 MHz, bit8 CLEAR  (the vendor's - does it dim?)",
      SL6806_PWM_CLK_ENABLE, 0, 1 },
    { "2  fast 24 MHz, bit8 SET    (what the driver ships)",
      SL6806_PWM_CLK_ENABLE, 1, 1 },
    { "3  slow ~25 kHz, bit8 CLEAR (POSITIVE CONTROL - must blink)",
      SL6806_PWM_CLK_ENABLE | SL6806_PWM_CLK_SRC_HIGH, 0, 0 },
};
#define NCFG ((int)(sizeof cfgs / sizeof cfgs[0]))

static int      cfg = -1, last_cfg, step;
static uint32_t due;

static void apply(const struct cfgrow *c)
{
    uint32_t chan = SL6806_PWM_CHAN(CH);

    sl6806_module_enable(SL6806_PWM_MODULE_ID);
    sl6806_mmio_clr(SL6806_PWM_CLK_REG,
                    SL6806_PWM_CLK_ENABLE | SL6806_PWM_CLK_SRC_HIGH);
    sl6806_mmio_set(SL6806_PWM_CLK_REG, c->clk);
    sl6806_pad_configure(SL6806_PWM_BL_PAD);

    sl6806_mmio_write(chan + SL6806_PWM_CTRL, SL6806_PWM_CTRL_INIT);
    sl6806_mmio_write(chan + SL6806_PWM_CTRL, SL6806_PWM_CTRL_INIT | 0x3Fu);
    sl6806_mmio_write(SL6806_PWM_PAIR(CH), c->force ? SL6806_PWM_PAIR_FORCE : 0);

    sl6806_pwm_set(CH, SL6806_PWM_BL_PERIOD, SL6806_PWM_BL_DUTY(50));
    sl6806_mmio_write(chan + SL6806_PWM_MODE, 0);
    sl6806_mmio_write(chan + SL6806_PWM_MODE, SL6806_PWM_MODE_ENABLE);
}

static void set_duty(unsigned pct)
{
    sl6806_pwm_set(CH, SL6806_PWM_BL_PERIOD, (uint16_t)SL6806_PWM_BL_DUTY(pct));
    /* Not sl6806_pwm_commit(): its bound is tens of microseconds and one
     * period here is 2 ms, so it always times out. The write lands; the
     * counter picks it up at the next boundary regardless. */
    sl6806_mmio_set(SL6806_PWM_CHAN(CH) + SL6806_PWM_CTRL,
                    SL6806_PWM_CTRL_UPDATE);
}

void setup()
{
    Serial.begin(115200);
    Serial.println();
    Serial.println("=== SL6806 backlight: does it dim, and is bit 8 needed? ===");
    Serial.println("row 3 is a POSITIVE CONTROL and must blink at ~0.5 Hz.");
    Serial.println("If it does not, ignore rows 1 and 2 entirely.");
    Serial.println("'0' runs all three, 'q' aborts. Keys are ignored while");
    Serial.println("a row is running - the previous version restarted on every");
    Serial.println("press, so the duty never left zero and nothing was tested.");
}

void loop()
{
    int key = Serial.read();

    if (key == 'q') {
        cfg = -1;
        Serial.println("aborted.");
        return;
    }
    /* Start only from idle. A keypress mid-row is dropped on purpose. */
    if (cfg < 0 && (key == '0' || (key >= '1' && key <= '0' + NCFG))) {
        cfg = (key == '0') ? 0 : key - '1';
        last_cfg = (key == '0') ? NCFG - 1 : cfg;
        step = 0;
        due = millis();
    }

    if (cfg < 0 || (int32_t)(millis() - due) < 0)
        return;

    if (step == 0) {
        Serial.printf("\n--- %s\n", cfgs[cfg].name);
        apply(&cfgs[cfg]);
        Serial.println(cfgs[cfg].ramp
                       ? "    ramping duty 0 -> 100 -> 0; WATCH THE PANEL"
                       : "    holding duty 50%; the panel should BLINK");
    }

    if (cfgs[cfg].ramp) {
        int half = STEPS / 2;
        unsigned pct = (unsigned)(step <= half ? step * 100 / half
                                              : (STEPS - step) * 100 / half);

        set_duty(pct);
        /* Print the duty as it goes, so the log itself is evidence that the
         * ramp actually happened. Its absence is what made the last run
         * unreadable. */
        if ((step % 10) == 0)
            Serial.printf("    step %3d/%d  duty %3u%%\n", step, STEPS, pct);
    }

    if (++step > STEPS) {
        Serial.printf("    done: pair %08x  CTRL %08x  cru+F4 %08x\n",
                      (unsigned)sl6806_mmio_read(SL6806_PWM_PAIR(CH)),
                      (unsigned)sl6806_mmio_read(SL6806_PWM_CHAN(CH)
                                                 + SL6806_PWM_CTRL),
                      (unsigned)sl6806_mmio_read(SL6806_PWM_CLK_REG));
        step = 0;
        if (cfg < last_cfg) {
            cfg++;
        } else {
            cfg = -1;
            Serial.println("\nDid row 3 blink? If not, rows 1 and 2 mean nothing.");
        }
    }
    due = millis() + STEP_MS;
}
