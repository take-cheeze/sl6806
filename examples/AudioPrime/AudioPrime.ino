/*
 * AudioPrime - START before the descriptor, and a 16-byte primer.
 *
 * [M] WHERE THIS STARTS. The audio block is configured and does not fetch: a
 * TX descriptor retires in a time proportional to its length but IDENTICAL
 * whether the source is SRAM or XIP flash, and flash is off-chip SPI at 32 MHz
 * where the measured 9 us for 4796 bytes is impossible by two orders of
 * magnitude. So the length register is counted down and no memory is read.
 * docs/AUDIO.md has the ledger, including the conclusions of mine it retracts.
 *
 * Every clock, route, module and gate candidate is negative. What is left is
 * the submit path, and reading it turned up two differences from the vendor:
 *
 *   [V] 0x0080D9BC - the vendor's submit - writes TX_ADDR, TX_CTRL[31:16] and
 *   TX_TRIG and DOES NOT START. The start is separate, at 0x00D97A5E, and is
 *   exactly TX_CTRL |= 0x10. sl6806_audio_play() does the same four writes, so
 *   the descriptor itself is right to the bit, watermark included.
 *
 *   [V] But at 0x00D97A20 the vendor primes first, and in an order this
 *   framework has never used:
 *
 *       TX_CTRL |= 0x10                       START set FIRST
 *       submit(state + 0x118, len = 16)       a SIXTEEN-BYTE descriptor
 *       while (state[0x128]) ;                wait for its own ISR
 *       0x00D94B0C()
 *
 * [!] AND THE WAIT IS SOFTWARE, NOT HARDWARE. The base is 0x0082B310, the
 * audio driver's state pointer, and 0x0080D955 - a callback - is stored beside
 * it two instructions earlier. So +0x128 is a flag its interrupt handler
 * clears, not a register. That matters: it means the vendor's sequence cannot
 * be copied literally here, because this framework has no audio interrupt, and
 * a sketch that spun on an MMIO address that is really SRAM would hang or
 * silently do nothing. Polling the completion flag is the right substitute.
 *
 * So the hardware difference reduces to exactly two things, and both are cheap:
 *
 *   1. START set BEFORE the descriptor is written, rather than after.
 *   2. A 16-byte primer submitted before the real buffer.
 *
 * THE DETECTOR is unchanged and needs no judgement: 23040 bytes of 48 kHz
 * 16-bit stereo is 120 ms of audio, and a buffer that is really played cannot
 * retire faster than it lasts. Unpaced it comes back in ~43 us. A factor of
 * 2800 is not something anyone has to squint at.
 *
 *     make SKETCH=examples/AudioPrime RUN_MODE=poll run
 *
 *   'b' baseline - descriptor then START, what the driver does now
 *   's' START first, then the descriptor
 *   'p' 16-byte primer, then the real buffer
 *   'r' primer before EVERY buffer, four in a row
 *   'q' stop
 *
 * Every key echoes and every rejected submit says so rather than printing a
 * zero, because in this investigation three probes have reported a failure to
 * run as though it were a result.
 */

#include <Arduino.h>
#include "sl6806_audio.h"
#include "sl6806_mmio.h"

#define RATE      48000u
#define CHANNELS  2u
#define FRAMES    5760u                 /* 23040 bytes = 120 ms */
#define NOTE_MS   (FRAMES * 1000u / RATE)

static int16_t wave[FRAMES * CHANNELS] __attribute__((aligned(4)));
static uint32_t primer[4] __attribute__((aligned(4)));   /* the vendor's 16 */

static const int16_t quarter[17] = {
        0,  3212,  6393,  9512, 12539, 15446, 18204, 20787,
    23170, 25330, 27245, 28898, 30273, 31357, 32138, 32610, 32767
};

static int16_t sine(unsigned phase, unsigned period)
{
    unsigned step = (phase * 64u) / period, q = step & 15u;

    switch ((step >> 4) & 3u) {
    case 0:  return quarter[q];
    case 1:  return quarter[16u - q];
    case 2:  return (int16_t)-quarter[q];
    default: return (int16_t)-quarter[16u - q];
    }
}

static bool up;

/*
 * Write the descriptor without starting it - the vendor's 0x0080D9BC, which
 * is also exactly what sl6806_audio_play() does apart from its final START.
 * Open-coded here so START can be moved to either side of it.
 */
static void descriptor(const void *buf, uint32_t len)
{
    sl6806_mmio_write(SL6806_AUD_TX_ADDR, (uint32_t)(uintptr_t)buf);
    sl6806_mmio_field(SL6806_AUD_TX_CTRL, SL6806_AUD_LEN_SHIFT, 16, len);
    sl6806_mmio_write(SL6806_AUD_TX_TRIG,
                      (((len * 3u) / 4u) << SL6806_AUD_LEN_SHIFT)
                      | SL6806_AUD_TRIG_ARM);
}

static void start(void)
{
    sl6806_mmio_set(SL6806_AUD_TX_CTRL, SL6806_AUD_START);
}

/* Wait for the completion flag, bounded past a paced buffer and well inside
 * the host's ~1 s SCSI timeout. Returns microseconds, or the bound. */
static unsigned long wait_done(void)
{
    unsigned long t0 = micros(), us;

    while ((us = micros() - t0) < 300000ul)
        if (sl6806_audio_done())
            break;
    return us;
}

static void verdict(const char *tag, unsigned long us)
{
    Serial.printf("  %-26s retired in %6lu us   %s\n", tag, us,
                  us * 2ul >= (unsigned long)NOTE_MS * 1000ul
                      ? "<<< PACED - THIS IS IT"
                      : "unpaced");
}

void setup()
{
    unsigned i;

    Serial.begin(115200);
    Serial.println();
    Serial.println("=== SL6806 audio: START order, and a 16-byte primer ===");

    for (i = 0; i < FRAMES; i++) {
        int16_t s = (int16_t)(sine(i % (RATE / 440u), RATE / 440u) / 2);
        wave[i * CHANNELS + 0] = s;
        wave[i * CHANNELS + 1] = s;
    }
    for (i = 0; i < 4; i++)
        primer[i] = 0;                  /* silence: it is a primer, not audio */

    up = sl6806_audio_begin(RATE) != 0;
    Serial.print("audio begin: ");
    Serial.println(up ? "ok" : "MODULE CLOCK REFUSED - stop here");
    if (!up)
        return;
    sl6806_audio_route(0, 1, 0);
    sl6806_audio_route(1, 1, 0);
    for (i = 0; i < SL6806_AUD_TX_CHANNELS; i++) {
        sl6806_audio_volume(i, SL6806_AUD_VOL_MAX);
        sl6806_audio_mute(i, 0);
    }
    sl6806_audio_clock_start(1);

    Serial.printf("%u bytes = %u ms of audio; unpaced retires in ~43 us\n",
                  (unsigned)sizeof(wave), NOTE_MS);
    Serial.println("'b' baseline  's' START first  'p' primer  'r' primer x4  'q'");
}

void loop()
{
    int key = Serial.read();

    if (!up || key < 0)
        return;
    if (key == '\r' || key == '\n')
        return;
    if (key != 'b' && key != 's' && key != 'p' && key != 'r' && key != 'q') {
        Serial.printf("unknown key '%c' - try b s p r q\n", key);
        return;
    }
    Serial.printf("\n[%c]\n", key);
    if (key == 'q') {
        Serial.println("stopped.");
        return;
    }

    switch (key) {
    case 'b':
        descriptor(wave, sizeof(wave));
        start();
        verdict("descriptor then START", wait_done());
        break;

    case 's':
        /* The order 0x00D97A20 uses: START is already set when the descriptor
         * lands. If the block latches the descriptor on a running channel
         * rather than on the START edge, this is the difference. */
        start();
        descriptor(wave, sizeof(wave));
        verdict("START then descriptor", wait_done());
        break;

    case 'p': {
        unsigned long a, b;

        start();
        descriptor(primer, sizeof(primer));
        a = wait_done();
        Serial.printf("  primer 16 bytes            retired in %6lu us\n", a);
        descriptor(wave, sizeof(wave));
        start();
        b = wait_done();
        verdict("real buffer after primer", b);
        break;
    }

    case 'r': {
        /* If the primer arms a single fetch rather than the channel, it has to
         * precede every buffer. Four in a row, so a one-shot effect shows up
         * as the first row differing from the rest. */
        int i;

        for (i = 0; i < 4; i++) {
            unsigned long us;

            start();
            descriptor(primer, sizeof(primer));
            (void)wait_done();
            descriptor(wave, sizeof(wave));
            start();
            us = wait_done();
            {
                char tag[20];
                tag[0] = 'b'; tag[1] = 'u'; tag[2] = 'f'; tag[3] = 'f';
                tag[4] = 'e'; tag[5] = 'r'; tag[6] = ' ';
                tag[7] = (char)('1' + i); tag[8] = 0;
                verdict(tag, us);
            }
        }
        break;
    }

    default:
        break;
    }
}
