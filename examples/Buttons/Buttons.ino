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

/*
 * [V, MEASURED] The ADC's module id, found by walking 0..127 (2026-08-07).
 * 84 lands in the third pair: CRU +0x68 gate, +0x78 shadow, bit 20.
 *
 * An earlier sweep tried that exact bit and failed, which is the lesson worth
 * keeping: it wrote gate and shadow together and never waited for the
 * acknowledgement. Shadow first, then gate, then poll - the order is the
 * operation.
 */
#define ADC_MODULE_ID   84

#define CRU_BASE        0x40080000u
#define MOD_BASE_HI     0x400F1000u   /* [V] ids 96..127 live here */
#define NMODULES        128
#define NWORDS          16            /* 0x40096000..0x4009603F */

static uint32_t last[NWORDS];
static bool     have_last;
static int      last_gpio = -1;

/*
 * The two /dev/key_io pads. They do not move on a button press, but the
 * firmware also has pwm_is_jack_exist and pwm_is_sd_exist, so a headphone or
 * a card may well move them - and watching them costs two reads. The previous
 * build dropped these, which is why a jack test against it measured nothing.
 */
#define KEYIO_PIN_A 12
#define KEYIO_PIN_B 17

static int gpio_state(void)
{
    int a = sl6806_pad_read(SL6806_PAD_ID(1, KEYIO_PIN_A, SL6806_PAD_FUNC_INPUT));
    int b = sl6806_pad_read(SL6806_PAD_ID(1, KEYIO_PIN_B, SL6806_PAD_FUNC_INPUT));

    return ((a > 0) ? 1 : 0) | ((b > 0) ? 2 : 0);
}
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

    /*
     * SHORT on purpose. An acknowledgement is a few cycles of hardware; a
     * long poll only costs time when the id is one nothing implements, and
     * every one of those is paid 128 times over. The first version polled
     * 100000 times and spent over a second inside the boot ROM's USB handler,
     * which is precisely how you take the device off the bus.
     */
    for (i = 0; i < 200u; i++)
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

/*
 * Turn a channel on. adc_init zeroes +0x00's low bits and +0x0C, and nothing
 * else ever sets them, which is why the block came up configured but idle.
 *   0x00D994BC: [base+0x00] bit ch  - channel enable
 *   0x00D9948C: [base+0x0C] bit ch
 */
static void adc_start(unsigned ch)
{
    sl6806_mmio_write(ADC_BASE + 0x00,
                      sl6806_mmio_read(ADC_BASE + 0x00) | (1u << ch));
    sl6806_mmio_write(ADC_BASE + 0x0C,
                      sl6806_mmio_read(ADC_BASE + 0x0C) | (1u << ch));
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

/*
 * One window of module ids per loop() call, not the whole walk in setup().
 *
 * Two reasons, both learned by wedging the device. The USB handler must get
 * control back promptly, and a walk that runs to completion inside setup()
 * does not give it back. And the log has to reach the host *as it goes*: the
 * console is a ring in this device's RAM, read over the very link that dies,
 * so anything printed in the same breath as the fatal write is lost. Printing
 * a window, returning, and printing the next means a wedge is bracketed by
 * what already arrived.
 */
#define WINDOW 4
static unsigned next_id;

/* Returns 1 when the ADC opens, -1 when the walk is done, 0 while running. */
static int hunt_step(void)
{
    unsigned end = next_id + WINDOW;
    unsigned id;

    if (next_id >= NMODULES)
        return -1;
    if (end > NMODULES)
        end = NMODULES;

    Serial.print("  ids ");
    Serial.print(next_id);
    Serial.print("..");
    Serial.print(end - 1);
    Serial.print(": ");

    for (id = next_id; id < end; id++) {
        module_enable(id);
        if (adc_writable()) {
            Serial.print("id ");
            Serial.print(id);
            Serial.println(" OPENS THE ADC");
            next_id = NMODULES;
            return 1;
        }
    }
    Serial.println("no");
    next_id = end;
    return 0;
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

    /* The two key_io pads, as pulled-up inputs, so jack/SD insertion shows. */
    sl6806_pad_configure(SL6806_PAD_ID(1, KEYIO_PIN_A, SL6806_PAD_FUNC_INPUT) | 8u);
    sl6806_pad_configure(SL6806_PAD_ID(1, KEYIO_PIN_B, SL6806_PAD_FUNC_INPUT) | 8u);

    dump("before:");

    /* The id is known now; only fall back to walking if this board differs. */
    module_enable(ADC_MODULE_ID);
    Serial.print("module id ");
    Serial.print(ADC_MODULE_ID);
    Serial.print(" (CRU +0x68 bit 20) -> ADC writable: ");
    Serial.println(adc_writable() ? "yes" : "no");

    if (adc_writable()) {
        next_id = NMODULES;
        opened_by = -2;              /* nothing to hunt */
    } else {
        Serial.println("ADC ignores writes. Enabling module clocks the way the");
        Serial.println("mask ROM does (0x00001C5C), ids 0..127, a few per poll.");
        Serial.println("If the log stops, the last range printed is where it");
        Serial.println("died - that is the whole point of printing as we go.");
    }
    Serial.println();
}

void loop()
{
    uint32_t now[NWORDS];
    int i, changes = 0;

    /* Walk the module ids first, a window at a time, then settle into
     * watching the block. */
    if (next_id < NMODULES) {
        int r = hunt_step();

        if (r == 1) {
            opened_by = 1;
            adc_init();
            adc_start(ADC_KEY_CHAN);
            dump("after: ");
            Serial.print("writable now: ");
            Serial.println(adc_writable() ? "yes" : "no");
            Serial.println("Press and HOLD a button.");
        } else if (r == -1) {
            Serial.println("no module id opened it.");
        }
        return;
    }
    if (opened_by == -2) {
        opened_by = -3;
        adc_init();
        adc_start(ADC_KEY_CHAN);
        dump("after: ");
        Serial.print("chan0 enable bit set; +0x00 = 0x");
        Serial.println(sl6806_mmio_read(ADC_BASE + 0x00), HEX);
        Serial.println("Press and HOLD a button. Jack/SD also watched.");
        return;
    }

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

    {
        int g = gpio_state();

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
    }

    if (Serial.read() >= 0) {
        Serial.print("steady: chan0 = 0x");
        Serial.print(sl6806_mmio_read(ADC_BASE + ADC_CHAN(ADC_KEY_CHAN)), HEX);
        Serial.print("   +0x08 = 0x");
        Serial.println(sl6806_mmio_read(ADC_BASE + 0x08), HEX);
    }
}
