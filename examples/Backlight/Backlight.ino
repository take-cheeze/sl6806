/*
 * Backlight - turn the panel's backlight on and fade it.
 *
 * This is what twelve hardware runs were for. The whole bring-up is now one
 * call; what follows is only the story, because the story is the useful part
 * if it ever breaks.
 *
 * WHAT IT TAKES, and each of these cost at least a run:
 *
 *   The PWM is at 0x40084000, the backlight is channel 3, and its module id
 *   is 68 - but a module clock is only enabled by writing the shadow
 *   register, then the gate, then waiting for the gate to read the bit back.
 *   Writing both at once leaves it dead. See sl6806_module.h.
 *
 *   The output pin is bank 1 pin 0 on alternate function 4. examples/
 *   BacklightHunt spent a lot of hardware time driving pads as plain outputs,
 *   which is a different operation, and skipped bank 1's low pins as "the
 *   panel's bus" - true of pins 1-8, not of pin 0.
 *
 *   And bit 8 of the pair register at 0x40084014 is what drives the pad.
 *   Nothing in the firmware writes it, so no amount of reading the vendor's
 *   code produces it; it was found by holding each of that register's 32
 *   reachable settings and watching the panel. [M] It was removed from the
 *   driver on 2026-08-14 on a theory that lasted an afternoon, and removing
 *   it turns the backlight off. It is back. Do not remove it.
 *
 * WHAT IT STILL CANNOT DO IS DIM, and the reason is now measured rather than
 * guessed at: the counter does not run in any configuration anyone has found.
 * examples/PwmClock walks the clock and the force bit together and reports a
 * commit witness - retiring CTRL bit 8 needs a period boundary - which comes
 * back 0/4 every time.
 *
 * THE TRAP THAT COST THE MOST IS STILL CTRL BIT 28, in both directions. This
 * file used to call it a liar; then, briefly, an authority. It is neither. It
 * clears when romclk 39 (0x400800F4 bit 0) is enabled and is set otherwise,
 * so it reports something like "the block has a clock" - and the panel is
 * DARK in one of the configurations where it says "running". Do not conclude
 * anything from it that a witness observing an effect has not also said.
 *
 * PRESS 'h' IN THE MONITOR to walk the 128 module ids. [M] §14a's walk was a
 * correct negative about bit 28, which is not the same question as what
 * starts the counter, so it is worth re-running against a better detector.
 * Anything is only ever switched on, never off.
 *
 *     make SKETCH=examples/Backlight RUN_MODE=poll run
 */

#include <Arduino.h>
#include "sl6806_pwm.h"
#include "sl6806_module.h"
#include "sl6806_lcdc.h"

static sl6806_color_t band[240 * 8];

static const uint8_t ramp[] = { 0, 10, 25, 50, 75, 100, 75, 50, 25, 10 };
#define NRAMP ((int)(sizeof ramp))

static int  step;
static bool white;
static int  hunt_id = -1;      /* -1 = not hunting */

void setup()
{
    Serial.begin(115200);
    Serial.println();
    Serial.println("=== SL6806 backlight ===");

    if (!sl6806_backlight_begin(100)) {
        Serial.println("backlight did not come up - the PWM's module clock");
        Serial.println("refused. See docs/sl6806_re_notes.md 14a.");
        return;
    }
    Serial.println("backlight on at 100%.");

    if (!Screen.begin(band, 240, 8))
        Serial.println("no panel - the backlight is still on");

    Serial.println("fading, one step per tick; run the monitor with --tick 2");
    Serial.println();
}

void loop()
{
    /* Flash the panel so the backlight and the display are both visible. */
    white = !white;
    Screen.fill(white ? SL6806_WHITE : SL6806_BLACK);
    Screen.display();

    {
        int c = Serial.read();

        if (c < 0)
            return;
        if (c == 'h' && hunt_id < 0) {
            hunt_id = 0;
            Serial.println();
            Serial.println("hunting the counter clock; 8 module ids per tick");
        }
    }

    if (hunt_id >= 0) {
        int end = hunt_id + 8, id;

        if (hunt_id >= 128) {
            Serial.println("  no module id started the counter (valid test");
            Serial.println("  this time: the block never ticked)");
            hunt_id = -1;
            return;
        }
        Serial.print("  ids ");
        Serial.print(hunt_id);
        Serial.print("..");
        Serial.print(end - 1);
        Serial.print(": ");
        for (id = hunt_id; id < end && id < 128; id++) {
            sl6806_module_enable((unsigned)id);
            if (sl6806_pwm_counter_ticking(3)) {
                Serial.print("id ");
                Serial.print(id);
                Serial.println(" TICKS - the counter is running");
                hunt_id = -1;
                return;
            }
        }
        Serial.println("no tick");
        hunt_id = end;
        return;
    }

    sl6806_backlight_set(ramp[step]);
    Serial.print("  brightness ");
    Serial.print(ramp[step]);
    Serial.print("%   p/d 0x");
    Serial.print(sl6806_mmio_read(SL6806_PWM_CHAN(3)
                                  + SL6806_PWM_PERIOD_DUTY), HEX);
    Serial.print("  ctrl 0x");
    Serial.println(sl6806_mmio_read(SL6806_PWM_CHAN(3)
                                    + SL6806_PWM_CTRL), HEX);
    step = (step + 1) % NRAMP;
}
