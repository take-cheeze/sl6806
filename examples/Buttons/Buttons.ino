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
 * Measured: neither GPIO key moves on a press, and pin 17 sits at a steady 0,
 * so those two are detects of some sort. The buttons are the ladder pair.
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
 * SAFETY. Clocks are only ever turned on, never off, and the ROM's own
 * disable (0x00001CE8) is deliberately not used - switching a clock off under
 * a running peripheral is how you lose USB. Progress is printed every eight
 * ids, so if one of them does wedge the device the log names the range.
 */

#include <Arduino.h>
#include "sl6806_padctl.h"
#include "sl6806_mmio.h"

/* [V] 0x00D994F8, the only MMIO literal in the ADC HAL. */
#define ADC_BASE        0x40096000u
#define ADC_CTRL        0x00        /* [V] init writes 0x80180000 */
#define ADC_CFG         0x04        /* [V] init writes 0x0002A800 */
#define ADC_CHAN(ch)    (0x20u + (uint32_t)(ch) * 0x10u)   /* [V] 0x00D993A0 */

/* [V] kadc constructor 0x00D3DB50: channel 0 is pad 0x00014800, value 0x780. */
#define ADC_KEY_PAD     (0x00014800u | 0x780u)
#define ADC_KEY_CHAN    0

/* [V] The map at 0x0081C02C. */
#define KEY_LEVEL_A     0x0200u
#define KEY_ID_A        0x42
#define KEY_LEVEL_B     0x0E60u
#define KEY_ID_B        0x40

#define CRU_BASE        0x40080000u
#define MOD_BASE_HI     0x400F1000u   /* [V] ids 96..127 live here */
#define NMODULES        128
#define NWORDS          16            /* 0x40096000..0x4009603F */

static uint32_t last[NWORDS];
static bool     have_last;
static int      opened_by = -1;

/*
 * 0x00001C5C, transcribed with a bounded poll. Returns true if the gate
 * acknowledged.
 */
static bool module_enable(unsigned id)
{
    uint32_t base, gate, shadow, bit;
    uint32_t i;

    if (id < 32)        { base = CRU_BASE;    gate = 0x60; shadow = 0x70; }
    else if (id < 64)   { base = CRU_BASE;    gate = 0x64; shadow = 0x74; id -= 32; }
    else if (id < 96)   { base = CRU_BASE;    gate = 0x68; shadow = 0x78; id -= 64; }
    else                { base = MOD_BASE_HI; gate = 0x20; shadow = 0x30; id -= 96; }

    bit = 1u << id;

    sl6806_mmio_write(base + shadow, sl6806_mmio_read(base + shadow) | bit);
    sl6806_mmio_write(base + gate,   sl6806_mmio_read(base + gate)   | bit);

    for (i = 0; i < 100000u; i++)
        if (sl6806_mmio_read(base + gate) & bit)
            return true;
    return false;
}

/*
 * Write and read back. A register that reads zero because it is gated and one
 * that reads zero because it is idle are the same thing from here - that
 * mistake cost four runs across two peripherals.
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

/* 0x00D994EC, transcribed. */
static void adc_init(void)
{
    sl6806_mmio_write(ADC_BASE + 0x10, 0);
    sl6806_mmio_write(ADC_BASE + 0x18, 0);
    sl6806_mmio_write(ADC_BASE + 0x0C, 0);
    sl6806_mmio_write(ADC_BASE + ADC_CFG,  0x0002A800u);
    sl6806_mmio_write(ADC_BASE + ADC_CTRL, 0x80180000u);
}

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

/* Turn on every module id in turn until the ADC answers a write. */
static int hunt_module(void)
{
    unsigned id;

    for (id = 0; id < NMODULES; id++) {
        if ((id & 7) == 0) {
            Serial.print("  ids ");
            Serial.print(id);
            Serial.print("..");
            Serial.print(id + 7);
            Serial.println(" ...");
        }
        module_enable(id);
        delayMicroseconds(200);
        if (adc_writable()) {
            Serial.print("  module id ");
            Serial.print(id);
            Serial.println(" OPENS THE ADC");
            return (int)id;
        }
    }
    return -1;
}

void setup()
{
    Serial.begin(115200);
    Serial.println();
    Serial.println("=== SL6806 buttons: the ADC ladder ===");
    Serial.print("key levels 0x");
    Serial.print(KEY_LEVEL_A, HEX);
    Serial.print(" -> 0x");
    Serial.print(KEY_ID_A, HEX);
    Serial.print(", 0x");
    Serial.print(KEY_LEVEL_B, HEX);
    Serial.print(" -> 0x");
    Serial.println(KEY_ID_B, HEX);

    Serial.print("pad 0x");
    Serial.print(ADC_KEY_PAD, HEX);
    Serial.print(" (bank 1 pin 9 fn 15, analog) -> ");
    Serial.println(sl6806_pad_configure(ADC_KEY_PAD) == 0 ? "ok" : "REFUSED");

    dump("before:");

    if (adc_writable()) {
        Serial.println("ADC already accepts writes.");
    } else {
        Serial.println("ADC ignores writes; enabling module clocks the way");
        Serial.println("the mask ROM does (0x00001C5C), ids 0..127:");
        opened_by = hunt_module();
        if (opened_by < 0)
            Serial.println("  no module id opened it.");
    }

    adc_init();
    delay(10);
    dump("after: ");
    Serial.print("writable now: ");
    Serial.println(adc_writable() ? "yes" : "no");

    Serial.println();
    Serial.println("Words are 0x40096000 +0,4,8,... Press and HOLD a button;");
    Serial.println("any word that changes is printed with its offset.");
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

    if (Serial.read() >= 0) {
        Serial.print("steady: chan0 = 0x");
        Serial.print(sl6806_mmio_read(ADC_BASE + ADC_CHAN(ADC_KEY_CHAN)), HEX);
        Serial.print("   +0x08 = 0x");
        Serial.println(sl6806_mmio_read(ADC_BASE + 0x08), HEX);
    }
}
