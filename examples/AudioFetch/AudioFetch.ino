/*
 * AudioFetch - walk every clock id, and ask whether the DMA has started
 *              reading memory.
 *
 * [M] WHERE THIS STARTS. The audio block accepts a descriptor, counts its
 * length down at ~536 B/us, and never reads its source: the retire time is
 * IDENTICAL from SRAM and from XIP flash, and flash is off-chip SPI at 32 MHz
 * where those bytes cannot be read in under 300 us. Consistent over a 1440:1
 * length range, 16 bytes to 23040. Every route, source mode, module gate,
 * 0x400E0000 bit, bit clock divider and source bit, the vendor's wider clock
 * chain, the audio PLL, the START order and a 16-byte primer are all negative.
 * The descriptor itself is bit-exact against the vendor's. docs/AUDIO.md has
 * the ledger, retractions included.
 *
 * So the question is no longer what paces the DMA. It is what lets the DMA
 * reach the bus at all - and that is an earlier question than this
 * investigation has been asking.
 *
 * THE DETECTOR IS THE SOURCE-MEMORY TEST, not the pacing one, and the choice
 * matters. Pacing needs two things to be true: the engine must fetch AND the
 * DAC must consume at 48 kHz. Fetching needs only the first. So a source-speed
 * difference will appear the moment the bus master wakes, even if nothing
 * downstream of it is ready - which makes it strictly the more sensitive
 * instrument for this question, and the one that fails first.
 *
 *     SRAM and flash the same  ->  still not reading
 *     flash slower than SRAM   ->  IT IS READING MEMORY
 *
 * WHY A WALK. 14a walked all 128 module ids once and got nothing, but it
 * scored on CTRL bit 28 of the PWM, which is not a run flag - that walk is
 * recorded as void in sl6806_re_notes.md 32. **No walk has ever been run
 * against the audio block with a working detector.** This is that walk, over
 * both clock families, with an instrument whose signal is a factor of thirty.
 *
 * SAFETY. Ids are only ever switched ON. sl6806_module.h records that the
 * disable direction is the dangerous one, and this sketch never uses it. Each
 * id is announced and the call RETURNS before the write happens, so if one of
 * them takes the device off the bus the last line names it - the rule
 * startup_payload.c gives and that this session has already lost one run to
 * ignoring.
 *
 *     make SKETCH=examples/AudioFetch RUN_MODE=poll run
 *
 *   'b' baseline, and prove the detector separates nothing yet
 *   'm' walk module ids 0..127
 *   'r' walk the mask ROM's romclk table
 *   'q' stop
 *
 * The walk is cumulative on purpose: clocks stay on once enabled, so a
 * combination that only works together is still reachable. That also means a
 * hit names the LAST id, not necessarily the only one that mattered - re-run
 * from cold and enable just that one to confirm.
 */

#include <Arduino.h>
#include "sl6806_audio.h"
#include "sl6806_module.h"
#include "sl6806_romclk.h"
#include "sl6806_mmio.h"

#define RATE     48000u
#define TESTLEN  4796u
static int16_t wave[TESTLEN / 2] __attribute__((aligned(4)));

#define FLASH_SRC 0x00C10000u    /* XIP, off-chip SPI - far slower than SRAM */

static bool up;
static int  phase = -1;
static int  idx, hits;
static bool announced;

/* The gap the block needs between descriptors. Without it the SECOND submit
 * times out - measured on all ten rows of ToneDemo's first pace(), and it is
 * why the first version of this sketch reported all 184 ids as hits. */
static void settle(void)
{
    unsigned long t = micros();

    while (micros() - t < 2000ul)
        ;
}

static unsigned long timeOne(const void *buf)
{
    unsigned long t0, us;

    if (sl6806_audio_play(buf, TESTLEN) != 0)
        return 0;                      /* rejected: never a measurement */
    t0 = micros();
    while ((us = micros() - t0) < 60000ul)
        if (sl6806_audio_done())
            break;
    return us;
}

/* Returns 1 if the two sources differ enough to mean the engine is reading.
 * Both are printed by the caller so a null result is visible as two equal
 * numbers rather than as a bare verdict. */
static int fetches(unsigned long *sram, unsigned long *flash)
{
    *sram  = timeOne(wave);
    settle();
    *flash = timeOne((const void *)(uintptr_t)FLASH_SRC);
    settle();
    if (!*sram || !*flash)
        return 0;                      /* rejected: never a measurement */
    /* A TIMEOUT IS NOT A SLOW READ. The bound means the descriptor never
     * retired at all, which is a different fault and must not score as a hit -
     * scoring it is exactly what made every row of the first run a "discovery". */
    if (*sram >= 60000ul || *flash >= 60000ul)
        return 0;
    /* Reading 4796 bytes over 32 MHz SPI is >= 300 us even in quad mode,
     * against ~10 us not reading. Half again is far outside the noise, which
     * the logs show as +-1 us. */
    return (*flash > *sram + (*sram / 2u) + 5u);
}

/*
 * The control, and it can fail. With nothing enabled the two sources must give
 * the SAME time - that is the known state, established over a 1440:1 length
 * range. If they differ before a single clock has been touched, the instrument
 * is measuring itself and no row below it means anything.
 */
static int baseline_ok(void)
{
    unsigned long a, b;
    int hit = fetches(&a, &b);

    Serial.printf("  baseline   SRAM %5lu us   flash %5lu us\n", a, b);
    if (!a || !b) {
        Serial.println("  [!] a row did not complete - the detector cannot run");
        return 0;
    }
    if (hit) {
        Serial.println("  [!] THE BASELINE ALREADY 'READS MEMORY'. It cannot -");
        Serial.println("      nothing has been enabled. The instrument is broken");
        Serial.println("      and the walk is refused. Do not read past here.");
        return 0;
    }
    Serial.println("  equal - the detector works and the answer is 'not yet'");
    return 1;
}

static void row(const char *what, int id)
{
    unsigned long a, b;
    int hit = fetches(&a, &b);

    if (hit || (id % 16) == 0)
        Serial.printf("  %s %3d   SRAM %5lu us   flash %5lu us%s\n",
                      what, id, a, b, hit ? "   <<< READS MEMORY" : "");
    if (hit)
        hits++;
}

void setup()
{
    unsigned i;

    Serial.begin(115200);
    Serial.println();
    Serial.println("=== SL6806 audio: what lets the DMA reach the bus? ===");
    for (i = 0; i < TESTLEN / 2; i++)
        wave[i] = (int16_t)(i * 977u);          /* anything non-constant */

    up = sl6806_audio_begin(RATE) != 0;
    Serial.print("audio begin: ");
    Serial.println(up ? "ok" : "MODULE CLOCK REFUSED - stop here");
    if (!up)
        return;
    sl6806_audio_route(0, 1, 0);
    sl6806_audio_route(1, 1, 0);
    sl6806_audio_clock_start(1);
    Serial.println("'b' baseline  'm' module ids  'r' romclk ids"
                   "  'c' census the audio block\n"
                   "'a' does TX_ADDR ADVANCE?  'q' stop");
}

void loop()
{
    int key = Serial.read();

    if (!up)
        return;
    if (key == 'q') { phase = -1; Serial.println("stopped."); return; }
    if (key == 'b' || key == 'm' || key == 'r' || key == 'c' || key == 'a') {
        phase = key; idx = 0; hits = 0; announced = false;
        Serial.printf("\n[%c]\n", key);
    } else if (key >= 0 && key != '\r' && key != '\n') {
        Serial.printf("unknown key '%c' - try b m r c a q\n", key);
        return;
    }
    if (phase < 0)
        return;

    switch (phase) {
    case 'b':
        phase = -1;
        (void)baseline_ok();
        break;

    case 'm':
        /* The walk refuses to run on a broken detector. The first version
         * printed the same "READS MEMORY" verdict on its baseline as on its
         * 184 rows, so the control could not contradict the result - which is
         * not a control at all. */
        if (idx == 0 && !announced && !baseline_ok()) { phase = -1; break; }
        if (idx > 127) {
            Serial.printf("  128 module ids, %d hit(s)\n", hits);
            phase = -1;
            break;
        }
        /* Announce, return, then act - so the console has the id before the
         * write that might take the device off the bus. */
        if (!announced) {
            if ((idx % 16) == 0)
                Serial.printf("  ... module %d\n", idx);
            announced = true;
            break;
        }
        sl6806_module_enable((unsigned)idx);     /* only ever ON */
        row("module", idx);
        idx++;
        announced = false;
        break;

    case 'r':
        if (idx == 0 && !announced && !baseline_ok()) { phase = -1; break; }
        if (idx >= SL6806_ROMCLK_COUNT) {
            Serial.printf("  %d romclk ids, %d hit(s)\n",
                          SL6806_ROMCLK_COUNT, hits);
            phase = -1;
            break;
        }
        if (!announced) {
            if ((idx % 16) == 0)
                Serial.printf("  ... romclk entry %d (id %u)\n",
                              idx, sl6806_romclk[idx].id);
            announced = true;
            break;
        }
        sl6806_mmio_set(sl6806_romclk[idx].addr, sl6806_romclk[idx].bit);
        row("romclk id", (int)sl6806_romclk[idx].id);
        idx++;
        announced = false;
        break;

    case 'c': {
        /*
         * [M] THE CLOCK SPACE IS EXHAUSTED. Both walks came back 0 of 184, and
         * they are cumulative - by the end every module id and every romclk id
         * was ON, and the DMA still never touched its source. Whatever stops it
         * fetching is not a clock.
         *
         * So census the block itself. Its thirty registers have had their RESET
         * VALUES recorded and nothing more: nobody has ever asked which of their
         * bits are writable. That is the question that found four hidden bits in
         * the PWM's pair register after a year of it being "known", and bit 4 of
         * the audio bit clock.
         *
         * One bit at a time with a restore after each, which is slower than
         * all-ones and cannot repeat the 0x3F0F mistake of probing only the bits
         * it already expected.
         */
        static const struct { const char *name; uint32_t reg; } regs[] = {
            { "+0x000 ctrl ", SL6806_AUD_REG(0x000) },
            { "+0x008 DAC  ", SL6806_AUD_DAC },
            { "+0x07C src  ", SL6806_AUD_REG(0x07C) },
            { "+0x080 src  ", SL6806_AUD_REG(0x080) },
            { "+0x100 TXen ", SL6806_AUD_TX_ENABLE },
            { "+0x104 TXtrg", SL6806_AUD_TX_TRIG },
            { "+0x108 TXctl", SL6806_AUD_TX_CTRL },
            { "+0x10C TXadr", SL6806_AUD_TX_ADDR },
            { "+0x200 RXen ", SL6806_AUD_RX_ENABLE },
            { "+0x208 RXctl", SL6806_AUD_RX_CTRL },
        };
        unsigned r;

        Serial.println("\n--- audio block census: which bits are writable?");
        Serial.println("    reset values were recorded long ago; writability");
        Serial.println("    never was. A fetch enable would be a bit here that");
        Serial.println("    holds and that nothing in the driver writes.");
        for (r = 0; r < sizeof regs / sizeof regs[0]; r++) {
            uint32_t saved = sl6806_mmio_read(regs[r].reg);
            uint32_t mask = 0;
            unsigned b;

            for (b = 0; b < 32; b++) {
                uint32_t m = 1u << b;

                sl6806_mmio_write(regs[r].reg, saved | m);
                if (sl6806_mmio_read(regs[r].reg) & m)
                    mask |= m;
                sl6806_mmio_write(regs[r].reg, saved);
            }
            Serial.printf("    %s holds %08x  implemented %08x  unwritten %08x\n",
                          regs[r].name, (unsigned)saved, (unsigned)mask,
                          (unsigned)(mask & ~saved));
        }
        Serial.println("    the 'unwritten' column is the search space.");
        phase = -1;
        break;
    }

    case 'a': {
        /*
         * [M] THE SOURCE TEST WAS VOID, and the census is what caught it.
         * +0x10C implements bits [19:2] and bit 23 ONLY - bits 20..22 are
         * absent - so it reaches SRAM and the low megabyte and CANNOT HOLD A
         * FLASH ADDRESS. Writing 0x00C10000 leaves 0x00810000, which is
         * exactly what the register was found holding. Both rows of that test
         * read SRAM, and their identical times were guaranteed by the hardware
         * truncating the address rather than by anything the DMA did.
         *
         * So "the DMA does not read its source" is retracted. With both
         * reachable regions on-chip there is no source-speed comparison left
         * to make - but there is a better test, and it needs no second memory:
         *
         *     if the engine walks the buffer, the address register moves.
         *
         * Write it, run one transfer, read it back. An engine that fetches
         * leaves it advanced by the length; a counter leaves it where it was.
         */
        uint32_t before, after;
        unsigned long us;

        Serial.println("\n--- does TX_ADDR advance across a transfer?");
        sl6806_mmio_write(SL6806_AUD_TX_ADDR, (uint32_t)(uintptr_t)wave);
        before = sl6806_mmio_read(SL6806_AUD_TX_ADDR);
        if (before != ((uint32_t)(uintptr_t)wave & 0x008FFFFCu)) {
            Serial.printf("    wrote %08x, reads %08x - not even the truncated"
                          " value; stop here\n",
                          (unsigned)(uintptr_t)wave, (unsigned)before);
            phase = -1;
            break;
        }
        us = timeOne(wave);
        after = sl6806_mmio_read(SL6806_AUD_TX_ADDR);
        settle();

        Serial.printf("    before %08x   after %08x   delta %ld   (len %u, %lu us)\n",
                      (unsigned)before, (unsigned)after,
                      (long)(after - before), TESTLEN, us);
        if (after == before)
            Serial.println("    UNCHANGED - the engine did not walk the buffer.");
        else if (after - before == TESTLEN)
            Serial.println("    ADVANCED BY THE LENGTH - IT READS MEMORY.");
        else
            Serial.println("    moved, but not by the length - worth explaining");
        phase = -1;
        break;
    }

    default:
        phase = -1;
        break;
    }
}
