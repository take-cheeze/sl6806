/*
 * Buttons - enable the ADC the way the mask ROM does, then read the keys.
 *
 * THE KEYS, from the dump. The key manager (0x00D1DA20) opens two devices and
 * hands each a map; both maps are in the SRAM blob, so they read straight out
 * of dump.bin (docs/sl6806_re_notes.md 13):
 *
 *   /dev/key_io   0x0081C044   bank 1 pin 17 -> key 0x3E, pin 12 -> key 0x3C
 *   /dev/kadc_ch0 0x0081C02C   level 0x0200 -> key 0x42, 0x0E60 -> key 0x40
 *
 * MEASURED, and now a driver: cores/sl6806/sl6806_adc.c and the key map in
 * variants/p20_player/variant.h. Volume up is key 0x40 at about 0xD58, volume
 * down is key 0x42 at about 0x23, and nothing pressed sits near 0xFEA.
 *
 * The GPIO pair does not move on a button press, and pin 17 sits at a steady
 * 0. A headphone jack does not move them either, so pwm_is_jack_exist is not
 * these two - still unidentified.
 *
 * THE ADC is at 0x40096000 (the only MMIO literal in its HAL, 0x00D994F8),
 * channels 0x10 apart from +0x20, key channel 0 on bank 1 pin 9 function 15.
 * It reads structured values and ignores every write.
 *
 * WHAT FOUR RUNS OF SWEEPING MISSED. The mask ROM has the answer, as a
 * function: 0x00001C5C is module_clock_enable(id), and it says the module
 * space is 128 ids across *four* register pairs, not three:
 *
 *     id  0.. 31   CRU +0x60 gate, +0x70 shadow
 *     id 32.. 63   CRU +0x64 gate, +0x74 shadow
 *     id 64.. 95   CRU +0x68 gate, +0x78 shadow
 *     id 96..127   0x400F1000 +0x20 gate, +0x30 shadow
 *
 * Two things were wrong with every sweep before this. The fourth block was
 * never touched at all - 0x400F1000 is filed in 7c as a storage controller -
 * so 32 module ids were out of reach. And the order matters: the ROM writes
 * the *shadow* first, then the gate, then spins until the gate reads the bit
 * back. Setting both at once and moving on is not the same operation.
 *
 * This reimplements that rather than calling it, for one reason: the ROM's
 * poll is unbounded, and a module id that nothing implements would spin
 * forever inside the boot ROM's USB handler and take the device off the bus.
 * Same algorithm, same order, with a timeout.
 *
 *     make SKETCH=examples/Buttons RUN_MODE=poll upload
 *     tools/sl6806-monitor --tick 5 build/Buttons.sym
 *
 * HOLD EACH BUTTON for a few seconds - loop() runs at the USB poll rate,
 * measured between 0.3 and 42 times a second.
 *
 * SAFETY, rewritten after this sketch wedged the device once.
 *
 * Clocks are only ever turned on, never off; the ROM's disable (0x00001CE8)
 * is deliberately unused, because switching a clock off under a running
 * peripheral is how you lose USB.
 *
 * The wedge was not a dangerous module, it was time. The first version polled
 * each gate 100000 times for an acknowledgement, and every id that nothing
 * implements paid the full count - 128 of those is over a second spent inside
 * the boot ROM's USB handler, which is exactly the failure the notes warn
 * about. The poll is now 200, which is generous for something that answers in
 * a few cycles.
 *
 * And the walk runs a few ids per loop() call rather than all of it in
 * setup(), so the handler keeps getting control back. That also fixes the
 * other half of that failure: the console is a ring in this device's RAM, read
 * over the very link that dies, so a log printed in the same breath as a fatal
 * write never arrives. Printing a window and returning means a wedge is
 * bracketed by output you already have.
 */

#include <Arduino.h>
#include "sl6806_adc.h"
#include "sl6806_padctl.h"
#include "variant.h"

/*
 * The two /dev/key_io pads. They are not the buttons and not the jack, but
 * they are the only two inputs the firmware names, so watching them costs two
 * reads and might yet catch an SD card.
 */
#define KEYIO_PIN_A 12
#define KEYIO_PIN_B 17

static int last_key = -2;
static int last_gpio = -1;

static int gpio_state(void)
{
    int a = sl6806_pad_read(SL6806_PAD_ID(1, KEYIO_PIN_A, SL6806_PAD_FUNC_INPUT));
    int b = sl6806_pad_read(SL6806_PAD_ID(1, KEYIO_PIN_B, SL6806_PAD_FUNC_INPUT));

    return ((a > 0) ? 1 : 0) | ((b > 0) ? 2 : 0);
}

static const char *key_name(int key)
{
    if (key == SL6806_KEY_VOL_UP)   return "VOL_UP";
    if (key == SL6806_KEY_VOL_DOWN) return "VOL_DOWN";
    return "none";
}

void setup()
{
    Serial.begin(115200);
    Serial.println();
    Serial.println("=== SL6806 keys ===");

    Serial.print("pad 0x");
    Serial.print(SL6806_KEY_ADC_PAD, HEX);
    Serial.print(" (bank 1 pin 9, analog) -> ");
    Serial.println(sl6806_pad_configure(SL6806_KEY_ADC_PAD) == 0 ? "ok" : "REFUSED");

    sl6806_pad_configure(SL6806_PAD_ID(1, KEYIO_PIN_A, SL6806_PAD_FUNC_INPUT) | 8u);
    sl6806_pad_configure(SL6806_PAD_ID(1, KEYIO_PIN_B, SL6806_PAD_FUNC_INPUT) | 8u);

    if (!sl6806_adc_begin()) {
        Serial.println("ADC did not come up - module clock refused.");
        Serial.println("See docs/sl6806_re_notes.md 15b; the write order to");
        Serial.println("the gate pair is the thing that usually goes wrong.");
        return;
    }
    sl6806_adc_channel(SL6806_KEY_ADC_CHANNEL, 1);

    Serial.print("ADC up on module ");
    Serial.print(SL6806_ADC_MODULE_ID);
    Serial.print(", channel ");
    Serial.print(SL6806_KEY_ADC_CHANNEL);
    Serial.print(", idle reading 0x");
    Serial.println(sl6806_adc_read(SL6806_KEY_ADC_CHANNEL), HEX);
    Serial.println();
    Serial.println("Press the volume buttons.");
    Serial.println();
}

void loop()
{
    uint32_t raw = sl6806_adc_read(SL6806_KEY_ADC_CHANNEL);
    int key = sl6806_key_decode(raw);
    int g;

    if (key != last_key) {
        if (key == SL6806_KEY_NONE) {
            Serial.print("released          ");
        } else {
            Serial.print(key_name(key));
            Serial.print(" (0x");
            Serial.print(key, HEX);
            Serial.print(") pressed ");
        }
        Serial.print("  adc 0x");
        Serial.println(raw, HEX);
        last_key = key;
    }

    g = gpio_state();
    if (last_gpio >= 0 && g != last_gpio) {
        Serial.print("gpio:   pin ");
        Serial.print(KEYIO_PIN_A);
        Serial.print("=");
        Serial.print(g & 1);
        Serial.print("  pin ");
        Serial.print(KEYIO_PIN_B);
        Serial.print("=");
        Serial.println((g >> 1) & 1);
    }
    last_gpio = g;

    if (Serial.read() >= 0) {
        Serial.print("idle: adc 0x");
        Serial.print(raw, HEX);
        Serial.print("  key ");
        Serial.println(key_name(key));
    }
}
