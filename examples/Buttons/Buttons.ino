/*
 * Buttons - the keys are on an ADC ladder, so watch the ADC.
 *
 * FIRST RUN SETTLED THE EASY HALF. The two GPIO keys from /dev/key_io do not
 * move when a button is pressed: bank 1 pin 12 sits at 1 and pin 17 sits at a
 * steady 0, which matches 7g's note that pin 17 is externally driven. Nor did
 * any of 7g's "likely button group" (pins 22-27, 30, 31) move. So the user
 * buttons are the other two keys - the ones on the resistor ladder.
 *
 * WHAT THE FIRMWARE SAYS. The key manager (0x00D1DA20) opens two key devices,
 * each with a map, and both maps are in the SRAM blob so they read straight
 * out of dump.bin (docs/sl6806_re_notes.md 13):
 *
 *   /dev/key_io   0x0081C044  {pad_id, 0, key_id, 0x101}
 *       bank 1 pin 17 -> key 0x3E,  bank 1 pin 12 -> key 0x3C     (not buttons)
 *   /dev/kadc_ch0 0x0081C02C  {adc_level, key_id, 0}
 *       level 0x0200 -> key 0x42,  level 0x0E60 -> key 0x40       (these)
 *
 * Two levels, two keys. On a player whose UI is a touchscreen, two physical
 * buttons either side of power is exactly what you would expect, so these are
 * the volume pair unless the log says otherwise.
 *
 * THE ADC IS AT 0x40096000, found from the only literal in its HAL
 * (0x00D994F8), and - unlike the PWM that ate the last seven runs - **it is
 * already alive**. In bootloader mode, with nothing set up, it reads
 * structured non-zero values: +0x00 = 0x00100000, +0x04 = 0x00000800,
 * +0x08 = 0x000001E0, +0x20 = 0x10, +0x30 = 0x10. No clock hunt required.
 *
 * Channels are 0x10 apart starting at +0x20 (0x00D993A0 dispatches ten of them
 * that way), and this board's key channel is 0, on bank 1 pin 9 - the kadc
 * constructor stores pad id 0x00014800 with pad value 0x780, which is function
 * 15, the analog setting.
 *
 * WHICH REGISTER HOLDS THE CONVERSION is not established, so this does not
 * guess: it dumps the whole block every poll and prints any word that changed.
 * Press a button and the log names the register and the value. That is both
 * the measurement and the identification, and it cannot be wrong the way
 * picking a likely-looking offset can.
 *
 *     make SKETCH=examples/Buttons RUN_MODE=poll upload
 *     tools/sl6806-monitor --tick 5 build/Buttons.sym
 *
 * HOLD EACH BUTTON for a few seconds. loop() runs at the USB poll rate, which
 * has been measured between 0.3 and 42 times a second, so a tap can fall
 * between two samples.
 *
 * SAFETY. The only writes are the ADC's own init sequence, transcribed from
 * 0x00D994EC, and one pad put into the analog function the vendor picks for
 * it. Nothing drives a pin. Nothing writes flash.
 */

#include <Arduino.h>
#include "sl6806_padctl.h"
#include "sl6806_mmio.h"

/* [V] From the only MMIO literal in the ADC HAL, at 0x00D994F8. */
#define ADC_BASE        0x40096000u
#define ADC_CTRL        0x00        /* [V] init writes 0x80180000  */
#define ADC_CFG         0x04        /* [V] init writes 0x0002A800  */
#define ADC_CHAN(ch)    (0x20u + (uint32_t)(ch) * 0x10u)   /* [V] 0x00D993A0 */

/* [V] kadc constructor 0x00D3DB50: channel 0 is pad 0x00014800 with pad value
 * 0x780, i.e. bank 1 pin 9 on function 15. */
#define ADC_KEY_PAD     (0x00014800u | 0x780u)
#define ADC_KEY_CHAN    0

/* [V] The map at 0x0081C02C. */
#define KEY_LEVEL_A     0x0200u
#define KEY_ID_A        0x42
#define KEY_LEVEL_B     0x0E60u
#define KEY_ID_B        0x40

#define NWORDS 16                   /* 0x40096000..0x4009603F */

static uint32_t last[NWORDS];
static bool     have_last;

static void dump(const char *tag)
{
    int i;

    Serial.print(tag);
    for (i = 0; i < NWORDS; i++) {
        Serial.print(" ");
        Serial.print(sl6806_mmio_read(ADC_BASE + (uint32_t)i * 4), HEX);
    }
    Serial.println();
}

/* 0x00D994EC, transcribed. The vendor guards it with a "already inited"
 * pointer; there is nothing to guard here, and re-running it is harmless. */
static void adc_init(void)
{
    sl6806_mmio_write(ADC_BASE + 0x10, 0);
    sl6806_mmio_write(ADC_BASE + 0x18, 0);
    sl6806_mmio_write(ADC_BASE + 0x0C, 0);
    sl6806_mmio_write(ADC_BASE + ADC_CFG,  0x0002A800u);
    sl6806_mmio_write(ADC_BASE + ADC_CTRL, 0x80180000u);
}

void setup()
{
    Serial.begin(115200);
    Serial.println();
    Serial.println("=== SL6806 buttons: the ADC ladder ===");
    Serial.println("The GPIO keys do not move on a press, so the buttons are");
    Serial.println("the two on /dev/kadc_ch0. Levels from the vendor's map:");
    Serial.print("  0x");
    Serial.print(KEY_LEVEL_A, HEX);
    Serial.print(" -> key 0x");
    Serial.print(KEY_ID_A, HEX);
    Serial.print("    0x");
    Serial.print(KEY_LEVEL_B, HEX);
    Serial.print(" -> key 0x");
    Serial.println(KEY_ID_B, HEX);
    Serial.println();

    Serial.print("pad 0x");
    Serial.print(ADC_KEY_PAD, HEX);
    Serial.print(" (bank 1 pin 9 fn 15, analog) -> ");
    Serial.println(sl6806_pad_configure(ADC_KEY_PAD) == 0 ? "ok" : "REFUSED");

    dump("before init:");
    adc_init();
    delay(10);
    dump("after  init:");

    Serial.println();
    Serial.println("Words are 0x40096000 +0,4,8,... Press and HOLD a button;");
    Serial.println("any word that changes is printed with its index.");
    Serial.println();
}

void loop()
{
    uint32_t now[NWORDS];
    int i, changes = 0;

    for (i = 0; i < NWORDS; i++)
        now[i] = sl6806_mmio_read(ADC_BASE + (uint32_t)i * 4);

    if (!have_last) {
        for (i = 0; i < NWORDS; i++)
            last[i] = now[i];
        have_last = true;
        return;
    }

    for (i = 0; i < NWORDS; i++) {
        if (now[i] == last[i])
            continue;
        if (!changes++)
            Serial.print("change:");
        Serial.print(" +0x");
        Serial.print(i * 4, HEX);
        Serial.print(" 0x");
        Serial.print(last[i], HEX);
        Serial.print("->0x");
        Serial.print(now[i], HEX);
        last[i] = now[i];
    }
    if (changes) {
        Serial.println();
        return;
    }

    /* Quiet. Print the channel word only when the host ticks, so a still log
     * does not scroll at the poll rate. */
    if (Serial.read() >= 0) {
        Serial.print("steady: chan");
        Serial.print(ADC_KEY_CHAN);
        Serial.print(" @+0x");
        Serial.print(ADC_CHAN(ADC_KEY_CHAN), HEX);
        Serial.print(" = 0x");
        Serial.print(sl6806_mmio_read(ADC_BASE + ADC_CHAN(ADC_KEY_CHAN)), HEX);
        Serial.print("   +0x08 = 0x");
        Serial.println(sl6806_mmio_read(ADC_BASE + 0x08), HEX);
    }
}
