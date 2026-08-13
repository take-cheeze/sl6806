/*
 * AudioWall - find the audio block's functional-clock bit, with a real
 * witness.
 *
 * ---------------------------------------------------------------------
 *  WHY: THE SAME FAILURE, FOR THE THIRD TIME
 * ---------------------------------------------------------------------
 * cores/sl6806/sl6806_module.h opens with this, written about two other
 * peripherals:
 *
 *     A module clock gives a peripheral its REGISTERS. It does not give it
 *     its FUNCTION. That distinction cost this project two peripherals and a
 *     lot of bench time, and both failures looked identical from the outside.
 *
 * Audio is the third. Measured 2026-08-13, over twelve combinations of output
 * route and clock source:
 *
 *     every register holds exactly what was written to it
 *     the PLL registers hold 0x00103060 / 0x8C498C00
 *     the bit clock holds 0x00000701 - divider 8, enabled
 *     the DMA accepts a descriptor and raises its completion flag
 *     and 25 ms of audio retires in a mean of 10 MICROSECONDS
 *
 * 4796 bytes in 10 us is 480 MB/s on a 64 MHz Cortex-M4. Nothing is being
 * transferred at audio rate. Configured, and not running - which is exactly
 * what the PWM did for twelve sessions and the camera did for eight.
 *
 * For both of those the answer was a functional clock. For the camera it was
 * 0x400E0000 bit 6. **Only four of that register's 32 bits are attributed**,
 * and until 2026-08-13 nobody could walk it, because it would not hold a bit
 * from a payload. examples/BtProbe changed that: after the group bring-up it
 * reads 0x21 and keeps it.
 *
 * So this walks it, with the buffer completion time as the witness.
 *
 * ---------------------------------------------------------------------
 *  WHAT IT DOES, IN ORDER
 * ---------------------------------------------------------------------
 *   1. Brings audio up and opens the 0x400E0000 gate (module 46, the PLL,
 *      0x4008011C - the sequence BtProbe ran without incident).
 *
 *   2. **Asks whether the DMA engine touches memory at all.** This is worth
 *      more than the walk if it comes back no. A capture buffer is filled
 *      with a pattern, handed to the RX descriptor, and checked afterwards.
 *      If it changes, the engine really moves data and only the audio clock
 *      domain is wrong. If it does not, the completion flag is a lie and
 *      nothing is running - which would make the whole DMA reading above
 *      much weaker than it looks, and is a result worth having either way.
 *
 *   3. Walks the bits of 0x400E0000 that are not already set. For each: set
 *      it, play one buffer, time it, restore the register. A bit that changes
 *      the completion time is the answer.
 *
 * ---------------------------------------------------------------------
 *  SAFETY
 * ---------------------------------------------------------------------
 * This only ever SETS bits, and restores the register to exactly what it
 * found between every step. Bits that are already set when it starts are
 * skipped and never cleared - turning off a functional clock something else
 * is using (the USB the console is on, for instance) is the one way this
 * could take the device with it.
 *
 * It cannot brick anything in payload mode: flash is never written. It can
 * hang the device. Unplug, hold the boot button, plug back in - and if it
 * hangs, the last bit printed is the one that did it, which is itself the
 * result.
 *
 *     make SKETCH=examples/AudioWall RUN_MODE=poll run
 *
 * WHAT COUNTS AS A RESULT
 *
 *   1. **A bit that moves the completion time off 10 us.** That is the audio
 *      block's functional clock, and it is the last thing between this
 *      framework and a sound.
 *   2. **The capture test showing memory unchanged.** The DMA is not running
 *      at all, and "the data path is confirmed" needs retracting.
 *   3. **Nothing at all across 32 bits.** Then 0x400E0000 is not audio's
 *      gate, and the next suspects are the module-2 clock needing the
 *      vendor's off-then-on cycle, or the analogue supply.
 */

#include <Arduino.h>
#include "sl6806_console.h"
#include "sl6806_audio.h"
#include "sl6806_module.h"

#define RATE      48000u
#define FRAMES    1199u
#define CHANNELS  2u

static int16_t wave[FRAMES * CHANNELS] __attribute__((aligned(4)));
static uint32_t rxbuf[64] __attribute__((aligned(4)));

#define RX_PATTERN(i)   (0xA5A50000u | (i))

static bool up;
static bool done;
static int  bit;
static uint32_t base_us;
static uint32_t initial;

static void printHex(uint32_t v)
{
    Serial.print("0x");
    Serial.print(v, HEX);
}

/* One buffer, timed. Bounded well under the host's ~1 s SCSI timeout. */
static uint32_t playOne(void)
{
    uint32_t start;

    if (sl6806_audio_play(wave, sizeof(wave)) != 0)
        return 0;

    start = micros();
    while (micros() - start < 100000ul) {
        if (sl6806_audio_done())
            return micros() - start;
    }
    return 100000ul;
}

/* Median-ish: the mean of four, which is enough to see 10 us become anything
 * else and cheap enough to run 32 times inside a USB command handler. */
static uint32_t timeFour(void)
{
    uint32_t t = 0;
    int i;

    for (i = 0; i < 4; i++)
        t += playOne();
    return t / 4u;
}

/*
 * Does the engine write memory? The one question a TX-only test cannot ask.
 */
static void captureTest(void)
{
    unsigned i, changed = 0;
    uint32_t start;

    for (i = 0; i < sizeof(rxbuf) / sizeof(rxbuf[0]); i++)
        rxbuf[i] = RX_PATTERN(i);

    Serial.println();
    Serial.println("--- does the DMA touch memory? ---");
    Serial.print("  rxbuf at ");
    printHex((uint32_t)(uintptr_t)rxbuf);
    Serial.println(", filled with a pattern");

    if (sl6806_audio_capture(rxbuf, sizeof(rxbuf)) != 0) {
        Serial.println("  capture() refused the buffer");
        return;
    }

    start = micros();
    while (micros() - start < 100000ul)
        if (sl6806_audio_capture_done())
            break;

    for (i = 0; i < sizeof(rxbuf) / sizeof(rxbuf[0]); i++)
        if (rxbuf[i] != RX_PATTERN(i))
            changed++;

    Serial.print("  after ");
    Serial.print(micros() - start);
    Serial.print(" us: ");
    Serial.print(changed);
    Serial.print(" of ");
    Serial.print((unsigned)(sizeof(rxbuf) / sizeof(rxbuf[0])));
    Serial.println(" words changed");
    Serial.println(changed ? "  THE ENGINE MOVES DATA - only the clock domain is wrong"
                           : "  memory untouched - the completion flag is not a transfer");
}

void setup()
{
    unsigned i;

    Serial.begin(115200);
    Serial.println();
    Serial.println("=== SL6806 0x400E0000 walk, audio as the witness ===");
    Serial.println("Configured and not running is the PWM's failure and the");
    Serial.println("camera's. For both the answer was a functional clock.");
    Serial.println("Read this sketch's header before drawing conclusions.");
    Serial.println();

    /* A square wave, not a sine: nothing here is listening, and a square is
     * one line. Half scale for the same reason ToneDemo is. */
    for (i = 0; i < FRAMES; i++) {
        int16_t s = (i % 109u) < 54u ? 16000 : -16000;
        wave[i * CHANNELS + 0] = s;
        wave[i * CHANNELS + 1] = s;
    }

    up = sl6806_audio_begin(RATE) != 0;
    Serial.print("audio begin: ");
    Serial.println(up ? "ok" : "MODULE CLOCK REFUSED - stop here");
    if (!up)
        return;

    sl6806_audio_route(0, 1, 0);
    sl6806_audio_route(1, 1, 0);

    Serial.print("group gate: ");
    Serial.println(sl6806_periph_group_begin() ? "module 46 up" : "REFUSED");

    initial = sl6806_mmio_read(SL6806_PERIPH_ENABLE_REG);
    Serial.print("0x400E0000 = ");
    printHex(initial);
    Serial.println(initial ? "  (holds bits - the walk can proceed)"
                           : "  READS ZERO - the gate did not open, walk is void");

    captureTest();

    base_us = timeFour();
    Serial.println();
    Serial.print("--- baseline: ");
    Serial.print(base_us);
    Serial.println(" us per buffer (25000 would be real-time) ---");
    Serial.println("walking 0x400E0000. Only bits that are clear are tried,");
    Serial.println("and the register is restored after every one.");
    Serial.flush();

    bit = 0;
}

void loop()
{
    uint32_t us;

    if (!up || done)
        return;

    /* One bit per loop() call, so the console keeps up and a hang names the
     * bit that caused it. */
    if (sl6806_console_space() < (int)(SL6806_CONSOLE_SIZE / 2))
        return;

    if (initial & (1u << bit)) {
        Serial.print("  bit ");
        Serial.print(bit);
        Serial.println(": already set, skipped");
    } else {
        Serial.print("  bit ");
        Serial.print(bit);
        Serial.print(": ");
        Serial.flush();

        sl6806_mmio_set(SL6806_PERIPH_ENABLE_REG, 1u << bit);
        delay(10);
        us = timeFour();
        sl6806_mmio_write(SL6806_PERIPH_ENABLE_REG, initial);
        delay(10);

        Serial.print(us);
        Serial.print(" us");
        /* Anything outside a tight band around the baseline is news. */
        if (us > base_us + (base_us / 2u) + 5u || us + 5u < base_us)
            Serial.println("   <-- CHANGED, this is the one to chase");
        else
            Serial.println();
    }

    bit++;
    if (bit < 32)
        return;

    Serial.println();
    Serial.print("0x400E0000 restored to ");
    printHex(sl6806_mmio_read(SL6806_PERIPH_ENABLE_REG));
    Serial.println();
    Serial.println("done. Report any CHANGED line, and the capture test's");
    Serial.println("verdict - that one matters even if no bit moved.");
    sl6806_audio_end();
    done = true;
}
