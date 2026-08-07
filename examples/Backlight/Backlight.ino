/*
 * Backlight - drive the PWM the way the stock firmware does, and see.
 *
 * This supersedes examples/BacklightHunt, which was written when the PWM
 * driver was believed unrecoverable and so went looking for a GPIO enable
 * instead. It found nothing, across every pad on the part. The premise was
 * wrong: the registers were in the dump the whole time. See
 * docs/sl6806_re_notes.md 14a and cores/sl6806/sl6806_pwm.h.
 *
 * WHAT IT DOES. Ungates the PWM block, programs channel 3 with the vendor's
 * own control word and period, and steps the duty from 0 to 100% and back so
 * a panel that lights is unmistakable rather than something you have to
 * squint at. Then it tries the other five channels, in case this board wires
 * the backlight somewhere else.
 *
 * At every step it prints what it wrote and reads the register back.
 * THE READBACK IS THE REAL RESULT, and it is worth more than the screen:
 *
 *   - CTRL reads back what was written  ->  0x40084000 is a live peripheral.
 *     If the panel is still dark, the map is addressable and the pad mux is
 *     the next suspect, not this file.
 *   - Everything reads back zero        ->  the block is still gated, or not
 *     there at all. Doubt SL6806_MODCTL_BIT_PWM first.
 *
 * WHILE IT RUNS the display flashes white/black, so a backlight that comes on
 * says something about the LCD driver too: a flashing picture means both
 * halves work, a uniform glow means the backlight is found and the bus driver
 * still has a bug.
 *
 *     make SKETCH=examples/Backlight RUN_MODE=poll upload
 *     tools/sl6806-monitor --tick 3 build/Backlight.sym
 *
 * PACED BY THE HOST. Same lesson BacklightHunt records at length: neither
 * clock on this device can time a dwell. millis() loses SysTick wraps between
 * polls, and counting loop() calls fails because the poll rate has been
 * measured anywhere from 0.3/s to 42/s. So this advances one step per byte
 * from the host, and --tick makes the dwell exact.
 *
 * SAFETY. Nothing here writes flash, and the block being poked is not one USB
 * needs, so the worst case is a wedged device that a replug recovers. That is
 * not true of pad sweeps, which is the other reason to try this first.
 */

#include <Arduino.h>
#include "sl6806_pwm.h"
#include "sl6806_lcdc.h"

static sl6806_color_t band[240 * 8];

/* Channel 3 first, because that is the one the firmware opens. */
static const uint8_t order[] = { 3, 0, 1, 2, 4, 5 };
#define NCHAN ((int)(sizeof order))

/* 0 -> 100 -> 0. A ramp rather than a flash, because a backlight that is on
 * but very dim is easy to miss against a dark panel. */
static const uint8_t ramp[] = { 0, 20, 40, 60, 80, 100, 80, 60, 40, 20 };
#define NRAMP ((int)(sizeof ramp))

static int  idx = -1;      /* index into order[]; -1 = nothing set up yet */
static int  step;          /* index into ramp[]                          */
static bool white;
static bool done;

static void show(const char *what, uint32_t addr)
{
    Serial.print("  ");
    Serial.print(what);
    Serial.print(" @0x");
    Serial.print(addr, HEX);
    Serial.print(" = 0x");
    Serial.println(sl6806_mmio_read(addr), HEX);
}

/* Program one channel exactly as 0x00D99C34 and 0x00D102F4 do. */
static void configure(unsigned ch)
{
    uint32_t base = SL6806_PWM_CHAN(ch);

    /* The vendor writes the bare 0x40 first, then ORs the assembled flags. */
    sl6806_mmio_write(base + SL6806_PWM_CTRL, SL6806_PWM_CTRL_INIT);
    sl6806_mmio_write(base + SL6806_PWM_CTRL,
                      sl6806_mmio_read(base + SL6806_PWM_CTRL) | 0x3Fu);

    sl6806_pwm_set(ch, SL6806_PWM_BL_PERIOD, SL6806_PWM_BL_DUTY(60));
    sl6806_pwm_enable(ch, 1);
    sl6806_pwm_run(ch, 1);

    show("ctrl", base + SL6806_PWM_CTRL);
    show("p/d ", base + SL6806_PWM_PERIOD_DUTY);
    show("mode", base + SL6806_PWM_MODE);
}

static void quiet(unsigned ch)
{
    sl6806_pwm_run(ch, 0);
    sl6806_pwm_enable(ch, 0);
    sl6806_pwm_set(ch, SL6806_PWM_BL_PERIOD, 0);
}

void setup()
{
    uint32_t before, after;

    Serial.begin(115200);
    Serial.println();
    Serial.println("=== SL6806 backlight: PWM at 0x40084000 ===");

    before = sl6806_mmio_read(SL6806_MODCTL_BASE);
    sl6806_pwm_module_enable();
    delay(10);
    after = sl6806_mmio_read(SL6806_MODCTL_BASE);

    Serial.print("modctl 0x");
    Serial.print(before, HEX);
    Serial.print(" -> 0x");
    Serial.println(after, HEX);
    if (before == after)
        Serial.println("WARNING: the gate did not change. Either it is already");
    if (before == after)
        Serial.println("on, or 0x400E0000 bit 2 is not the PWM's gate.");
    Serial.println();

    /* Bring the panel up too, so a backlight that lights shows a picture. */
    if (!Screen.begin(band, 240, 8))
        Serial.println("no panel - the flash below will do nothing");
    else
        Serial.println("lcd bus up; flashing white/black while we try");
    Serial.println();
    Serial.println("one step per host tick; run the monitor with --tick 3");
    Serial.println();
}

void loop()
{
    unsigned ch;

    /* Flip every poll, not every tick, so the picture is lively even while
     * we are waiting for the host. */
    white = !white;
    Screen.fill(white ? SL6806_WHITE : SL6806_BLACK);
    Screen.display();

    if (done)
        return;

    /* Advance only when the host says so. */
    if (Serial.read() < 0)
        return;

    /* Start of a channel: announce, configure, and let the host drain it
     * before anything else happens. */
    if (idx < 0 || step >= NRAMP) {
        if (idx >= 0)
            quiet(order[idx]);

        if (++idx >= NCHAN) {
            done = true;
            Serial.println();
            Serial.println("=== all six channels tried ===");
            Serial.println("If the panel never lit, the ctrl readbacks above");
            Serial.println("are the finding: non-zero means the block is live");
            Serial.println("and the pad mux is next; zero means the gate is.");
            return;
        }

        ch = order[idx];
        Serial.print("channel ");
        Serial.print(ch);
        Serial.print(" at 0x");
        Serial.println(SL6806_PWM_CHAN(ch), HEX);
        configure(ch);
        step = 0;
        return;
    }

    ch = order[idx];
    sl6806_pwm_set(ch, SL6806_PWM_BL_PERIOD, SL6806_PWM_BL_DUTY(ramp[step]));
    Serial.print("  duty ");
    Serial.print(ramp[step]);
    Serial.println("%");
    step++;
}
