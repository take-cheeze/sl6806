/*
 * PwmFreq - the panel follows the counter at 0.5 Hz and not at 500 Hz.
 *           Where in between does it stop, and is it the frequency or the
 *           clock?
 *
 * [M] WHERE THIS STARTS, and it is the first properly controlled result in
 * this whole investigation. examples/PwmDim ran three rows with a positive
 * control and a transcript that proves the ramp happened:
 *
 *   1  fast 24 MHz, bit 8 clear, duty 0->100->0   -> BLACK at every duty
 *   2  fast 24 MHz, bit 8 set                     -> bright
 *   3  slow 25 kHz, bit 8 clear, duty 50%         -> BLINKS   <- the control
 *
 * Row 3 blinking is what makes rows 1 and 2 mean anything: it proves the
 * counter reaches the pad with bit 8 CLEAR. So the pad is not gated by bit 8,
 * the waveform is real, and rows 1 and 3 differ in exactly one thing:
 *
 *     row 1 output = 24 MHz / 48000 = 500 Hz
 *     row 3 output = 25 kHz / 48000 = 0.5 Hz
 *
 * The panel follows one and not the other. That is no longer a question about
 * the PWM - the PWM is working, measured to 24.000 MHz with zero residual -
 * it is a question about what is on the other end of bank 1 pin 0.
 *
 * TWO PHASES.
 *
 * A. DOES DUTY MODULATE AT ALL, and which way round? Hold 0.5 Hz and step the
 *    duty 10 / 50 / 90 %. At half a hertz the ratio is directly visible, so
 *    this reads the duty and its polarity off the panel with no instrument in
 *    between. If 10% gives a short flash and 90% a long one, duty works and
 *    is active-high; the reverse means active-low; no change means duty does
 *    nothing and everything above is wrong.
 *
 * B. WHERE DOES IT STOP FOLLOWING? A frequency ladder at 50% duty, from 0.5
 *    to 1000 Hz. Report for each: BLINK, FLICKER, STEADY-HALF, or BLACK.
 *
 *    The ladder crosses 500 Hz and 1000 Hz on BOTH clocks - the slow one with
 *    a small period, the fast one with a large one. That is the cross-check
 *    the whole question turns on:
 *
 *      black on both at 500 Hz  -> it is the FREQUENCY. The backlight driver
 *                                  is not a brightness input; it looks like an
 *                                  enable with a slow response, and PWM
 *                                  dimming through this pin is not available.
 *      black only on the fast   -> it is the CLOCK, and something about the
 *                                  fast source is wrong in a way the commit
 *                                  timing did not reveal.
 *
 * WHAT TO EXPECT IF IT IS THE FREQUENCY. Somewhere on the ladder the blink
 * becomes a flicker, then fuses into steady half-brightness - that is the
 * normal way a PWM backlight behaves and would mean dimming works, just not
 * at the frequency tried first. If instead it goes straight from blinking to
 * black without ever passing through steady-half, the pin is not dimming
 * anything.
 *
 *     make SKETCH=examples/PwmFreq RUN_MODE=poll run
 *
 * 'a' phase A, 'b' phase B, '0' both. Keys are ignored while a row runs, and
 * each row prints its own parameters and holds for about six seconds.
 *
 * Bit 8 stays CLEAR throughout - row 3 proved that is not what gates the pad,
 * and setting it would pin the output and hide everything.
 */

#include <Arduino.h>
#include "sl6806_pwm.h"
#include "sl6806_module.h"
#include "sl6806_mmio.h"
#include "sl6806_padctl.h"

#define CH 3
#define HOLD_MS 6000u

/* [M] 24.000 MHz exactly, from examples/PwmSlow. The slow source measured
 * 23.4-26.1 kHz and 24 MHz / 1024 sits inside that, so it is used for the
 * arithmetic - the printed frequency is therefore approximate for slow rows
 * and exact for fast ones. */
#define F_FAST 24000000UL
#define F_SLOW (24000000UL / 1024UL)

struct row {
    int      slow;           /* which source            */
    uint16_t period;
    uint8_t  duty_pct;
    const char *note;
};

static const struct row rows[] = {
    /* Phase A: duty and its polarity, read straight off a half-hertz blink. */
    { 1, 46875, 10, "A  0.5 Hz, duty 10% - expect a SHORT flash if active-high" },
    { 1, 46875, 50, "A  0.5 Hz, duty 50% - expect even on/off" },
    { 1, 46875, 90, "A  0.5 Hz, duty 90% - expect a LONG flash if active-high" },

    /* Phase B: the ladder. 500 and 1000 Hz appear on both clocks. */
    { 1, 46875, 50, "B     0.5 Hz  slow" },
    { 1, 23438, 50, "B       1 Hz  slow" },
    { 1, 11719, 50, "B       2 Hz  slow" },
    { 1,  4688, 50, "B       5 Hz  slow" },
    { 1,  2344, 50, "B      10 Hz  slow" },
    { 1,  1172, 50, "B      20 Hz  slow" },
    { 1,   469, 50, "B      50 Hz  slow" },
    { 1,   234, 50, "B     100 Hz  slow" },
    { 1,   117, 50, "B     200 Hz  slow" },
    { 1,    47, 50, "B     500 Hz  slow  <-- cross-check" },
    { 0, 48000, 50, "B     500 Hz  FAST  <-- cross-check, same freq" },
    { 1,    23, 50, "B    1000 Hz  slow  <-- cross-check" },
    { 0, 24000, 50, "B    1000 Hz  FAST  <-- cross-check, same freq" },
};
#define NROW ((int)(sizeof rows / sizeof rows[0]))
#define PHASE_B_FIRST 3

static int      idx = -1, last_row;
static uint32_t due;
static bool     applied;

static void apply(const struct row *r)
{
    uint32_t chan = SL6806_PWM_CHAN(CH);
    uint32_t duty = (uint32_t)r->period * r->duty_pct / 100u;

    sl6806_module_enable(SL6806_PWM_MODULE_ID);
    sl6806_mmio_clr(SL6806_PWM_CLK_REG,
                    SL6806_PWM_CLK_ENABLE | SL6806_PWM_CLK_SRC_HIGH);
    sl6806_mmio_set(SL6806_PWM_CLK_REG,
                    SL6806_PWM_CLK_ENABLE
                    | (r->slow ? SL6806_PWM_CLK_SRC_HIGH : 0));
    sl6806_pad_configure(SL6806_PWM_BL_PAD);

    sl6806_mmio_write(chan + SL6806_PWM_CTRL, SL6806_PWM_CTRL_INIT);
    sl6806_mmio_write(chan + SL6806_PWM_CTRL, SL6806_PWM_CTRL_INIT | 0x3Fu);

    /* Bit 8 clear: PwmDim row 3 proved the pad follows the counter without
     * it, and setting it would pin the output and hide the answer. */
    sl6806_mmio_write(SL6806_PWM_PAIR(CH), 0);

    sl6806_pwm_set(CH, r->period, (uint16_t)duty);
    sl6806_mmio_write(chan + SL6806_PWM_MODE, 0);
    sl6806_mmio_write(chan + SL6806_PWM_MODE, SL6806_PWM_MODE_ENABLE);
    sl6806_mmio_set(chan + SL6806_PWM_CTRL, SL6806_PWM_CTRL_UPDATE);
}

void setup()
{
    Serial.begin(115200);
    Serial.println();
    Serial.println("=== SL6806 backlight: where does the panel stop following? ===");
    Serial.println("bit 8 CLEAR throughout. duty 50% unless stated.");
    Serial.println("report each row as BLINK / FLICKER / STEADY-HALF / BLACK");
    Serial.println("'a' phase A (duty+polarity), 'b' phase B (ladder), '0' both");
}

void loop()
{
    int key = Serial.read();

    if (key == 'q') { idx = -1; Serial.println("aborted."); return; }

    if (idx < 0) {
        if (key == 'a')      { idx = 0; last_row = PHASE_B_FIRST - 1; }
        else if (key == 'b') { idx = PHASE_B_FIRST; last_row = NROW - 1; }
        else if (key == '0') { idx = 0; last_row = NROW - 1; }
        else                 return;
        applied = false;
        due = millis();
    }

    if ((int32_t)(millis() - due) < 0)
        return;

    if (!applied) {
        const struct row *r = &rows[idx];
        unsigned long f = (r->slow ? F_SLOW : F_FAST) / (r->period ? r->period : 1);

        apply(r);
        Serial.printf("\n--- %s\n", r->note);
        Serial.printf("    period %5u  duty %3u%%  -> %lu Hz  (cru+F4 %08x)\n",
                      r->period, r->duty_pct, f,
                      (unsigned)sl6806_mmio_read(SL6806_PWM_CLK_REG));
        applied = true;
        due = millis() + HOLD_MS;
        return;
    }

    Serial.printf("    held: CTRL %08x  pair %08x\n",
                  (unsigned)sl6806_mmio_read(SL6806_PWM_CHAN(CH)
                                             + SL6806_PWM_CTRL),
                  (unsigned)sl6806_mmio_read(SL6806_PWM_PAIR(CH)));

    applied = false;
    if (idx < last_row) {
        idx++;
        due = millis();
    } else {
        idx = -1;
        Serial.println("\ndone. Which row was the last to BLINK, and did any row");
        Serial.println("read STEADY-HALF? And do the two 500 Hz rows agree?");
    }
}
