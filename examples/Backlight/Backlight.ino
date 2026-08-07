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
 *   And bit 8 of the pair register at 0x40084014 is the counter's clock
 *   enable. Nothing in the firmware writes it, so no amount of reading the
 *   vendor's code produces it; it was found by holding each of that
 *   register's 32 reachable settings and watching the panel.
 *
 * THE TRAP THAT COST THE MOST. CTRL bit 28 looks exactly like a busy flag -
 * the vendor's own setter spins on it - and it is set while the backlight is
 * running perfectly. Three separate "the counter never starts" conclusions
 * came from testing it. The panel is the only detector that was ever right.
 *
 *     make SKETCH=examples/Backlight RUN_MODE=poll run
 */

#include <Arduino.h>
#include "sl6806_pwm.h"
#include "sl6806_lcdc.h"

static sl6806_color_t band[240 * 8];

static const uint8_t ramp[] = { 0, 10, 25, 50, 75, 100, 75, 50, 25, 10 };
#define NRAMP ((int)(sizeof ramp))

static int  step;
static bool white;

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

    if (Serial.read() < 0)
        return;

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
