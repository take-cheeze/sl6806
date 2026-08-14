/*
 * AudioLen - is "10 us" a measurement, or the floor of the instrument?
 *
 * WHAT THIS IS CHECKING. docs/AUDIO.md rests its central negative on one
 * number: a 4796-byte descriptor retires in a mean of 10 us, which would be
 * 480 MB/s, which a 64 MHz AHB cannot do - so nothing is being transferred.
 * The reasoning is sound IF the 10 us is a measurement. Look at how it is
 * taken (examples/AudioWall):
 *
 *     sl6806_audio_play(...);
 *     start = micros();
 *     while (micros() - start < 100000)
 *         if (sl6806_audio_done())
 *             return micros() - start;
 *
 * The value returned when the flag is already up is the cost of ONE ITERATION
 * of that loop - a micros() call, an MMIO read, a write-1-to-clear write,
 * another micros(). At the 312 ns per register access this project measured
 * for the LCDC, that is not obviously less than 10 us. If one iteration IS
 * about 10 us then "10 us" does not mean 480 MB/s; it means **the flag was
 * already set the first time it was asked**, and it carries no timing
 * information whatever.
 *
 * AND THE TELL IS ALREADY IN THE DOCUMENT: "a mean of 10 us in all twelve.
 * Nothing varies." An observable that does not vary across a twelve-way sweep
 * is usually a broken instrument, not a discovery. That sentence cost the PWM
 * investigation four rounds and about a dozen hardware sessions - see
 * sl6806_re_notes.md 32, where `commits 0/4` held across 168 configurations
 * for exactly this reason, and where the counter turned out to have been
 * running the whole time.
 *
 * [M] FIRST RUN - AND BOTH OF THIS SKETCH'S OWN HYPOTHESES ARE DEAD:
 *
 *   1. The floor is 1.73 us per iteration, so AudioWall's 10 us is about six
 *      iterations, comfortably above it. **The 10 us is a real reading**, and
 *      the argument in docs/AUDIO.md is not an artifact of the poll loop.
 *   2. TX_CTRL reads 0x00000800 before anything starts. DONE is bit 10 and is
 *      CLEAR. **The flag is not stale.** (Bit 11 is set and survives two
 *      write-backs, so it is a status bit and not write-1-to-clear - a small
 *      new fact, and the first evidence about what else lives in that word.)
 *
 * So the transfer between them is a genuinely fast completion on a real flag,
 * and the tidy explanation this sketch was written to find does not exist.
 * Phase 3 is now the whole of it.
 *
 * THREE THINGS, in order, each cheap:
 *
 *   1. CALIBRATE THE INSTRUMENT. Time the identical poll loop against a
 *      condition that can never be true, and divide. That is the floor. Every
 *      number this project has quoted for audio has to be read against it.
 *
 *   2. IS THE FLAG ALREADY SET BEFORE THE TRANSFER STARTS? Arm a descriptor
 *      but do not start it, and read the flags. Also write the
 *      acknowledgement and read back, to confirm the bit is write-1-to-clear
 *      at all - nothing had ever verified that it clears.
 *
 *   3. DOES COMPLETION TIME SCALE WITH LENGTH? This is the control the whole
 *      question turns on, and the sweeps so far have varied routes, clock
 *      sources and 0x400E0000's bits while holding the length at 4796 bytes
 *      throughout. A transfer takes time proportional to its length. Nothing
 *      else does.
 *
 *        time scales with length  -> data IS moving, and the only thing wrong
 *                                    is the rate. The negative is void.
 *        time is flat in length   -> nothing moves, and the document is right
 *                                    for a better reason than it had.
 *
 * It is the same control that settled the PWM: halve the period and the time
 * must halve. Here: double the length and the time must double.
 *
 *     make SKETCH=examples/AudioLen RUN_MODE=poll run
 *
 * 's' to run. Nothing blocks longer than the harness allows.
 */

#include <Arduino.h>
#include "sl6806_audio.h"
#include "sl6806_mmio.h"
#include "wiring_time.h"

#define RATE     48000u
#define CHANNELS 2

/* 16384 frames = 65536 bytes, over the 0xFFFF descriptor limit, so the
 * longest row below is 16000 frames = 64000 bytes. The shortest is 128 bytes.
 * That is a 500:1 span - if the completion time is proportional to length it
 * cannot possibly hide in the noise. */
#define MAX_FRAMES 16000u
/* [!] ALIGNED(4), and the alignment is load-bearing. sl6806_audio_play()
 * rejects a buffer whose address is not word-aligned, and `int16_t wave[]` is
 * only 2-byte aligned - the first run of this sketch had every one of its
 * eight rows rejected before touching hardware, and printed a column of
 * zeros that reads exactly like the "flat, therefore nothing moves" result it
 * was built to look for. */
static int16_t wave[MAX_FRAMES * CHANNELS] __attribute__((aligned(4)));

static const uint32_t lens[] = {
    128u, 512u, 2048u, 4796u, 8192u, 16384u, 32768u, 64000u,
};
#define NLEN ((int)(sizeof lens / sizeof lens[0]))

static bool up;

/*
 * The floor. The identical shape of loop as AudioWall's, but testing
 * something that is never true, so what comes back is pure overhead.
 */
static uint32_t pollFloorUs(unsigned iters)
{
    uint32_t start = micros();
    unsigned i;
    volatile uint32_t sink = 0;

    for (i = 0; i < iters; i++) {
        sink += sl6806_audio_flags();      /* the same MMIO read + W1C write */
        (void)micros();                    /* and the same clock reads */
    }
    (void)sink;
    return micros() - start;
}

/* One buffer, timed, and also counted in iterations - because an elapsed time
 * of one iteration means "already done", not a rate. */
static uint32_t playOne(uint32_t len, unsigned *iters_out, int *rejected)
{
    uint32_t start;
    unsigned n = 0;

    *rejected = 0;
    if (sl6806_audio_play(wave, len) != 0) {
        *iters_out = 0;
        *rejected = 1;
        return 0;
    }
    start = micros();
    while (micros() - start < 60000ul) {
        n++;
        if (sl6806_audio_done()) {
            *iters_out = n;
            return micros() - start;
        }
    }
    *iters_out = n;
    return 60000ul;
}

void setup()
{
    unsigned i;

    Serial.begin(115200);
    Serial.println();
    Serial.println("=== SL6806 audio: is 10 us a measurement or a floor? ===");

    for (i = 0; i < MAX_FRAMES; i++) {
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
    Serial.println("press 's' to run");
}

void loop()
{
    if (Serial.read() != 's' || !up)
        return;

    /* 1. the instrument's floor */
    {
        uint32_t t100 = pollFloorUs(100);

        Serial.println("\n--- 1. instrument floor");
        Serial.printf("    100 poll iterations took %lu us -> %lu.%02lu us each\n",
                      (unsigned long)t100, (unsigned long)(t100 / 100),
                      (unsigned long)((t100 % 100)));
        Serial.println("    Any completion time near this is 'already done',");
        Serial.println("    not a transfer rate. The 10 us in docs/AUDIO.md");
        Serial.println("    has to be read against this number.");
    }

    /* 2. is DONE already up before the transfer starts? */
    {
        uint32_t before, after_ack, after_ack2;

        Serial.println("\n--- 2. is the flag stale?");
        before = sl6806_mmio_read(SL6806_AUD_TX_CTRL);
        sl6806_mmio_write(SL6806_AUD_TX_CTRL, before);      /* W1C attempt */
        after_ack = sl6806_mmio_read(SL6806_AUD_TX_CTRL);
        sl6806_mmio_write(SL6806_AUD_TX_CTRL, after_ack);
        after_ack2 = sl6806_mmio_read(SL6806_AUD_TX_CTRL);

        Serial.printf("    TX_CTRL before ack %08x  DONE %s\n",
                      (unsigned)before,
                      (before & SL6806_AUD_IRQ_DONE) ? "SET" : "clear");
        Serial.printf("    after one ack      %08x  DONE %s\n",
                      (unsigned)after_ack,
                      (after_ack & SL6806_AUD_IRQ_DONE) ? "STILL SET" : "cleared");
        Serial.printf("    after two acks     %08x\n", (unsigned)after_ack2);
        if (after_ack & SL6806_AUD_IRQ_DONE)
            Serial.println("    ^ the bit does not clear - it is not W1C, and"
                           " every completion reading so far is void");
    }

    /* 3. the control: does the time scale with the length? */
    Serial.println("\n--- 3. does completion scale with length?");
    Serial.printf("    buffer at %08x  (word aligned: %s)\n",
                  (unsigned)(uintptr_t)wave,
                  ((uintptr_t)wave & 3u) ? "NO - every row will be rejected"
                                         : "yes");
    Serial.println("    len      us   iters   us/KiB");
    for (int i = 0; i < NLEN; i++) {
        unsigned iters = 0;
        int rejected = 0;
        uint32_t us = playOne(lens[i], &iters, &rejected);

        if (rejected) {
            /* Never print a rejected row as a zero. A column of zeros is
             * indistinguishable from the flat result this test looks for,
             * and that is how the first run of this sketch nearly produced a
             * false negative. */
            Serial.printf("    %5lu  REJECTED by sl6806_audio_play - not a"
                          " measurement\n", (unsigned long)lens[i]);
            continue;
        }
        Serial.printf("    %5lu  %6lu  %5u   %lu\n",
                      (unsigned long)lens[i], (unsigned long)us, iters,
                      (unsigned long)(us * 1024ul / (lens[i] ? lens[i] : 1)));
    }
    Serial.println("\n    A transfer's time is proportional to its length.");
    Serial.println("    Flat us across a 500:1 span of length = nothing moves.");
    Serial.println("    Rising us = data IS moving and only the rate is wrong,");
    Serial.println("    and docs/AUDIO.md's central negative is void.");
    Serial.println("    Compare 'iters' against the floor above: 1-2 means the");
    Serial.println("    flag was up before the loop ever looked.");
}
