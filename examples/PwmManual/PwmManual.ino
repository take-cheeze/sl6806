/*
 * PwmManual - drive the backlight by hand, at your own pace.
 *
 * WHY THIS EXISTS. The last four probes each ran a fixed sequence and asked
 * an observer to judge brightness against something that was no longer on the
 * screen, or that had gone past four seconds ago. That is the wrong job for
 * an eye, and it has produced one ambiguous answer after another:
 *
 *   "brightness changed"        - was a blinking panel
 *   "1 was black"               - was a ramp that never left 0% duty
 *   "can't see a difference"    - was flicker fusion, i.e. the observer
 *   "doesn't seem proportional" - and now this, with no way to tell whether
 *                                 the control row passed
 *
 * So this sketch runs nothing. It holds a state and lets you change one thing
 * at a time, instantly, as often as you like. A/B by keypress beats any
 * timed sequence, because you can flip back and forth until you are sure.
 *
 * KEYS
 *   0 1 2 3 4   duty 0 / 25 / 50 / 75 / 100 %
 *   - +         duty down / up in 5% steps
 *   [ ]         frequency down / up the ladder
 *   f           toggle the source: 24 MHz <-> 25 kHz
 *   b           toggle the pair register's bit 8 (the pad force)
 *   p           print the full register state
 *
 * THE TEST THAT MATTERS is maximum contrast, not 10 vs 90: press '0' then
 * '4', back and forth, at each frequency. If 0% and 100% look identical the
 * pad is not driving at that frequency, and no subtler comparison is going to
 * show it either. At 0.5 Hz the answer is already known - it must differ -
 * so start there and use it to calibrate your own eye before trusting a
 * negative higher up.
 *
 * AND IF THE EYE STILL CANNOT SETTLE IT, use a camera. A phone's slow-motion
 * mode samples at 120-240 fps and resolves PWM well past flicker fusion; two
 * stills at a fixed manual exposure, one at 0% and one at 100%, will differ
 * in a way that is measurable rather than remembered. That is a real
 * instrument and this project has been substituting an eye for one.
 *
 *     make SKETCH=examples/PwmManual RUN_MODE=poll run
 *
 * [M] What is already settled and does not need re-testing: the counter runs
 * at exactly 24.000 MHz (examples/PwmSlow), the pad follows it with bit 8
 * CLEAR (PwmDim row 3), and duty modulates the pulse width active-high
 * (PwmFreq phase A, 10/50/90% giving short/even/long at 0.5 Hz). The only
 * open question is what the panel does with that waveform above 50 Hz.
 */

#include <Arduino.h>
#include "sl6806_pwm.h"
#include "sl6806_module.h"
#include "sl6806_mmio.h"
#include "sl6806_padctl.h"

#define CH 3
#define F_FAST 24000000UL
#define F_SLOW (24000000UL / 1024UL)

/* Periods chosen so the ladder means the same thing on either source: the
 * index is a frequency, and 'f' re-derives the period for the other clock. */
static const unsigned long ladder_hz[] = {
    1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000,
};
#define NLADDER ((int)(sizeof ladder_hz / sizeof ladder_hz[0]))

static int      hz_i = 0;
static int      slow = 1;         /* start on the source that visibly blinks */
static int      force;            /* pair bit 8 */
static unsigned duty_pct = 50;

static uint16_t period_now(void)
{
    unsigned long src = slow ? F_SLOW : F_FAST;
    unsigned long p = src / ladder_hz[hz_i];

    if (p < 2)     p = 2;
    if (p > 65535) p = 65535;
    return (uint16_t)p;
}

static void apply(void)
{
    uint32_t chan = SL6806_PWM_CHAN(CH);
    uint16_t per = period_now();

    sl6806_module_enable(SL6806_PWM_MODULE_ID);
    sl6806_mmio_clr(SL6806_PWM_CLK_REG,
                    SL6806_PWM_CLK_ENABLE | SL6806_PWM_CLK_SRC_HIGH);
    sl6806_mmio_set(SL6806_PWM_CLK_REG,
                    SL6806_PWM_CLK_ENABLE | (slow ? SL6806_PWM_CLK_SRC_HIGH : 0));
    sl6806_pad_configure(SL6806_PWM_BL_PAD);

    sl6806_mmio_write(chan + SL6806_PWM_CTRL, SL6806_PWM_CTRL_INIT);
    sl6806_mmio_write(chan + SL6806_PWM_CTRL, SL6806_PWM_CTRL_INIT | 0x3Fu);
    sl6806_mmio_write(SL6806_PWM_PAIR(CH), force ? SL6806_PWM_PAIR_FORCE : 0);
    sl6806_pwm_set(CH, per, (uint16_t)((uint32_t)per * duty_pct / 100u));
    sl6806_mmio_write(chan + SL6806_PWM_MODE, 0);
    sl6806_mmio_write(chan + SL6806_PWM_MODE, SL6806_PWM_MODE_ENABLE);
    sl6806_mmio_set(chan + SL6806_PWM_CTRL, SL6806_PWM_CTRL_UPDATE);
}

static void show(void)
{
    unsigned long src = slow ? F_SLOW : F_FAST;
    uint16_t per = period_now();
    unsigned long mhz = src * 1000UL / per;

    Serial.printf("duty %3u%%  %lu.%03lu Hz  (period %5u, %s source)  bit8 %s\n",
                  duty_pct, mhz / 1000, mhz % 1000, per,
                  slow ? "25 kHz" : "24 MHz", force ? "SET" : "clear");
}

void setup()
{
    Serial.begin(115200);
    Serial.println();
    Serial.println("=== SL6806 backlight, manual ===");
    Serial.println("0..4 duty 0/25/50/75/100   - + duty step   [ ] frequency");
    Serial.println("f source 24MHz<->25kHz     b pair bit 8    p registers");
    Serial.println();
    Serial.println("Flip '0' and '4' back and forth - that is maximum contrast.");
    Serial.println("Start at 1 Hz where it MUST differ, then walk up with ']'.");
    apply();
    show();
}

void loop()
{
    int key = Serial.read();

    if (key < 0)
        return;

    switch (key) {
    case '0': duty_pct = 0;   break;
    case '1': duty_pct = 25;  break;
    case '2': duty_pct = 50;  break;
    case '3': duty_pct = 75;  break;
    case '4': duty_pct = 100; break;
    case '-': duty_pct = (duty_pct >= 5) ? duty_pct - 5 : 0;    break;
    case '+':
    case '=': duty_pct = (duty_pct <= 95) ? duty_pct + 5 : 100;  break;
    case '[': if (hz_i > 0) hz_i--;            break;
    case ']': if (hz_i < NLADDER - 1) hz_i++;  break;
    case 'f': slow = !slow;                    break;
    case 'b': force = !force;                  break;
    case 'p':
        Serial.printf("  CTRL %08x  P/D %08x  MODE %08x  pair %08x  cru+F4 %08x\n",
                      (unsigned)sl6806_mmio_read(SL6806_PWM_CHAN(CH)
                                                 + SL6806_PWM_CTRL),
                      (unsigned)sl6806_mmio_read(SL6806_PWM_CHAN(CH)
                                                 + SL6806_PWM_PERIOD_DUTY),
                      (unsigned)sl6806_mmio_read(SL6806_PWM_CHAN(CH)
                                                 + SL6806_PWM_MODE),
                      (unsigned)sl6806_mmio_read(SL6806_PWM_PAIR(CH)),
                      (unsigned)sl6806_mmio_read(SL6806_PWM_CLK_REG));
        return;
    default:
        return;
    }

    apply();
    show();
}
