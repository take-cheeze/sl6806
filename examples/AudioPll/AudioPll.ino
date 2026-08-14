/*
 * AudioPll - start the audio master clock the vendor's way, and see whether
 *            the DMA slows down to real time.
 *
 * [M] WHAT THE LENGTH SWEEP FOUND (examples/AudioLen). Completion time is
 * LINEAR IN LENGTH across a 500:1 span:
 *
 *     len      us   iters
 *       128     3      1
 *      2048     5      2
 *      4796    10      5
 *      8192    17      8
 *     16384    32     16
 *     64000   125     64        fit: us = 0.001926 * len + 1.43
 *
 * That is ~519 bytes/us with a fixed 1.4 us overhead, and the residuals are
 * negligible. **A descriptor that is retired without doing anything takes
 * constant time.** This does not. So something proportional to the data is
 * happening, and docs/AUDIO.md's central negative - "a descriptor is accepted
 * and retired in 10 us having moved nothing" - is VOID. It was one length,
 * measured many ways.
 *
 * What is true instead is far more useful: the DMA runs, at bus speed, and
 * **nothing paces it at the sample rate.** Real time for 48 kHz stereo 16-bit
 * is 192 KB/s; the measured 519 MB/s is 2667x too fast, and that ratio is
 * constant across every length. A DMA draining into a sink that never
 * back-pressures looks exactly like this.
 *
 * (519 MB/s is also not absurd here. 30 measured the PLL already running at
 * 192 MHz, and ~130 M words/s is about two thirds of a word per cycle on such
 * a bus - an ordinary DMA rate, not an impossible one. The 480 MB/s figure was
 * called impossible against a 64 MHz core, which is not the bus this chip has.)
 *
 * SO WHAT IS MISSING IS THE AUDIO MASTER CLOCK, and the vendor's bring-up for
 * it has never been performed by this framework. sl6806_audio_begin() does
 * module 37 and romclk 19 and sets the bit clock. The vendor's own path at
 * 0x00D9A224 does all of this first, and none of it appears anywhere in
 * cores/:
 *
 *     romclk_enable(0)
 *     0x00807300(24576000)        <- an AUDIO PLL, decoded below
 *     module_enable(87)
 *     romclk_enable(31)
 *     romclk_enable(56)
 *     0x4009B04C = (v & ~0x0F000000) | 0x0E000000
 *     0x4009B050 = 27
 *     0x4009B040 |= 0x80000000 | 1
 *
 * [V] 0x00807300 is a second PLL, and not the one sl6806_pwm.h documents at
 * 0x40080008:
 *
 *     freq == 24576000 -> mult 0x3126, mode 3      (48 kHz family)
 *     else                mult 0x186C2, mode 2     (44.1 kHz family)
 *     poll 0x40080014 bit 11 until clear
 *     0x40080010 = (v & ~0xFC1F0000) | 0x3000 | (mode << 5)
 *     0x40080014 = (v & 0x3800) | 0x80000000 | 0x400 | (mult << 14)
 *
 * The multiplier is selected by the audio rate. That is an audio PLL by
 * construction, and 0x4009B000 is a block this project has never named.
 *
 * THE TEST. Do the vendor's chain, then re-run the length sweep.
 *
 *   completion moves toward 25 ms for 4796 bytes -> the DAC is now pacing the
 *       DMA, audio is running, and this is the answer
 *   completion stays at ~10 us                   -> the clock chain is not
 *       what paces it, and the next question is what consumes the FIFO
 *
 * Either way the length linearity should persist; what changes is the slope.
 * 192 KB/s instead of 519 MB/s is a factor of 2667 - there is no way to
 * misread it, which is the point after five rounds of ambiguous eyeballing on
 * the PWM.
 *
 * [!] THIS WRITES A PLL. Every step is announced before it happens and the
 * call returns first, so the host has the line before the write - if a step
 * takes the device off the bus, the last line names it. That is
 * startup_payload.c's rule and this session has already lost one run to
 * ignoring it. The PLL at 0x40080008 has been written from a payload before
 * (14a, BtProbe, AudioWall) and survived; this is a different one at +0x10 and
 * +0x14, believed dedicated to audio, but that is inference and it is why the
 * announcements are there.
 *
 *     make SKETCH=examples/AudioPll RUN_MODE=poll run
 *
 * 'b' baseline sweep (no PLL), 'p' bring the clock chain up one step per
 * call, 's' sweep again, 'q' stop.
 */

#include <Arduino.h>
#include "sl6806_audio.h"
#include "sl6806_module.h"
#include "sl6806_romclk.h"
#include "sl6806_mmio.h"
#include "wiring_time.h"

#define RATE     48000u
#define CHANNELS 2
#define MAX_FRAMES 16000u
static int16_t wave[MAX_FRAMES * CHANNELS] __attribute__((aligned(4)));

/* 32768 timed out in the first sweep and 64000 did not, which is odd enough
 * to keep in the ladder and watch rather than quietly drop. */
static const uint32_t lens[] = { 2048u, 4796u, 8192u, 16384u, 32768u, 64000u };
#define NLEN ((int)(sizeof lens / sizeof lens[0]))

/* [V] 0x00D9A224's block, and 0x00807300's PLL. */
#define AUD_PLL_A       0x40080010u
#define AUD_PLL_B       0x40080014u
#define AUD_PLL_BUSY    (1u << 11)
#define AUD_B_BASE      0x4009B000u
#define AUD_MASTER_HZ   24576000ul

static bool up;
static int  step_i = -1;

static int romclk_on(unsigned id)
{
    unsigned i;

    for (i = 0; i < SL6806_ROMCLK_COUNT; i++)
        if (sl6806_romclk[i].id == id) {
            sl6806_mmio_set(sl6806_romclk[i].addr, sl6806_romclk[i].bit);
            return 1;
        }
    return 0;             /* not in the ROM's table - say so, do not pretend */
}

/* [V] 0x00807300, transcribed. Bounded: the vendor's poll is unbounded and
 * this runs inside the USB handler. */
static int audio_pll(unsigned long hz)
{
    uint32_t mult = (hz == AUD_MASTER_HZ) ? 0x3126u : 0x186C2u;
    uint32_t mode = (hz == AUD_MASTER_HZ) ? 3u : 2u;
    uint32_t v;
    unsigned i;

    for (i = 0; i < 10000u; i++)
        if (!(sl6806_mmio_read(AUD_PLL_B) & AUD_PLL_BUSY))
            break;
    if (i == 10000u)
        return 0;

    v = sl6806_mmio_read(AUD_PLL_A);
    sl6806_mmio_write(AUD_PLL_A, (v & ~0xFC1F0000u) | 0x3000u | (mode << 5));

    v = sl6806_mmio_read(AUD_PLL_B);
    sl6806_mmio_write(AUD_PLL_B,
                      (v & 0x3800u) | 0x80000000u | 0x400u | (mult << 14));
    return 1;
}

static void sweep(const char *tag)
{
    Serial.printf("\n--- length sweep: %s\n", tag);
    Serial.println("    len      us   iters   B/us     vs real time");
    for (int i = 0; i < NLEN; i++) {
        uint32_t start, us;
        unsigned n = 0;

        if (sl6806_audio_play(wave, lens[i]) != 0) {
            Serial.printf("    %5lu  REJECTED - not a measurement\n",
                          (unsigned long)lens[i]);
            continue;
        }
        start = micros();
        while ((us = micros() - start) < 60000ul) {
            n++;
            if (sl6806_audio_done())
                break;
        }
        Serial.printf("    %5lu  %6lu  %6u  %6lu   %lux fast\n",
                      (unsigned long)lens[i], (unsigned long)us, n,
                      us ? (unsigned long)(lens[i] / us) : 0ul,
                      us ? (unsigned long)((lens[i] * 1000ul / 192ul) / us) : 0ul);
    }
    Serial.println("    real time is 1x. 2667x means nothing paces the DMA.");
}

void setup()
{
    unsigned i;

    Serial.begin(115200);
    Serial.println();
    Serial.println("=== SL6806 audio: the master clock the vendor starts ===");
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
    Serial.println("'b' baseline sweep, 'p' clock chain (7 steps), 's' sweep, 'q'");
}

void loop()
{
    int key = Serial.read();

    if (!up)
        return;

    if (key == 'b') { sweep("baseline, no audio PLL"); return; }
    if (key == 's') { sweep("after the vendor's clock chain"); return; }
    if (key == 'q') { step_i = -1; return; }
    if (key == 'p') { step_i = 0; }

    if (step_i < 0)
        return;

    /* One step per call, each announced by the PREVIOUS call, so the console
     * line is already drained before the write happens. */
    switch (step_i) {
    case 0:
        Serial.println("\n  next: romclk_enable(0)");                  break;
    case 1:
        Serial.printf("  romclk 0: %s\n", romclk_on(0) ? "set" : "NOT IN TABLE");
        Serial.println("  next: audio PLL 0x40080010/14 <- 24.576 MHz");break;
    case 2:
        Serial.printf("  audio PLL: %s   A %08x  B %08x\n",
                      audio_pll(AUD_MASTER_HZ) ? "written" : "BUSY TIMEOUT",
                      (unsigned)sl6806_mmio_read(AUD_PLL_A),
                      (unsigned)sl6806_mmio_read(AUD_PLL_B));
        Serial.println("  next: module_enable(87)");                   break;
    case 3:
        Serial.printf("  module 87: %s\n",
                      sl6806_module_enable(87) ? "acked" : "REFUSED");
        Serial.println("  next: romclk_enable(31), romclk_enable(56)");break;
    case 4:
        Serial.printf("  romclk 31: %s   romclk 56: %s\n",
                      romclk_on(31) ? "set" : "NOT IN TABLE",
                      romclk_on(56) ? "set" : "NOT IN TABLE");
        Serial.println("  next: 0x4009B04C / +0x50 / +0x40");          break;
    case 5:
        sl6806_mmio_field(AUD_B_BASE + 0x4C, 24, 4, 0xEu);
        sl6806_mmio_write(AUD_B_BASE + 0x50, 27u);
        sl6806_mmio_set(AUD_B_BASE + 0x40, 0x80000000u | 1u);
        Serial.printf("  0x4009B000: +40 %08x  +4C %08x  +50 %08x\n",
                      (unsigned)sl6806_mmio_read(AUD_B_BASE + 0x40),
                      (unsigned)sl6806_mmio_read(AUD_B_BASE + 0x4C),
                      (unsigned)sl6806_mmio_read(AUD_B_BASE + 0x50));
        Serial.println("  chain up. press 's' to sweep again.");       break;
    default:
        step_i = -1;
        return;
    }
    step_i++;
}
