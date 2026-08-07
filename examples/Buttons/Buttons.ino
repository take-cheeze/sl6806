/*
 * Buttons - read the key pads, and find out which one is volume.
 *
 * WHAT THE FIRMWARE SAYS. The stock key manager (0x00D1DA20) opens two key
 * devices and hands each one a map, and both maps are in the SRAM blob, so
 * they read straight out of dump.bin (docs/sl6806_re_notes.md 13):
 *
 *   /dev/key_io   map at 0x0081C044, 16-byte records {pad_id, 0, key_id, 0x101}
 *       0x00018800  bank 1 pin 17  key 0x3E
 *       0x00016000  bank 1 pin 12  key 0x3C
 *
 *   /dev/kadc_ch0 map at 0x0081C02C, 12-byte records {adc_level, key_id, 0}
 *       level 0x0200  key 0x42
 *       level 0x0E60  key 0x40
 *
 * So this board has four keys and a power button: two on GPIO, two on a
 * resistor ladder into an ADC channel, and power through the PMU. **Only the
 * two GPIO keys are readable today** - nothing here has an ADC driver, and
 * 0x00D1DA4A is the only clue where that channel lives. If volume turns out
 * to be one of the ADC keys, this sketch will show nothing for it, and that
 * is itself the answer to which half of the problem to work on next.
 *
 * WHICH KEY IS WHICH is not decided here. The key ids are numbers the vendor's
 * scene framework consumes, and chasing 0x3C and 0x3E through it is a lot of
 * work to learn something one button press settles. So: press a button, watch
 * which pad changes. That is what the log is for.
 *
 * ALSO WATCHED: bank 1 pins 22-27, 30 and 31. 7g records these as going
 * through gpio_config as inputs and calls them "the likely button group" -
 * they are not in either key map, so they are something else, but they cost
 * nothing to include and a board with more buttons than the maps admit would
 * show up here.
 *
 * NOT TOUCHED: bank 1 pins 1-8, which are the panel's QSPI bus, and pin 0,
 * which is the backlight PWM (14a). Reconfiguring those mid-run would be a
 * good way to lose the display or the session.
 *
 *     make SKETCH=examples/Buttons RUN_MODE=poll upload
 *     tools/sl6806-monitor --tick 5 build/Buttons.sym
 *
 * HOLD EACH BUTTON DOWN for a few seconds. loop() runs at the USB poll rate,
 * which has been measured anywhere between 0.3 and 42 times a second, so a
 * quick tap can fall entirely between two samples. This is the same reason
 * every other probe here is paced from the host rather than from millis().
 *
 * SAFETY. Read-only: every pad below is put in function 0, a plain input,
 * with a pull-up. That is the one alternate function PadScope found leaves the
 * input buffer alive, which is why it is used rather than anything cleverer.
 * Nothing drives a pin, so nothing can contend with whatever is out there.
 */

#include <Arduino.h>
#include "sl6806_padctl.h"

#define PULL_UP 8u

typedef struct {
    uint8_t     pin;
    const char *what;
} key_t;

/*
 * The two from /dev/key_io first, then 7g's group. Order matters only for
 * readability of the log.
 */
static const key_t keys[] = {
    { 12, "key_io key 0x3C" },
    { 17, "key_io key 0x3E" },
    { 22, "7g group" },
    { 23, "7g group" },
    { 24, "7g group" },
    { 25, "7g group" },
    { 26, "7g group" },
    { 27, "7g group" },
    { 30, "7g group" },
    { 31, "7g group" },
};
#define NKEYS ((int)(sizeof keys / sizeof keys[0]))

static uint16_t last;
static bool     have_last;

static uint16_t sample(void)
{
    uint16_t v = 0;
    int i;

    for (i = 0; i < NKEYS; i++)
        if (sl6806_pad_read(SL6806_PAD_ID(1, keys[i].pin, SL6806_PAD_FUNC_INPUT)) > 0)
            v |= (uint16_t)(1u << i);
    return v;
}

static void report(uint16_t v, uint16_t changed)
{
    int i;

    for (i = 0; i < NKEYS; i++) {
        Serial.print((v >> i) & 1 ? '1' : '0');
        if (changed & (1u << i))
            Serial.print('*');
        else
            Serial.print(' ');
    }
    Serial.print(" ");
    for (i = 0; i < NKEYS; i++) {
        if (!(changed & (1u << i)))
            continue;
        Serial.print(" pin ");
        Serial.print(keys[i].pin);
        Serial.print(((v >> i) & 1) ? " released(1) " : " PRESSED(0) ");
        Serial.print(keys[i].what);
    }
    Serial.println();
}

void setup()
{
    int i;

    Serial.begin(115200);
    Serial.println();
    Serial.println("=== SL6806 buttons ===");
    Serial.println("Two GPIO keys are known from the firmware's own map:");
    Serial.println("  bank 1 pin 12 = key 0x3C, bank 1 pin 17 = key 0x3E.");
    Serial.println("The other two keys on this board are on an ADC ladder");
    Serial.println("(/dev/kadc_ch0) and cannot be read yet.");
    Serial.println();

    for (i = 0; i < NKEYS; i++) {
        uint32_t id = SL6806_PAD_ID(1, keys[i].pin, SL6806_PAD_FUNC_INPUT) | PULL_UP;

        Serial.print("  pin ");
        Serial.print(keys[i].pin);
        Serial.print(" input+pullup -> ");
        Serial.println(sl6806_pad_configure(id) == 0 ? "ok" : "REFUSED");
    }

    Serial.println();
    Serial.print("columns: ");
    for (i = 0; i < NKEYS; i++) {
        Serial.print(keys[i].pin);
        Serial.print(" ");
    }
    Serial.println();
    Serial.println("A pulled-up input reads 1 idle and 0 while pressed.");
    Serial.println("Hold each button down for a few seconds - loop() runs at");
    Serial.println("the USB poll rate and a tap can fall between samples.");
    Serial.println();
}

void loop()
{
    uint16_t v = sample();
    uint16_t changed = have_last ? (uint16_t)(v ^ last) : 0;

    if (!have_last) {
        Serial.print("idle:   ");
        report(v, 0);
        last = v;
        have_last = true;
        return;
    }

    if (changed) {
        Serial.print("change: ");
        report(v, changed);
        last = v;
        return;
    }

    /* Nothing moved. Print a keepalive only when the host ticks, so a quiet
     * run does not fill the log at the poll rate. */
    if (Serial.read() >= 0) {
        Serial.print("steady: ");
        report(v, 0);
    }
}
