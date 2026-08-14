/*
 * PwmSlow - time how long the update bit takes to retire, in seconds.
 *
 * WHY. examples/PwmSrc finished, went idle, wrote nothing further - and the
 * panel is BLINKING. Trace what it left behind and the reason is plain:
 *
 *     0x400800F4 = 0x11        bit 0 enable AND bit 4 source select
 *     pair register = 0        the force bit is CLEAR
 *     CTRL = 0x7F, MODE = 1, period/duty = 48000/24000
 *
 * The pad is toggling with bit 8 clear, so it is not being held at a level -
 * it is being driven by the counter. THE COUNTER IS RUNNING. It is simply
 * running so slowly that a period is a visible blink.
 *
 * WHICH MAKES EVERY "commits 0/4" IN §32 A TIMESCALE ARTIFACT, NOT A
 * NEGATIVE. sl6806_pwm_commit() polls 1000 iterations - microseconds. At
 * 48000 counts against a 32 kHz-ish source one period is about 1.5 seconds,
 * so the commit times out roughly a million times too early, every time, in
 * every configuration. The witness was sound; its deadline was wrong by six
 * orders of magnitude.
 *
 * And it retires the other claim in the same breath: "bit 4 selects a source
 * that is not running" was read off a 100 ms settle window and four
 * microsecond polls. Bit 4 selects a source that is SLOW.
 *
 * [M] FIRST RESULTS, and they confirm it - once the instrument's own floor is
 * taken out. Every retire time came back an integer multiple of ~115 ms:
 * 1, 3, 5 and 9 of them. 115 ms is not a hardware time, it is the RUN_MODE=poll
 * loop() interval, because the first version of this sketch timed across
 * loop() calls with millis(). So:
 *
 *   clk = 0x11 (slow source): 3, 5 and 9 polls for periods 6000, 12000 and
 *   24000 - it SCALES WITH PERIOD, which is the control this sketch was
 *   built around. Bounding each bucket gives a source of 23.4-26.1 kHz, and
 *   24 MHz / 1024 = 23437 Hz sits inside it.
 *
 *   clk = 0x01 (bit 0 alone): a constant ~115 ms at EVERY period - which is
 *   not a counter, it is the floor. The retire is faster than one poll for
 *   all four periods, so this source is fast: 48000 counts in under 115 ms
 *   means over 417 kHz, and 24 MHz would put it at 2 ms.
 *
 * So THE COUNTER RUNS IN BOTH SETTINGS, and it has all along. Bit 4 selects a
 * slow source and bit 0 alone a fast one. sl6806_pwm_commit()'s 1000-iteration
 * bound is tens of microseconds; even the fast case needs 2 ms, so it timed
 * out everywhere, always, and that uniformity is what made "commits 0/4" look
 * like a clean negative in §32.
 *
 * THE MEASUREMENT, in two tiers, because the range spans microseconds to
 * seconds:
 *
 *   - a tight micros() spin INSIDE one loop() call, bounded at 40 ms, well
 *     under SL6806_POLL_BLOCK_LIMIT_MS so the link is never at risk;
 *   - the across-calls millis() timer only for retires too slow for that, and
 *     its results are printed as `ms*` upper bounds, not as numbers, because
 *     they carry the poll quantisation.
 *
 * A DISCARD COMMIT RUNS FIRST. A commit retires at the end of the period that
 * is CURRENTLY running, so the first one after a reconfigure measures the
 * PREVIOUS row. The first run shows it plainly: the same row gave 1037 ms
 * after a slow configuration and 124 ms after a fast one.
 *
 * THE CONTROL is unchanged and is what makes this a measurement rather than
 * another hopeful reading: **halve the period and the time must halve.**
 *
 *     make SKETCH=examples/PwmSlow RUN_MODE=poll run
 *
 * The panel is left free to follow the counter throughout - pair bit 8 stays
 * clear - so a blink you can see and a retire time that matches it are two
 * independent readings of the same number.
 *
 * Nothing blocks beyond 40 ms; the slow wait is a state polled from loop().
 */

#include <Arduino.h>
#include "sl6806_pwm.h"
#include "sl6806_module.h"
#include "sl6806_mmio.h"
#include "sl6806_padctl.h"

#define CH 3
#define TIMEOUT_MS 20000u

struct row {
    uint32_t clk;        /* what to leave in 0x400800F4 */
    uint16_t period;
};

/* Both clock settings against four periods. The periods are what test the
 * hypothesis; the clock settings are what identify the source. */
static const struct row rows[] = {
    { SL6806_PWM_CLK_ENABLE | SL6806_PWM_CLK_SRC_HIGH, 48000 },
    { SL6806_PWM_CLK_ENABLE | SL6806_PWM_CLK_SRC_HIGH, 24000 },
    { SL6806_PWM_CLK_ENABLE | SL6806_PWM_CLK_SRC_HIGH, 12000 },
    { SL6806_PWM_CLK_ENABLE | SL6806_PWM_CLK_SRC_HIGH,  6000 },
    { SL6806_PWM_CLK_ENABLE,                           48000 },
    { SL6806_PWM_CLK_ENABLE,                           24000 },
    { SL6806_PWM_CLK_ENABLE,                           12000 },
    { SL6806_PWM_CLK_ENABLE,                            6000 },
};
#define NROW ((int)(sizeof rows / sizeof rows[0]))

static int      idx = -1;
static bool     waiting;
static uint32_t t0, deadline;

static void apply(const struct row *r)
{
    uint32_t chan = SL6806_PWM_CHAN(CH);

    sl6806_module_enable(SL6806_PWM_MODULE_ID);
    sl6806_mmio_clr(SL6806_PWM_CLK_REG,
                    SL6806_PWM_CLK_ENABLE | SL6806_PWM_CLK_SRC_HIGH);
    sl6806_mmio_set(SL6806_PWM_CLK_REG, r->clk);
    sl6806_pad_configure(SL6806_PWM_BL_PAD);

    sl6806_mmio_write(chan + SL6806_PWM_CTRL, SL6806_PWM_CTRL_INIT);
    sl6806_mmio_write(chan + SL6806_PWM_CTRL, SL6806_PWM_CTRL_INIT | 0x3Fu);

    /* Bit 8 clear: let the counter have the pad, so the blink is the witness
     * the eye can check the number against. */
    sl6806_mmio_write(SL6806_PWM_PAIR(CH), 0);

    sl6806_pwm_set(CH, r->period, (uint16_t)(r->period / 2));
    sl6806_mmio_write(chan + SL6806_PWM_MODE, 0);
    sl6806_mmio_write(chan + SL6806_PWM_MODE, SL6806_PWM_MODE_ENABLE);
}

void setup()
{
    Serial.begin(115200);
    Serial.println();
    Serial.println("=== SL6806 PWM: how long does a commit take, in SECONDS? ===");
    Serial.printf("0x400800F4 on entry: %08x   pair: %08x\n",
                  (unsigned)sl6806_mmio_read(SL6806_PWM_CLK_REG),
                  (unsigned)sl6806_mmio_read(SL6806_PWM_PAIR(CH)));
    Serial.println("if the retire time halves when the period halves, the");
    Serial.println("counter is real and f = period / retire");
    Serial.println("us rows are measured in one loop() call; ms* rows are");
    Serial.println("quantised to the host poll and are upper bounds only");
    Serial.println("'s' to start, 'q' to stop");
}

void loop()
{
    int key = Serial.read();
    uint32_t ctrl_addr = SL6806_PWM_CHAN(CH) + SL6806_PWM_CTRL;

    if (key == 's') {
        idx = 0; waiting = false;
        Serial.println("\n clk        period   retire      implied source");
    } else if (key == 'q') {
        idx = -1;
        Serial.println("stopped.");
    }

    if (idx < 0)
        return;

    if (idx >= NROW) {
        Serial.println("\ndone. Compare the implied-source column: if the four");
        Serial.println("rows of a clock setting agree, that is the frequency,");
        Serial.println("and the counter has been running all along.");
        idx = -1;
        return;
    }

    if (!waiting) {
        uint32_t us0, us;

        apply(&rows[idx]);

        /*
         * A DISCARD COMMIT FIRST. A commit retires at the end of the period
         * that is CURRENTLY running, so the first one after a reconfigure
         * measures the previous row, not this one. That is visible in the
         * first run's data: the same row gave 1037 ms following a slow
         * configuration and 124 ms following a fast one. Flush it, then time
         * the second.
         */
        sl6806_mmio_set(ctrl_addr, SL6806_PWM_CTRL_UPDATE);
        us0 = micros();
        while ((sl6806_mmio_read(ctrl_addr) & SL6806_PWM_CTRL_UPDATE)
               && micros() - us0 < 40000u)
            ;

        /*
         * MICROSECOND RESOLUTION, INSIDE ONE loop() CALL.
         *
         * [!] The first version timed across loop() calls with millis(), and
         * in RUN_MODE=poll loop() only advances when the host polls - about
         * every 115 ms here. Every result came back an integer multiple of
         * that: 1, 3, 5 and 9 polls. The clk = 0x01 rows read a constant
         * ~115 ms at every period, which is not a counter, it is the floor of
         * the instrument. A fast clock retires 48000 counts in about 2 ms and
         * was unresolvable.
         *
         * So spin here, bounded well under SL6806_POLL_BLOCK_LIMIT_MS so the
         * USB link is never at risk, and fall back to the across-calls timer
         * only for retires too slow to catch this way.
         */
        sl6806_mmio_set(ctrl_addr, SL6806_PWM_CTRL_UPDATE);
        us0 = micros();
        while ((sl6806_mmio_read(ctrl_addr) & SL6806_PWM_CTRL_UPDATE)
               && (us = micros() - us0) < 40000u)
            ;

        if (!(sl6806_mmio_read(ctrl_addr) & SL6806_PWM_CTRL_UPDATE)) {
            us = micros() - us0;
            Serial.printf(" %08x   %5u   %6lu us   %lu Hz   CTRL %08x\n",
                          (unsigned)rows[idx].clk, rows[idx].period,
                          (unsigned long)us,
                          us ? (unsigned long)(rows[idx].period * 1000000ULL / us)
                             : 0UL,
                          (unsigned)sl6806_mmio_read(ctrl_addr));
            idx++;
            return;
        }

        /* Too slow for the tight poll; hand it to the coarse one. */
        t0 = millis();
        deadline = t0 + TIMEOUT_MS;
        waiting = true;
        return;
    }

    {
        uint32_t ctrl = sl6806_mmio_read(ctrl_addr);
        uint32_t el;

        if (ctrl & SL6806_PWM_CTRL_UPDATE) {
            if ((int32_t)(millis() - deadline) < 0)
                return;                       /* still waiting, not blocking */
            Serial.printf(" %08x   %5u   TIMEOUT >%us   -\n",
                          (unsigned)rows[idx].clk, rows[idx].period,
                          (unsigned)(TIMEOUT_MS / 1000));
            waiting = false;
            idx++;
            return;
        }

        el = millis() - t0;
        /* Coarse path: quantised to the host's poll interval, so the true
         * time is somewhere in (el - poll, el]. Reported as a bound, not a
         * number - pretending otherwise is what produced the first run's
         * spurious "implied source" column. */
        Serial.printf(" %08x   %5u   %6u ms*  <=%lu Hz  (poll-quantised)"
                      "   CTRL %08x\n",
                      (unsigned)rows[idx].clk, rows[idx].period, (unsigned)el,
                      (unsigned long)(el ? (rows[idx].period * 1000UL / el) : 0UL),
                      (unsigned)ctrl);
        waiting = false;
        idx++;
    }
}
