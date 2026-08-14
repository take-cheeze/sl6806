/*
 * AudioBeep - the smallest thing that asks the audio block for a sound.
 *
 * ---------------------------------------------------------------------
 *  [M] THIS IS SILENT TODAY, AND IT SAYS SO WHEN IT RUNS.
 * ---------------------------------------------------------------------
 * The audio block at 0x40009000 is fully configured by sl6806_audio_begin()
 * and does not fetch. Measured 2026-08-14: a TX descriptor retires in a time
 * proportional to its length but **identical whether the source is SRAM or
 * XIP flash** - and XIP flash is off-chip SPI at 32 MHz, where 4796 bytes
 * cannot be read in under 300 us, let alone the 9 us measured. A transfer's
 * time depends on how fast its source can be read; this one does not depend on
 * it at all. So the length register is being counted down and nothing is
 * fetched. docs/AUDIO.md has the whole ledger, including two conclusions of
 * mine that were retracted on the way to that one.
 *
 * So why does this sketch exist?
 *
 *   1. It is the reference for what the API looks like. examples/ToneDemo is
 *      now seven phases of instrumentation and is unreadable as an example;
 *      this is twenty lines of "how do I play a buffer".
 *   2. It is the regression that will announce success. It does not claim to
 *      have worked - it MEASURES whether the hardware consumed the buffer at
 *      real time, and prints a verdict either way. The day someone finds what
 *      makes the block fetch, this sketch says so without being modified.
 *
 * That second point is the whole design. A demo that prints "playing!" and
 * makes no sound is worse than no demo, because it invites the reader to go
 * looking for a fault in their headphones. This one reports the number.
 *
 * HOW IT DECIDES. A buffer of N bytes of 48 kHz 16-bit stereo is N/192 ms of
 * audio. If the hardware is really playing it, the completion cannot arrive
 * before that - a DAC consuming at 48 kHz paces the DMA, and that is exactly
 * what "it works" means here. Completion in microseconds means the descriptor
 * was retired without the audio ever existing.
 *
 *     make SKETCH=examples/AudioBeep RUN_MODE=poll run
 *
 * Plug headphones in. On this board the lead doubles as the FM aerial, so the
 * jack is at least wired to something; the speaker may need an enable that is
 * not in the variant's pad map.
 */

#include <Arduino.h>
#include "sl6806_audio.h"

#define RATE      48000u
#define CHANNELS  2u

/* An A major arpeggio, because four notes is enough to tell a tune from a
 * buzz and short enough to hold in .bss without thinking about it. */
static const unsigned notes[] = { 440u, 554u, 659u, 880u };
#define NNOTES ((int)(sizeof notes / sizeof notes[0]))

/* 120 ms a note: 5760 frames, 23040 bytes - inside the descriptor's 16-bit
 * length field, and inside the poll bound below even when it is paced. */
#define FRAMES    5760u
#define NOTE_MS   (FRAMES * 1000u / RATE)
static int16_t wave[FRAMES * CHANNELS] __attribute__((aligned(4)));

/* Quarter-wave sine, so this needs neither libm nor float. */
static const int16_t quarter[17] = {
        0,  3212,  6393,  9512, 12539, 15446, 18204, 20787,
    23170, 25330, 27245, 28898, 30273, 31357, 32138, 32610, 32767
};

static int16_t sine(unsigned phase, unsigned period)
{
    unsigned step = (phase * 64u) / period;
    unsigned q = step & 15u;

    switch ((step >> 4) & 3u) {
    case 0:  return quarter[q];
    case 1:  return quarter[16u - q];
    case 2:  return (int16_t)-quarter[q];
    default: return (int16_t)-quarter[16u - q];
    }
}

static void fill(unsigned hz)
{
    unsigned period = RATE / hz;
    unsigned i;

    for (i = 0; i < FRAMES; i++) {
        /* Half scale: a first tone into an unknown amplifier, and into a pair
         * of earbuds, should not be full scale. */
        int16_t s = (int16_t)(sine(i % period, period) / 2);

        wave[i * CHANNELS + 0] = s;
        wave[i * CHANNELS + 1] = s;
    }
}

static bool up;
static int  note = -1;
static unsigned long fastest = 0xFFFFFFFFul;

void setup()
{
    Serial.begin(115200);
    Serial.println();
    Serial.println("=== SL6806 audio beep ===");

    up = sl6806_audio_begin(RATE) != 0;
    Serial.print("audio begin: ");
    Serial.println(up ? "ok" : "MODULE CLOCK REFUSED - see examples/AudioProbe");
    if (!up)
        return;

    /* Route both channels to the DAC and open the output at full volume. */
    sl6806_audio_route(0, 1, 0);
    sl6806_audio_route(1, 1, 0);
    for (unsigned ch = 0; ch < SL6806_AUD_TX_CHANNELS; ch++) {
        sl6806_audio_volume(ch, SL6806_AUD_VOL_MAX);
        sl6806_audio_mute(ch, 0);
    }
    sl6806_audio_clock_start(1);

    Serial.printf("four notes, %u ms each. press 'p' to play\n", NOTE_MS);
    Serial.println("a note that really plays cannot complete faster than it");
    Serial.println("lasts - that is the test, and it is printed per note.");
}

void loop()
{
    unsigned long start, us;

    if (!up)
        return;

    if (Serial.read() == 'p')
        note = 0;
    if (note < 0 || note >= NNOTES) {
        if (note >= NNOTES) {
            /*
             * The verdict. Real time is the floor: 23040 bytes of 48 kHz
             * stereo is 120 ms and no correct implementation can retire it
             * sooner. Anything far below that is the descriptor being counted
             * down with no audio behind it.
             */
            Serial.println();
            if (fastest * 2ul >= (unsigned long)NOTE_MS * 1000ul) {
                Serial.println("PACED AT REAL TIME - the block is playing.");
                Serial.println("If you heard nothing, the fault is now after");
                Serial.println("the DMA: routing, the amplifier enable, or the");
                Serial.println("jack. That is a different search and a shorter one.");
            } else {
                Serial.printf("NOT PLAYING. Fastest note retired in %lu us; %u ms"
                              " of audio cannot.\n", fastest, NOTE_MS);
                Serial.println("The block counts the length down and fetches");
                Serial.println("nothing - see docs/AUDIO.md. This is expected");
                Serial.println("today and is not a fault in your headphones.");
            }
            note = -1;
        }
        return;
    }

    fill(notes[note]);
    if (sl6806_audio_play(wave, sizeof(wave)) != 0) {
        Serial.println("play() refused the buffer - alignment or length");
        note = NNOTES;
        return;
    }

    /* 300 ms: comfortably past the 120 ms a paced note needs, and still well
     * inside the host's ~1 s SCSI timeout that RUN_MODE=poll must respect. */
    start = micros();
    while ((us = micros() - start) < 300000ul)
        if (sl6806_audio_done())
            break;

    if (us < fastest)
        fastest = us;
    Serial.printf("  %4u Hz  %5lu bytes  retired in %6lu us  (%u ms of audio)\n",
                  notes[note], (unsigned long)sizeof(wave), us, NOTE_MS);
    note++;
}
