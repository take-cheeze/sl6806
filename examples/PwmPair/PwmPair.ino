/*
 * PwmPair - sweep the pair register's [7:0], the field nobody has ever set.
 *
 * WHY THIS IS THE LEAD. examples/PwmSrc censused the pair register by writing
 * all ones, and it retained 0x1FF - bits [8:0], nine of them. sl6806_pwm.h has
 * said for a year that the writable mask is 0x10F, and that reading was taken
 * by writing 0x3F0F, which does not set bits [7:4] at all. So the old
 * measurement was consistent and blind: **[7:4] exist and nothing has ever
 * written them.**
 *
 * Now put that next to the vendor's own accessor. 0x00811EC0 is
 *
 *     [pair] = src | (div << 8)
 *
 * and if `src` is a byte - which is what a source select looks like, and what
 * the argument register holds - then src is exactly [7:0] and the "divider"
 * is the single bit 8 that is implemented above it. That is a perfect fit for
 * the nine bits the hardware just reported.
 *
 * AND WE HAVE ONLY EVER WRITTEN src = 0. Every run in §32, and every run of
 * the 2026-08-07 sweep that found bit 8, set bit 8 with [7:0] left at zero.
 * If [7:0] selects which clock the pair counts, then zero plausibly selects
 * none, and a counter with no clock is precisely and entirely what has been
 * measured for a year: registers accept everything, CTRL bit 28 clears
 * because the block's own gate is open, no commit retires, no event fires,
 * and the pad only ever answers bit 8 because bit 8 drives it directly.
 *
 * This also retires the last confusion about bit 8. It is not a clock enable
 * and it is not an output force in the sense of overriding a waveform - it is
 * `div`, and with src = 0 there is nothing to divide, so it behaves as a
 * plain output level. That is why the panel tracks it exactly.
 *
 * THE SWEEP. 256 values of [7:0], bit 8 CLEAR, so the pad is not driven by
 * bit 8 and anything that reaches it came from the counter. Detector is
 * sl6806_pwm_commit() - retiring the update bit needs a period boundary,
 * which is the one thing a stopped counter cannot fake. On a hit the sweep
 * stops and ramps the duty so the panel can confirm it.
 *
 *     make SKETCH=examples/PwmPair RUN_MODE=poll run
 *
 * Then 's'. Expect the panel dark until something works.
 *
 * Two phases per row, and the wait between them is a state rather than a
 * comment - examples/PwmMode's first sweep produced 168 void negatives by
 * measuring inside the settling window, and the fix is not to write the wait
 * down but to wait.
 */

#include <Arduino.h>
#include "sl6806_pwm.h"
#include "sl6806_module.h"
#include "sl6806_mmio.h"
#include "sl6806_padctl.h"

#define CH 3
#define NSRC 256

static int      idx = -1;
static int      hits;
static bool     paused, settling, settled;
static uint32_t due, settle_deadline;
static int      ramp_i = -1;

static void apply(unsigned src)
{
    uint32_t chan = SL6806_PWM_CHAN(CH);

    sl6806_module_enable(SL6806_PWM_MODULE_ID);
    sl6806_mmio_clr(SL6806_PWM_CLK_REG,
                    SL6806_PWM_CLK_ENABLE | SL6806_PWM_CLK_SRC_HIGH);
    sl6806_pwm_clock_begin(0);
    sl6806_pad_configure(SL6806_PWM_BL_PAD);

    /* The vendor's order: CTRL, then the rest. */
    sl6806_mmio_write(chan + SL6806_PWM_CTRL, SL6806_PWM_CTRL_INIT);
    sl6806_mmio_write(chan + SL6806_PWM_CTRL, SL6806_PWM_CTRL_INIT | 0x3Fu);

    /* [7:0] = src, bit 8 clear. Written whole, not OR'd: leaving a previous
     * row's bits set would make a hit unattributable, and §32 lost a reading
     * to exactly that. */
    sl6806_mmio_write(SL6806_PWM_PAIR(CH), src & 0xFFu);

    sl6806_pwm_set(CH, SL6806_PWM_BL_PERIOD, SL6806_PWM_BL_DUTY(50));
    sl6806_mmio_write(chan + SL6806_PWM_MODE, 0);
    sl6806_mmio_write(chan + SL6806_PWM_MODE, SL6806_PWM_MODE_ENABLE);
}

void setup()
{
    Serial.begin(115200);
    Serial.println();
    Serial.println("=== SL6806 PWM: sweeping the pair register's [7:0] ===");
    Serial.println("256 values, bit 8 held CLEAR, detector is the commit");
    Serial.printf("pair on entry: %08x   (implemented mask is 0x1FF)\n",
                  (unsigned)sl6806_mmio_read(SL6806_PWM_PAIR(CH)));
    Serial.println("'s' start, 'c' continue after a hit, 'q' stop");
}

void loop()
{
    int key = Serial.read();
    uint32_t chan = SL6806_PWM_CHAN(CH);

    if (key == 's') {
        idx = 0; hits = 0; paused = false; ramp_i = -1; settling = false;
        due = millis();
        Serial.println("\nsweeping...");
    } else if (key == 'c') {
        paused = false; ramp_i = -1;
    } else if (key == 'q') {
        idx = -1; ramp_i = -1;
        Serial.println("stopped.");
    }

    if (idx < 0 || paused || (int32_t)(millis() - due) < 0)
        return;

    if (ramp_i >= 0) {
        sl6806_backlight_set((unsigned)((ramp_i % 20) * 5));
        if (++ramp_i > 80) {
            ramp_i = -1; paused = true;
            Serial.println("     ramp done. 'c' to continue.");
        } else {
            due = millis() + 40;
        }
        return;
    }

    if (idx >= NSRC) {
        Serial.printf("\ndone. %d hit(s) in %d source values.\n", hits, NSRC);
        if (!hits)
            Serial.println("[7:0] is not a clock source select, or not one"
                           " that is running.");
        idx = -1;
        return;
    }

    if (settling) {
        uint32_t c = sl6806_mmio_read(chan + SL6806_PWM_CTRL);
        int commits = 0;

        if (!(c & SL6806_PWM_CTRL_NOCLK))
            settled = true;
        else if ((int32_t)(millis() - settle_deadline) < 0)
            return;
        settling = false;

        for (int k = 0; k < 4; k++)
            commits += sl6806_pwm_commit(CH);

        if (commits || !settled || (idx % 32) == 0)
            Serial.printf("%s src %3d (0x%02x) -> commits %d/4  CTRL %08x"
                          "  pair %08x  clock %s\n",
                          commits ? ">>> HIT" : (settled ? "       " : "?? NOCLK"),
                          idx, idx, commits,
                          (unsigned)sl6806_mmio_read(chan + SL6806_PWM_CTRL),
                          (unsigned)sl6806_mmio_read(SL6806_PWM_PAIR(CH)),
                          settled ? "settled" : "NEVER SETTLED");

        if (commits) {
            hits++;
            Serial.println("     ramping duty 0 -> 100 -> 0; WATCH THE PANEL");
            ramp_i = 0;
        }
        idx++;
        return;
    }

    apply((unsigned)idx);
    settling = true;
    settled  = false;
    settle_deadline = millis() + 100;
}
