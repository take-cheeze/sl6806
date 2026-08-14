/*
 * ToneDemo - a 440 Hz sine into the audio block, and what still will not pace it.
 *
 * ---------------------------------------------------------------------
 *  [!] REWRITTEN. THIS SKETCH'S OLD PREMISE WAS RETRACTED.
 * ---------------------------------------------------------------------
 * The header this replaces argued, at length, that a 4796-byte descriptor
 * retiring in 10 us meant nothing was being transferred - 480 MB/s on a
 * 64 MHz core being impossible. Both halves of that are now wrong:
 *
 *   [M] The DMA MOVES DATA. examples/AudioLen swept the descriptor length,
 *   which none of the twelve-combination runs ever did, and completion time is
 *   LINEAR IN LENGTH across 500:1 - us = 0.001926 * len + 1.43, ~519 bytes/us.
 *   A descriptor retired without doing anything takes CONSTANT time. This does
 *   not. Every sweep behind the old conclusion varied routes, clock sources and
 *   all 32 bits of 0x400E0000 while holding the length at 4796 bytes, so the
 *   one variable that could have exposed it was never moved.
 *
 *   [M] And 64 MHz was the wrong clock to call it impossible against. Notes 30
 *   measured the PLL already running at 192 MHz; ~130 M words/s is two thirds
 *   of a word per cycle on such a bus, an ordinary DMA rate.
 *
 * WHAT IS TRUE INSTEAD, and it is more useful than the negative was: the DMA
 * runs at bus speed and NOTHING PACES IT AT THE SAMPLE RATE. Real time for
 * 48 kHz stereo 16-bit is 192 KB/s; the measured rate is 2667x that, and the
 * ratio is constant at every length. That is a DMA draining into a sink which
 * never back-pressures.
 *
 * SUSPECTS THIS SKETCH NO LONGER CARRIES, because they are eliminated:
 *
 *   - "the audio PLL may not be locking". It is at exactly 24.576 MHz.
 *     0x40080014 reads 0x8C498C00, bit-for-bit what audio_pll_set() writes for
 *     the 48 kHz family - which is also the point: our own driver sets it from
 *     sl6806_audio_begin(), so this confirms the decode rather than the chip's
 *     reset state. See docs/AUDIO.md.
 *   - the four output routes and the three source modes. Twelve combinations,
 *     every one still unpaced. That negative was correctly measured: a paced
 *     DMA would have jumped to 25 ms and none did.
 *   - the vendor's wider clock chain - romclk 0, module 87, romclk 31 (a ROM
 *     no-op), romclk 56, and 0x4009B000. examples/AudioPll applied all of it
 *     with readback on every step and the sweep did not move.
 *
 * SO WHAT THIS SKETCH DOES NOW. It stops re-running the twelve and goes after
 * the three things that have never been examined, with an instrument that
 * cannot report a constant when it should report a slope:
 *
 *   A. Report state, and CENSUS the two registers in the pacing path that have
 *      never had their implemented bits read back - 0x40080094, the bit clock,
 *      and 0x4009B000. A census is what found four hidden bits in the PWM's
 *      pair register after a year of that register being "known".
 *   B. Establish pacing with TWO lengths, not one. The ratio is the detector:
 *      real time is 1x, and anything that introduces pacing moves the slope by
 *      a factor of 2667. A single length is what hid this for four sessions.
 *   C. Sweep the bit clock divider. sl6806_audio.c hardcodes 8 into
 *      0x40080094[10:8] because the vendor writes 8; the field is three bits
 *      and the other seven values have never been tried.
 *   D. Cycle module 2 off and on rather than bare-enabling it, which is the
 *      one difference between the vendor's stream start and ours that has
 *      never been reproduced.
 *
 * The sine plays throughout. If anything ever paces the DMA you will hear it
 * before you read it - but the ratio is the result, because five rounds of the
 * PWM investigation were lost to trusting an impression over a number.
 *
 * Plug headphones in. The speaker may need an enable that is not in the
 * variant's pad map; the headphone jack is at least wired to something, since
 * on this board the lead doubles as the FM aerial.
 *
 * WHAT IS STILL A GUESS
 *
 *   - The sample format. 16-bit stereo little-endian is what the vendor's
 *     config struct looks like it carries. Wrong pitch or noise points here
 *     first: try mono, try 8-bit.
 *   - There is no interrupt. The vendor refills from IRQ 30 at the
 *     three-quarter mark; this polls and re-submits, so it will gap between
 *     buffers even when everything else is right. A gapped tone is a success.
 *
 *     make SKETCH=examples/ToneDemo RUN_MODE=poll run
 *
 * In payload mode this cannot brick anything: flash is never written. It can
 * hang the device - unplug, hold the boot button, plug back in.
 */

#include <Arduino.h>
#include "sl6806_audio.h"
#include "sl6806_module.h"
#include "sl6806_mmio.h"

#define RATE      48000u
#define TONE_HZ   440u
#define CHANNELS  2u

/*
 * A whole number of cycles, so re-submitting the buffer does not click.
 * 48000 / 440 is not an integer, so the period is rounded to 109 samples -
 * 440.4 Hz rather than 440, which no ear will mind - and the buffer holds
 * exactly eleven of them. 1199 frames = 4796 bytes = 25.0 ms of audio.
 *
 * In .bss, which ld/sl6806_payload.ld puts above the 64 KiB load window - see
 * examples/BigBuffer. The DMA reads it where it lies, so it must be static:
 * a buffer on loop()'s stack would be handed to the hardware and then reused.
 */
#define PERIOD    (RATE / TONE_HZ)      /* 109 samples */
#define FRAMES    (PERIOD * 11u)
static int16_t wave[FRAMES * CHANNELS] __attribute__((aligned(4)));

/* 25 ms a buffer if it is ever paced; 20 per combination, 12 combinations. */
#define BUFFERS_PER_ROUTE  20u

/* A quarter-wave sine table, so this needs no libm and no float. Values are
 * sin(x) * 32767 at 0, 90/16, 2*90/16 ... degrees. */
static const int16_t quarter[17] = {
        0,  3212,  6393,  9512, 12539, 15446, 18204, 20787,
    23170, 25330, 27245, 28898, 30273, 31357, 32138, 32610, 32767
};

static int16_t sine(unsigned phase, unsigned period)
{
    /* Fold the phase into the quarter table: 64 steps per cycle. */
    unsigned step = (phase * 64u) / period;
    unsigned q = step & 15u;
    int16_t v;

    switch ((step >> 4) & 3u) {
    case 0:  v = quarter[q];              break;
    case 1:  v = quarter[16u - q];        break;
    case 2:  v = (int16_t)-quarter[q];    break;
    default: v = (int16_t)-quarter[16u - q]; break;
    }
    return v;
}

static void fillTone(void)
{
    unsigned i;

    for (i = 0; i < FRAMES; i++) {
        /* Half scale. A first tone into an unknown amplifier should not be
         * full scale, and this one is going into a pair of earbuds. */
        int16_t s = (int16_t)(sine(i % PERIOD, PERIOD) / 2);
        wave[i * CHANNELS + 0] = s;
        wave[i * CHANNELS + 1] = s;
    }
}

static void report(const char *what, uint32_t v)
{
    Serial.print(what);
    Serial.print(" = 0x");
    Serial.println(v, HEX);
}

static bool up;
static int  phase = -1;
static int  step;

/*
 * TWO LENGTHS, AND THE RATIO IS THE RESULT.
 *
 * A single length cannot tell "the DMA is unpaced" from "the DMA is not
 * running", because both give a small constant. Two lengths can: an unpaced
 * DMA's time is proportional to length at bus speed, a paced one at 192 KB/s,
 * and those differ by 2667x. Reported as bytes/us so the comparison needs no
 * arithmetic from the reader.
 *
 * Bounded well under the host's ~1 s SCSI timeout, because in RUN_MODE=poll
 * this runs inside the boot ROM's USB command handler. 60 ms is also more than
 * twice the 25 ms a real-time buffer would take, so a paced transfer has room
 * to finish rather than reading as a timeout.
 */
static unsigned long timeLen(uint32_t len)
{
    unsigned long start;

    if (sl6806_audio_play(wave, len) != 0)
        return 0;                        /* rejected - never a measurement */
    start = micros();
    while (micros() - start < 60000ul)
        if (sl6806_audio_done())
            return micros() - start;
    return 60000ul;
}

static void pace(const char *tag)
{
    const uint32_t small = 4796u, big = (uint32_t)sizeof(wave);
    unsigned long a = timeLen(small), b = timeLen(big);

    Serial.printf("  %-22s %5lu B/%lu us   %5lu B/%lu us",
                  tag, (unsigned long)small, a, (unsigned long)big, b);
    if (!a || !b) {
        Serial.println("   REJECTED - not a measurement");
        return;
    }
    Serial.printf("   %lu B/us  %lux real time\n",
                  (unsigned long)(big / (b ? b : 1)),
                  (unsigned long)((big * 1000ul / 192ul) / (b ? b : 1)));
}

/*
 * An implemented-bit census, one bit at a time with a restore after each.
 *
 * All-ones in one write would be quicker and is how the PWM's pair register
 * was first "measured" - with 0x3F0F, which does not set bits [7:4] at all, so
 * four implemented bits went unseen for a year. One bit at a time cannot make
 * that mistake, and on a CRU register it also never holds an unknown bit for
 * more than a single write.
 */
static void census(const char *name, uint32_t reg)
{
    uint32_t saved = sl6806_mmio_read(reg);
    uint32_t mask = 0;
    unsigned b;

    for (b = 0; b < 32; b++) {
        uint32_t m = 1u << b;

        sl6806_mmio_write(reg, saved | m);
        if (sl6806_mmio_read(reg) & m)
            mask |= m;
        sl6806_mmio_write(reg, saved);
    }
    Serial.printf("  %-14s %08x  holds %08x  implemented %08x\n",
                  name, (unsigned)reg, (unsigned)saved, (unsigned)mask);
}

void setup()
{
    Serial.begin(115200);
    Serial.println();
    Serial.println("=== SL6806 tone: what still will not pace the DMA ===");
    Serial.println("read the header - the old 10 us conclusion is retracted.");

    fillTone();
    Serial.printf("buffer at %08x, %lu bytes, %s\n",
                  (unsigned)(uintptr_t)wave, (unsigned long)sizeof(wave),
                  ((uintptr_t)wave & 3u) ? "NOT WORD ALIGNED - rows will be"
                                           " rejected" : "word aligned");

    up = sl6806_audio_begin(RATE) != 0;
    Serial.print("sl6806_audio_begin(48000): ");
    Serial.println(up ? "ok" : "MODULE CLOCK REFUSED - stop here, see AudioProbe");
    if (!up)
        return;

    for (unsigned ch = 0; ch < SL6806_AUD_TX_CHANNELS; ch++) {
        sl6806_audio_volume(ch, SL6806_AUD_VOL_MAX);
        sl6806_audio_mute(ch, 0);
    }
    sl6806_mmio_write(SL6806_AUD_DAC, 0);
    sl6806_audio_route(0, 1, 0);
    sl6806_audio_route(1, 1, 0);
    sl6806_audio_clock_start(1);

    Serial.println("'a' state+census  'b' baseline pace  'c' divider sweep"
                   "  'd' module 2 cycle  'q' stop");
}

void loop()
{
    int key = Serial.read();

    if (!up)
        return;
    if (key == 'q') { phase = -1; Serial.println("stopped."); return; }
    if (key == 'a' || key == 'b' || key == 'c' || key == 'd') {
        phase = key; step = 0;
    }
    if (phase < 0)
        return;

    switch (phase) {
    case 'a':
        Serial.println("\n--- state, and a census of the pacing path");
        report("  PLL sel  40080010", sl6806_mmio_read(SL6806_AUD_PLL_SEL));
        report("  PLL rat  40080014", sl6806_mmio_read(SL6806_AUD_PLL_RATIO));
        report("  DAC      +0x008  ", sl6806_mmio_read(SL6806_AUD_DAC));
        report("  TX ctrl  +0x108  ", sl6806_mmio_read(SL6806_AUD_TX_CTRL));
        Serial.println("  0x8C498C00 in PLL rat is the 48 kHz family - and is");
        Serial.println("  our own audio_pll_set()'s write, not a reset value.");
        Serial.println("  census (one bit at a time, restored after each):");
        census("bit clock", SL6806_AUD_BITCLK_REG);
        census("0x4009B040", 0x4009B040u);
        census("0x4009B04C", 0x4009B04Cu);
        census("0x4009B050", 0x4009B050u);
        Serial.println("  any implemented bit outside what the driver writes is");
        Serial.println("  a field nobody has tried.");
        phase = -1;
        break;

    case 'b':
        Serial.println("\n--- baseline pacing (two lengths; the ratio is the result)");
        pace("as configured");
        Serial.println("  2667x real time = unpaced. 1x = paced, and done.");
        phase = -1;
        break;

    case 'c':
        /*
         * The field is three bits and the driver writes DIV - 1, so field N
         * divides by N + 1. The vendor's 8 is field 7, and fields 0..6 -
         * divide by 1 through 7 - have never been tried. 24.576 MHz / 8 is
         * 64 fs for 32-bit stereo frames, which is why 8 was believed right;
         * that reasoning assumes a frame size nothing has confirmed.
         */
        if (step == 0)
            Serial.println("\n--- bit clock divider sweep, 0x40080094[10:8]"
                           "  (field N = divide by N+1; the driver uses 7)");
        if (step > 7) { phase = -1; break; }
        sl6806_mmio_field(SL6806_AUD_BITCLK_REG, SL6806_AUD_BITCLK_DIV_SHIFT,
                          SL6806_AUD_BITCLK_DIV_BITS, (uint32_t)step);
        sl6806_mmio_set(SL6806_AUD_BITCLK_REG, SL6806_AUD_BITCLK_ENABLE);
        {
            /* step is 0..7 by construction, so the two digits are the whole
             * range - but say so rather than rely on the compiler agreeing. */
            static const char *const tags[8] = {
                "field 0 = /1", "field 1 = /2", "field 2 = /3", "field 3 = /4",
                "field 4 = /5", "field 5 = /6", "field 6 = /7", "field 7 = /8",
            };
            pace(tags[step & 7]);
        }
        step++;
        break;

    case 'd':
        /*
         * The vendor's stream start cycles this module rather than bare
         * enabling it, and sl6806_module.h records that the disable direction
         * is the dangerous one - so this only ever switches off an id it is
         * about to switch back on, in that order, and nothing else.
         */
        Serial.println("\n--- module 2 off-then-on, then measure");
        sl6806_module_disable(2);
        Serial.printf("  module 2 off, enabled: %s\n",
                      sl6806_module_enabled(2) ? "STILL ON" : "off");
        Serial.printf("  module 2 on: %s\n",
                      sl6806_module_enable(2) ? "acked" : "REFUSED");
        pace("after the cycle");
        phase = -1;
        break;

    default:
        phase = -1;
        break;
    }
}
