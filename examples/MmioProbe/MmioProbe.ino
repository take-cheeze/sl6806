/*
 * MmioProbe - confirm on hardware where peripherals actually live.
 *
 * This is the sketch that turns a guess from tools/sl6806-find-mmio into a
 * fact. It reads candidate addresses and reports which ones respond, and
 * optionally toggles bits while you watch a pin.
 *
 * ---------------------------------------------------------------------
 *  READ THIS FIRST
 * ---------------------------------------------------------------------
 * Reading an unmapped address on a Cortex-M raises a BusFault. In payload
 * mode the boot ROM owns the fault handler, so the likely outcome is that the
 * device stops responding. That is not damage: unplug it, hold the boot
 * button, plug it back in, and you are in bootloader mode again with nothing
 * changed. Flash is never touched by this sketch.
 *
 * WRITING is a different matter and is off by default. A write to a clock or
 * power register can switch off the core, and a write to the flash controller
 * could in principle corrupt it. Only enable PROBE_WRITES once you have a
 * verified dump you can restore, and only for addresses you have reason to
 * believe are GPIO.
 *
 * The scan prints each address BEFORE reading it, so if the device dies the
 * last line in your monitor is the address that killed it - which is itself
 * useful information.
 */

#include <Arduino.h>

/* Set to 1 only after reading the warning above. */
#define PROBE_WRITES 0

/* Candidates to check. Replace these with what tools/sl6806-find-mmio
 * suggests for your dump - these are only the conventional ARM peripheral
 * windows, and this SoC does not follow convention elsewhere, so treat them
 * as a starting point rather than a recommendation. */
static const uint32_t candidates[] = {
    0x40000000, 0x40010000, 0x40020000, 0x40030000,
    0x50000000, 0x50010000,
};

static void probe(uint32_t base)
{
    Serial.print("probing 0x");
    Serial.print(base, HEX);
    Serial.println(" ...");
    Serial.flush();

    /* Four consecutive registers is enough to recognise a port. */
    for (int i = 0; i < 4; i++) {
        uint32_t addr = base + i * 4;
        uint32_t v = *(volatile uint32_t *)addr;

        Serial.print("  +0x");
        Serial.print(i * 4, HEX);
        Serial.print(" = 0x");
        Serial.println(v, HEX);
    }

#if PROBE_WRITES
    /* Write-then-read-back: a register that keeps what you wrote is real and
     * writable. Restore the original value either way. */
    volatile uint32_t *reg = (volatile uint32_t *)base;
    uint32_t saved = *reg;
    *reg = 0xA5A5A5A5;
    uint32_t got = *reg;
    *reg = saved;

    Serial.print("  write test: wrote A5A5A5A5, read back 0x");
    Serial.println(got, HEX);
#endif
}

void setup()
{
    Serial.begin(115200);

    Serial.println();
    Serial.println("=== SL6806 MMIO probe ===");
    Serial.println("If output stops, the last address printed is the one that");
    Serial.println("faulted. Replug in bootloader mode to recover.");
#if PROBE_WRITES
    Serial.println("WRITES ARE ENABLED.");
#else
    Serial.println("Read-only (set PROBE_WRITES to 1 to enable writes).");
#endif
    Serial.println();
    delay(1000);

    for (unsigned i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        probe(candidates[i]);
        delay(200);
    }

    Serial.println();
    Serial.println("scan complete - the device survived every address above.");
}

void loop()
{
}
