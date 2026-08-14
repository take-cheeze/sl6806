/*
 * PwmDuty - above flicker fusion, does duty still change the BRIGHTNESS?
 *
 * [M] WHERE THIS STARTS. examples/PwmFreq established two things:
 *
 *   phase A: at 0.5 Hz the panel blinked at duty 10%, 50% and 90% - so the
 *            counter drives the pad and duty does not stop it;
 *   phase B: the blink became indistinguishable somewhere around 20-50 Hz.
 *
 * THAT FUSION THRESHOLD IS THE EYE, NOT THE HARDWARE. Human flicker fusion is
 * 20-60 Hz. So phase B did not find a limit in the panel; it found the limit
 * of watching a panel blink. Above it, "I can't see a difference" is exactly
 * what BOTH answers look like:
 *
 *   - steady half-brightness (dimming works, and it fused as it should), and
 *   - off (the pin stops driving above some frequency)
 *
 * are both "no visible change from one row to the next". The earlier report
 * that 500 Hz was BLACK cannot be reconciled with fusion into half-brightness,
 * so one of the two readings is wrong and neither can be told apart by asking
 * "is it steady-half?" - that needs a remembered comparison against a state
 * that is no longer on the screen.
 *
 * THE FIX IS A WITHIN-ROW COMPARISON. At each frequency, alternate duty 10%
 * and 90% every four seconds, twice, announcing each change before it
 * happens. The eye is very good at "did that just get brighter" and very bad
 * at "is this the same brightness as a minute ago", so this asks only the
 * question it can answer.
 *
 *   the panel visibly changes between 10% and 90%  -> duty controls
 *                                                     brightness at this
 *                                                     frequency: dimming
 *                                                     WORKS
 *   no change, panel lit                           -> duty is ignored; the
 *                                                     pin is saturating
 *   no change, panel dark                          -> the pin has stopped
 *                                                     driving at this
 *                                                     frequency
 *
 * The ladder deliberately includes 0.5 Hz, where the answer is already known
 * from PwmFreq - the ratio must visibly differ - so the first row is a
 * POSITIVE CONTROL for the method itself. If 0.5 Hz does not read as
 * different, nothing below it means anything.
 *
 * And 500 Hz appears on both clocks again, because if duty works at 500 Hz on
 * the slow source and not on the fast one, the problem is the clock and not
 * the frequency.
 *
 *     make SKETCH=examples/PwmDuty RUN_MODE=poll run
 *
 * '0' runs the ladder. Each row takes about 16 seconds. Report per row:
 * DIFFERS / SAME-LIT / SAME-DARK.
 *
 * Bit 8 stays CLEAR - PwmDim row 3 proved the pad follows the counter without
 * it, and setting it pins the output and hides everything.
 */

#include <Arduino.h>
#include "sl6806_pwm.h"
#include "sl6806_module.h"
#include "sl6806_mmio.h"
#include "sl6806_padctl.h"

#define CH 3
#define HOLD_MS  4000u
#define NALT     4            /* 10,90,10,90 - two full comparisons */

#define F_FAST 24000000UL
#define F_SLOW (24000000UL / 1024UL)

struct row {
    int         slow;
    uint16_t    period;
    const char *note;
};

static const struct row rows[] = {
    { 1, 46875, "0.5 Hz  slow  <-- POSITIVE CONTROL: the ratio must differ" },
    { 1,  4688, "  5 Hz  slow" },
    { 1,   469, " 50 Hz  slow  (above fusion - brightness, not blink)" },
    { 1,   117, "200 Hz  slow" },
    { 1,    47, "500 Hz  slow  <-- cross-check" },
    { 0, 48000, "500 Hz  FAST  <-- cross-check, same frequency" },
    { 0,  4800, "  5 kHz FAST" },
};
#define NROW ((int)(sizeof rows / sizeof rows[0]))

static int      idx = -1, alt;
static uint32_t due;

static void configure(const struct row *r)
{
    uint32_t chan = SL6806_PWM_CHAN(CH);

    sl6806_module_enable(SL6806_PWM_MODULE_ID);
    sl6806_mmio_clr(SL6806_PWM_CLK_REG,
                    SL6806_PWM_CLK_ENABLE | SL6806_PWM_CLK_SRC_HIGH);
    sl6806_mmio_set(SL6806_PWM_CLK_REG,
                    SL6806_PWM_CLK_ENABLE
                    | (r->slow ? SL6806_PWM_CLK_SRC_HIGH : 0));
    sl6806_pad_configure(SL6806_PWM_BL_PAD);

    sl6806_mmio_write(chan + SL6806_PWM_CTRL, SL6806_PWM_CTRL_INIT);
    sl6806_mmio_write(chan + SL6806_PWM_CTRL, SL6806_PWM_CTRL_INIT | 0x3Fu);
    sl6806_mmio_write(SL6806_PWM_PAIR(CH), 0);
    sl6806_mmio_write(chan + SL6806_PWM_MODE, 0);
    sl6806_mmio_write(chan + SL6806_PWM_MODE, SL6806_PWM_MODE_ENABLE);
}

static void set_duty(const struct row *r, unsigned pct)
{
    uint32_t chan = SL6806_PWM_CHAN(CH);

    sl6806_pwm_set(CH, r->period, (uint16_t)((uint32_t)r->period * pct / 100u));
    sl6806_mmio_set(chan + SL6806_PWM_CTRL, SL6806_PWM_CTRL_UPDATE);
}

void setup()
{
    Serial.begin(115200);
    Serial.println();
    Serial.println("=== SL6806 backlight: does duty change brightness? ===");
    Serial.println("Each row alternates duty 10% and 90% every 4 s, twice.");
    Serial.println("Watch for a CHANGE between them - not an absolute level.");
    Serial.println("Report each row: DIFFERS / SAME-LIT / SAME-DARK");
    Serial.println("Row 1 is a control: at 0.5 Hz the on/off ratio MUST differ.");
    Serial.println("'0' to run, 'q' to abort");
}

void loop()
{
    int key = Serial.read();

    if (key == 'q') { idx = -1; Serial.println("aborted."); return; }

    if (idx < 0) {
        if (key != '0')
            return;
        idx = 0; alt = 0; due = millis();
        Serial.println();
    }

    if ((int32_t)(millis() - due) < 0)
        return;

    if (alt == 0) {
        const struct row *r = &rows[idx];
        /* Print milli-hertz: the previous sketch printed integer Hz and
         * showed "0 Hz" for every sub-hertz row, which made the log useless
         * for the rows that mattered most. */
        unsigned long mhz = (r->slow ? F_SLOW : F_FAST) * 1000UL / r->period;

        configure(r);
        Serial.printf("\n--- %s\n", r->note);
        Serial.printf("    period %5u  -> %lu.%03lu Hz\n",
                      r->period, mhz / 1000, mhz % 1000);
    }

    {
        unsigned pct = (alt & 1) ? 90u : 10u;

        set_duty(&rows[idx], pct);
        Serial.printf("    NOW duty %2u%%   <- look\n", pct);
    }

    if (++alt >= NALT) {
        alt = 0;
        if (idx < NROW - 1) {
            idx++;
        } else {
            idx = -1;
            Serial.println("\ndone. Row 1 must read DIFFERS. For each other row:");
            Serial.println("  DIFFERS   -> dimming works at that frequency");
            Serial.println("  SAME-LIT  -> duty ignored, output saturating");
            Serial.println("  SAME-DARK -> the pin stops driving there");
        }
    }
    due = millis() + HOLD_MS;
}
