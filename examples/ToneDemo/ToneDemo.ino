/*
 * ToneDemo - push a sine wave through the audio DMA and find the output route.
 *
 * ---------------------------------------------------------------------
 *  MEASURED 2026-08-13: THE DMA RUNS. THE OUTPUT STAGE IS OFF.
 * ---------------------------------------------------------------------
 * The first version of this sketch got a complete transfer on the first try:
 *
 *     buffer 1: completion flag set after 1 ms, ctrl=0x12BC0910
 *
 * `0x12BC` is 4796, which is exactly the buffer length in bytes - so the
 * descriptor path is real, the length register holds, and the completion flag
 * comes back on its own. That is the whole data path working.
 *
 * It also made no sound, and the line above it says why:
 *
 *     DAC (+0x008) = 0x0
 *
 * The output route register was never written, by this sketch or by
 * sl6806_audio_begin(), so the DAC had no path selected. The corroborating
 * number is the timing: 4796 bytes of 48 kHz 16-bit stereo is **25 ms** of
 * audio, and it was completing in under one. A DMA that drains that fast is
 * not being paced by a sample clock; it is emptying into nothing.
 *
 * ---------------------------------------------------------------------
 *  AND THE SECOND RUN: THE ROUTES WERE NOT THE WHOLE STORY
 * ---------------------------------------------------------------------
 * With the route written and the DAC enabled, all four routes still came back
 * at a **mean of 10 microseconds**. 4796 bytes in 10 us is 480 MB/s on a
 * 64 MHz Cortex-M4 - not a transfer, a descriptor being retired as fast as it
 * can be read. Nothing was clocking the output stage.
 *
 * That run also had a bug worth knowing about: the sweep OR'd each route on
 * top of the last, because the vendor's setter is a read-modify-write and
 * this never cleared +0x08 between attempts. Route 1 should have been 0x2043
 * and read 0x20C3. Only route 0 was actually tested. Fixed here.
 *
 * The missing piece was the vendor's *stream start* at 0x00D9662C, a
 * different routine from the init - it enables module clock 2, sets a divider
 * of 8 into 0x40080094 and enables it, sets 0x40009400 bit 7, and moves one
 * of three 3-bit source fields from 4 to 6. The divider is the giveaway:
 * 24.576 MHz / 8 and 22.579 MHz / 8 are both exactly 64 fs, which is the bit
 * clock for 32-bit stereo frames.
 *
 * So this sketch now sweeps **three source modes against four routes**,
 * clearing +0x08 between each, and reports the mean completion time for all
 * twelve.
 *
 * WHAT TO WATCH FOR, in order of how much it settles:
 *
 *   1. **A completion time near 25000 us.** That is the DMA being paced by a
 *      real sample clock, which would mean the route is live and the rate is
 *      right. It is a stronger result than hearing something, because it is a
 *      number rather than an impression - and it says which route.
 *   2. **Sound**, on any route, even wrong-pitched or distorted.
 *   3. **All twelve still at 10 us.** The bit clock is not the whole story
 *      either. Next suspects: the module-2 clock needing the vendor's
 *      off-then-on cycle rather than a bare enable, the audio PLL not
 *      actually locking (the sketch prints CRU +0x10/+0x14 so you can see
 *      what it holds), or the amplifier enable this board has and nobody has
 *      found (the settings partition has a `spk_switch` key; §7k).
 *
 * Plug headphones in. The speaker may need an enable that is not in the
 * variant's pad map; the headphone jack is at least wired to something, since
 * on this board the lead doubles as the FM aerial.
 *
 * WHAT IS STILL A GUESS
 *
 *   - **The sample format.** 16-bit stereo little-endian is what the vendor's
 *     config struct looks like it carries, but the byte that would settle it
 *     comes from a caller this analysis did not follow. Wrong pitch or noise
 *     points here first: try mono, try 8-bit.
 *   - **The `trim` field** at +0x08 [12:8]. The vendor takes it from its own
 *     driver state; this passes 0.
 *   - **There is no interrupt.** The vendor refills from IRQ 30 at the
 *     three-quarter mark. This polls and re-submits, so it will gap between
 *     buffers even when everything else is right. A gapped tone is a success.
 *
 *     make SKETCH=examples/ToneDemo RUN_MODE=poll run
 *
 * In payload mode this cannot brick anything: flash is never written. It can
 * hang the device - unplug, hold the boot button, plug back in.
 */

#include <Arduino.h>
#include "sl6806_audio.h"

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
static unsigned route;
static unsigned mode;
static unsigned nbuf;
static unsigned long total_us;
static unsigned timeouts;

/*
 * How long one buffer takes, in microseconds. millis() was useless here: the
 * first run printed "0 ms" for every buffer, and the interesting question is
 * whether the answer is 0.5 ms or 25 ms.
 *
 * Bounded well under the host's ~1 s SCSI timeout, because in RUN_MODE=poll
 * this runs inside the boot ROM's USB command handler.
 */
static unsigned long playOne(void)
{
    unsigned long start;

    if (sl6806_audio_play(wave, sizeof(wave)) != 0)
        return 0;

    start = micros();
    while (micros() - start < 200000ul) {
        if (sl6806_audio_done())
            return micros() - start;
    }
    return 0;                           /* did not finish */
}

static void startCombo(unsigned m, unsigned r)
{
    Serial.println();
    Serial.print("--- source mode ");
    Serial.print(m);
    Serial.print(", route ");
    Serial.print(r);
    Serial.println(" ---");

    /*
     * Clear +0x08 first. The vendor's route setter is a read-modify-write and
     * expects to start from whatever its own driver left; sweeping without
     * clearing OR's each route on top of the last, which is what made the
     * previous run's routes 1..3 untested.
     */
    sl6806_mmio_write(SL6806_AUD_DAC, 0);
    sl6806_audio_route(0, r, 0);
    sl6806_audio_route(1, r, 0);
    sl6806_audio_clock_start(m);

    report("  DAC    (+0x008)", sl6806_mmio_read(SL6806_AUD_DAC));
    report("  bitclk 40080094", sl6806_mmio_read(SL6806_AUD_BITCLK_REG));

    nbuf = 0;
    total_us = 0;
    timeouts = 0;
}

void setup()
{
    Serial.begin(115200);
    Serial.println();
    Serial.println("=== SL6806 tone demo ===");
    Serial.println("440 Hz, 48 kHz, 16-bit stereo, into 0x40009000.");
    Serial.println("A buffer is 25.0 ms of audio. Watch the completion time:");
    Serial.println("near 25000 us means a sample clock is pacing the DMA.");
    Serial.println();

    fillTone();
    Serial.print("buffer at 0x");
    Serial.print((uint32_t)(uintptr_t)wave, HEX);
    Serial.print(", ");
    Serial.print((uint32_t)sizeof(wave));
    Serial.println(" bytes");

    up = sl6806_audio_begin(RATE) != 0;
    Serial.print("sl6806_audio_begin(48000): ");
    Serial.println(up ? "module clock acked, block configured"
                      : "MODULE CLOCK REFUSED - stop here, see AudioProbe");

    if (!up)
        return;

    /* Everything audible at once, so silence is not a muted channel. */
    for (unsigned ch = 0; ch < SL6806_AUD_TX_CHANNELS; ch++) {
        sl6806_audio_volume(ch, SL6806_AUD_VOL_MAX);
        sl6806_audio_mute(ch, 0);
    }

    report("PLL sel  (40080010)", sl6806_mmio_read(SL6806_AUD_PLL_SEL));
    report("PLL rat  (40080014)", sl6806_mmio_read(SL6806_AUD_PLL_RATIO));

    mode = 1;
    route = 0;
    startCombo(mode, route);
}

void loop()
{
    unsigned long us;

    if (!up)
        return;

    us = playOne();
    if (us)
        total_us += us;
    else
        timeouts++;
    nbuf++;

    if (nbuf < BUFFERS_PER_ROUTE)
        return;

    /*
     * One summary line per route rather than one per buffer. The first run
     * printed a line per buffer, outran the monitor's poll rate and lost
     * output - and forty identical lines say nothing one mean does not.
     */
    Serial.print("  ");
    Serial.print(nbuf);
    Serial.print(" buffers, mean ");
    Serial.print(total_us / nbuf);
    Serial.print(" us (25000 us would be real-time)");
    if (timeouts) {
        Serial.print(", ");
        Serial.print(timeouts);
        Serial.print(" never finished");
    }
    Serial.println();
    report("  ctrl (+0x108)", sl6806_mmio_read(SL6806_AUD_TX_CTRL));

    route++;
    if (route >= SL6806_AUD_ROUTES) {
        route = 0;
        mode++;
    }
    if (mode <= SL6806_AUD_SRC_MODES) {
        startCombo(mode, route);
        return;
    }

    Serial.println();
    Serial.println("twelve combinations tried. Two things to report:");
    Serial.println(" - any that sounded, and what it sounded like;");
    Serial.println(" - any whose mean moved off 10 us at all. 25000 us is");
    Serial.println("   real time; even 2000 would mean something is pacing.");
    sl6806_audio_clock_stop();
    sl6806_audio_end();
    up = false;
}
