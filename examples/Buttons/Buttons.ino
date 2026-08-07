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

/*
 * CRU registers that hold gate-shaped values in a cold chip. 0x60..0x78 are
 * the pairs the backlight hunt used; the rest were found by dumping the whole
 * CRU and are new. Sweeping is how the PWM's gate turned up (14a), and it is
 * cheaper than reasoning about a block with no datasheet.
 */
#define CRU_BASE 0x40080000u
static const uint16_t cru_gates[] = {
    0x60, 0x64, 0x68, 0x70, 0x74, 0x78,
    0xA0, 0xB0, 0xDC, 0xEC, 0xFC,
};
#define NCRU ((int)(sizeof cru_gates / sizeof cru_gates[0]))

/*
 * Does the ADC take a write? +0x04 is the config register the vendor's init
 * writes; it reads 0x800 in a cold chip. A bare read cannot tell a gated
 * block from an idle one - that mistake cost three runs on the PWM - so this
 * writes and reads back, then puts it right again.
 */
static bool adc_writable(void)
{
    uint32_t reg = ADC_BASE + ADC_CFG;
    uint32_t saved = sl6806_mmio_read(reg);
    bool ok;

    sl6806_mmio_write(reg, 0x0002A800u);
    ok = (sl6806_mmio_read(reg) == 0x0002A800u);
    sl6806_mmio_write(reg, saved);
    return ok;
}

/*
 * The other register class, and the one every sweep so far has missed.
 *
 * The LCDC needs two things to work from a payload, not one: a gate bit
 * (0x64/0x74 bit 15) *and* a module clock at 0x4008010C, which its driver
 * sets to 0x911. 0x00D44690 shows the shape of those registers plainly -
 * it builds [7:4] from one argument and [11:8] from another, stores, then
 * ORs in bit 0:
 *
 *     bit 0 = enable,  [7:4] = source,  [11:8] = divider
 *
 * and 0x4008011C's 0x31 fits it too: enabled, source 3. In a cold chip
 * +0x104 and +0x110..+0x13C all read 0, which under that reading is a row of
 * *disabled module clocks*. A gate without a module clock is exactly what
 * the PWM looked like in 14a - registers writable, counter stopped - so this
 * is the likeliest thing wrong with both peripherals.
 */
#define CRU_MODCLK_LO 0x100
#define CRU_MODCLK_HI 0x13C

static int hunt_module_clock(void)
{
    /* Enable, with each plausible source, then a couple with a divider. */
    static const uint32_t vals[] = {
        0x001, 0x011, 0x021, 0x031, 0x041, 0x051, 0x061, 0x071,
        0x111, 0x911,
    };
    const int nvals = (int)(sizeof vals / sizeof vals[0]);
    uint16_t off;
    int v;

    for (off = CRU_MODCLK_LO; off <= CRU_MODCLK_HI; off += 4) {
        uint32_t r = CRU_BASE + off;
        uint32_t base = sl6806_mmio_read(r);

        if (base & 1u)
            continue;                 /* already enabled; leave it alone */

        for (v = 0; v < nvals; v++) {
            sl6806_mmio_write(r, vals[v]);
            delayMicroseconds(200);
            if (adc_writable()) {
                Serial.print("    +0x");
                Serial.print(off, HEX);
                Serial.print(" <- 0x");
                Serial.print(vals[v], HEX);
                Serial.println(" OPENS THE ADC - left on");
                return 1;
            }
        }
        sl6806_mmio_write(r, base);
    }
    return 0;
}

/* Set each bit of each candidate in turn; restore unless it worked. */
static int hunt_adc_gate(void)
{
    int i, bit;

    for (i = 0; i < NCRU; i++) {
        uint32_t r = CRU_BASE + cru_gates[i];
        uint32_t base = sl6806_mmio_read(r);

        Serial.print("  +0x");
        Serial.print(cru_gates[i], HEX);
        Serial.print(" = 0x");
        Serial.println(base, HEX);

        for (bit = 0; bit < 32; bit++) {
            if (base & (1u << bit))
                continue;
            sl6806_mmio_write(r, base | (1u << bit));
            delayMicroseconds(200);
            if (adc_writable()) {
                Serial.print("    bit ");
                Serial.print(bit);
                Serial.println(" OPENS THE ADC - left on");
                return 1;
            }
            sl6806_mmio_write(r, base);
        }
    }
    return 0;
}

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

    dump("before:");
    if (adc_writable()) {
        Serial.println("ADC already accepts writes.");
    } else {
        Serial.println("ADC ignores writes - hunting a CRU gate:");
        if (!hunt_adc_gate()) {
            Serial.println("  no gate bit did it; trying module clocks");
            Serial.println("  (CRU +0x100..+0x13C, bit 0 = enable):");
            if (!hunt_module_clock()) {
                Serial.println("  nothing opened it. The ADC is readable but");
                Serial.println("  not writable, same shape as the PWM in 14a.");
            }
        }
    }
    adc_init();
    delay(10);
    dump("after: ");
    Serial.print("writable now: ");
    Serial.println(adc_writable() ? "yes" : "no");

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
