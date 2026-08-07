/*
 * PadSweep - the backlight is on a pad nobody has named. Find it, safely.
 *
 * WHY A SWEEP AFTER ALL
 *
 * Seventeen candidates from tools/sl6806-padscan were tried on hardware and
 * none lit the panel. That is not a failure of the method, it explains
 * itself: padscan finds pads the *flash* image configures, and the backlight
 * is driven by the PWM driver, which lives in SRAM and configures its own
 * pad. The enable was never going to be in that list.
 *
 * So the remaining pads have to be tried. The last attempt at that drove all
 * 192 at once, wedged the device off the bus, and - because the console ring
 * is only drained by host polling - destroyed the log that would have named
 * the pad responsible. This is the same idea done in a way that survives.
 *
 * HOW THIS ONE IS DIFFERENT
 *
 *   - One pad at a time, never all at once.
 *   - Host-paced. Each step waits for a byte from sl6806-monitor --tick, so
 *     the dwell is exactly what the host says. Nothing here trusts millis()
 *     or the poll rate; both were measured varying by 100x.
 *   - Announce, return, then act. The pad's name reaches the host before it
 *     is driven, so a pad that kills the link still identifies itself. That
 *     is the property the all-pads version lacked.
 *   - One bank per run, chosen at build time. If a bank wedges the device,
 *     the others are unaffected and the log says which pad did it.
 *   - Pads the stock firmware already configures are skipped: they have been
 *     tried, and re-driving the panel's own bus would only hide the answer.
 *
 * RISK, PLAINLY. These are pads whose function nobody knows. In payload mode
 * nothing writes flash, so the worst case is that the device stops answering
 * and needs a replug - and the last line printed names the pad that did it,
 * which is worth knowing. Do not run this in firmware mode.
 *
 *   make SKETCH=examples/PadSweep RUN_MODE=poll SWEEP_BANK=3
 *   tools/sl6806-monitor --no-sudo --no-input --tick 3 --duration 100 <sym>
 */

#include <Arduino.h>
#include "sl6806_padctl.h"
#include "sl6806_mmio.h"
#include "sl6806_lcdc.h"

#ifndef SWEEP_BANK
#define SWEEP_BANK 3
#endif

/*
 * Which way to drive. The first pass over all six banks drove every pad HIGH
 * and found nothing - but that is only half the search: an enable that is
 * active low, or one with an external pull-up holding it high already, is
 * untouched by driving it high. SWEEP_LEVEL=0 covers the other half.
 */
#ifndef SWEEP_LEVEL
#define SWEEP_LEVEL 1
#endif

static sl6806_color_t band[240 * 8];

/*
 * Pads the stock firmware configures, from tools/sl6806-padscan. Skipped:
 * every one has either been tried already or belongs to the panel's own bus,
 * and driving those would only confuse the result.
 */
static bool known(unsigned bank, unsigned pin)
{
    switch (bank) {
    case 1: return pin <= 8 || pin == 10 || (pin >= 13 && pin <= 17)
                || pin == 20 || (pin >= 22 && pin <= 27) || pin >= 30;
    case 2: return pin == 2;
    case 3: return pin <= 6;
    case 4: return pin <= 15;
    case 5: return pin == 1 || pin == 2;
    default: return false;
    }
}

static int pin = -1;
static bool announced;
static bool white;
static unsigned savedFunc;
static bool holding;

static uint32_t bankBase(void) { return sl6806_pad_bank_base[SWEEP_BANK]; }

static unsigned funcOf(unsigned p)
{
    uint32_t w = sl6806_mmio_read(bankBase() + (p >> 3) * 4);
    return (w >> ((p & 7) * 4)) & 0xFu;
}

void setup()
{
    Serial.begin(115200);
    Serial.println();
    Serial.println("=== SL6806 pad sweep ===");
    Serial.print("bank ");
    Serial.print(SWEEP_BANK);
    Serial.print(", driving ");
    Serial.print(SWEEP_LEVEL ? "HIGH" : "LOW");
    Serial.println(", pads the stock firmware does not configure.");
    Serial.println("Each is named, then driven high on the next tick, then");
    Serial.println("restored. Watch the panel; the last name printed is it.");

    if (!Screen.begin(band, 240, 8))
        Serial.println("WARNING: no panel; the flash will do nothing");
    Serial.println();
}

void loop()
{
    /* Flip every poll so a backlight coming on is unmissable, and so the
     * picture says at the same time whether the LCD driver is right. */
    white = !white;
    Screen.fill(white ? SL6806_WHITE : SL6806_BLACK);
    Screen.display();

    /*
     * Advance on the host's tick - but drain the whole backlog, not one byte.
     * Reading a single byte per loop() call ties the step rate to the poll
     * rate, and when that drops (it varies by 100x on this device) the ticks
     * queue up faster than they are consumed and the sweep crawls: one run
     * managed two pads in 55 seconds.
     */
    if (!Serial.available())
        return;
    while (Serial.available())
        Serial.read();

    if (!announced) {
        int next = pin;

        do {
            if (++next >= 32) {
                Serial.println();
                /* It cannot know whether anything lit; only the person
                 * watching can. Say what actually happened. */
                Serial.println("=== bank complete - every pad driven ===");
                return;
            }
        } while (known(SWEEP_BANK, (unsigned)next));

        Serial.print("bank ");
        Serial.print(SWEEP_BANK);
        Serial.print(" pin ");
        Serial.print(next);
        Serial.print(" (was function ");
        Serial.print(funcOf((unsigned)next));
        Serial.print(") -> driving ");
        Serial.print(SWEEP_LEVEL ? "HIGH" : "LOW");
        Serial.println(" next tick");
        announced = true;
        return;                       /* the host has this line before we act */
    }

    /* Put the previous one back before touching another. */
    if (holding) {
        sl6806_pad_set_func(SL6806_PAD_ID(SWEEP_BANK, pin, 0), savedFunc);
        holding = false;
    }

    do {
        if (++pin >= 32)
            return;
    } while (known(SWEEP_BANK, (unsigned)pin));

    savedFunc = funcOf((unsigned)pin);
    sl6806_pad_configure(SL6806_PAD_ID(SWEEP_BANK, pin, SL6806_PAD_FUNC_OUTPUT)
                         | (SWEEP_LEVEL ? (1u << 6) : 0u));
    holding = true;
    announced = false;
}
