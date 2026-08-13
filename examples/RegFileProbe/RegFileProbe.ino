/*
 * RegFileProbe - hunt the SL6806's indexed register file on hardware.
 *
 * WHAT THIS IS LOOKING FOR
 *
 * The vendor firmware reaches a byte-wide register file through two SRAM
 * veneers - `read(index)` at 0x00804EAC and `write(index, value)` at
 * 0x00804E44 - which between them account for 140 call sites, including
 * every clock channel and, most likely, whatever switches the camera's
 * supply. Register numbers run 0x00..0x82 and values are masked as bytes
 * (`uxtb`, `and #0x5d`, `orn #0x7f`), which is the shape of a PMU or analog
 * register bank rather than an ordinary MMIO block.
 *
 * Where it is cannot be answered from the dumps. Those two addresses are
 * veneers whose targets are written at run time, and docs/sl6806_re_notes.md
 * §7h records the five searches across dump.bin, maskrom.bin and sram.bin
 * that establish there is nothing static to find. So the question moves to
 * hardware: read the candidates and see which one answers like a register
 * file.
 *
 * THE CANDIDATES, and where they come from
 *
 * Counting 32-bit literals in the mask ROM gives its own peripheral map,
 * independent of the FIRM-derived list in §7c. The two bases that are heavily
 * used by the ROM and absent from that list are the interesting ones - the
 * ROM is exactly the code that would own a power/clock register file, since
 * it has to bring the chip up before anything else exists.
 *
 * ---------------------------------------------------------------------
 *  READ THIS FIRST - the same warning examples/MmioProbe carries
 * ---------------------------------------------------------------------
 * Reading an unmapped address on a Cortex-M raises a BusFault, and in payload
 * mode the boot ROM owns the fault handler, so the likely outcome is that the
 * device stops responding. That is not damage: unplug, hold the boot button,
 * plug back in. Flash is never touched here.
 *
 * THIS SKETCH ONLY READS. Not one write. A write to a power or clock register
 * can switch the core off, and the whole point of the file being hunted is
 * that it plausibly controls rails.
 *
 * Every address is printed BEFORE it is read, and the console is flushed, so
 * if the device dies mid-scan the last line in the monitor names the address
 * that killed it. That is the most useful thing a probe like this can leave
 * behind, and it is why the scan is paced one small block per loop() call
 * rather than run to completion inside one.
 *
 *     make SKETCH=examples/RegFileProbe run
 *
 * WHAT COUNTS AS A HIT. A register file of ~131 byte-wide registers should
 * read back as varied small values - not all zero, not all 0xFFFFFFFF, and
 * not a handful of values repeating. The summary at the end scores each base
 * on exactly that, but the raw dump is printed too, because "what does it
 * actually contain" is the question a score cannot answer.
 */

#include <Arduino.h>
#include "sl6806_console.h"

/* How many indices the firmware is known to use: 0x00..0x82. */
#define REG_COUNT      0x83

/* Registers per loop() call. Small, so a fault names a narrow range and the
 * console never has to swallow more than a few lines at once. */
#define REGS_PER_PASS  8

typedef struct {
    uint32_t base;
    const char *note;
} candidate_t;

/*
 * Ordered by how much the mask ROM uses them, with the two the FIRM-derived
 * map never saw first - those are the ones this sketch exists to test. The
 * clock and reset unit is last and is the control: it is known to be real
 * (§7c), so if it does not read back sensibly, the sketch is broken rather
 * than the chip being interesting.
 */
static const candidate_t candidates[] = {
    { 0x40080000u, "the clock and reset unit - the CONTROL, known real" },
    { 0x40036000u, "92 ROM refs, absent from the FIRM-derived map"      },
    { 0x4010E000u, "65 ROM refs, absent from the FIRM-derived map"      },
    { 0x40040000u, "125 ROM refs, the most used of all"                 },
    { 0x40030000u, "87 ROM refs; FIRM's heavily-used 16-bit block"      },
    { 0x40020000u, "70 ROM refs"                                        },
};
#define NCANDIDATES ((int)(sizeof(candidates) / sizeof(candidates[0])))

/*
 * ===================================================================
 *  BUILD THIS WITH RUN_MODE=poll
 * ===================================================================
 *     make SKETCH=examples/RegFileProbe RUN_MODE=poll run
 *
 * Not optional advice. This probe is deliberately made of very small steps -
 * one address announced per loop() call, one read block per call - and in the
 * default run mode loop() is driven by the boot ROM's idle callback, which on
 * this unit has been measured as low as a fraction of a call per second. The
 * first two attempts at this scan looked exactly like a BusFault: output
 * stopped after the banner and the device went quiet. It was not a fault. It
 * was a probe needing a hundred calls being given about one a second, and a
 * monitor giving up after ten seconds of silence. In poll mode loop() runs at
 * the USB poll rate and the whole scan finishes in seconds.
 *
 * WHAT THE HARDWARE SAID, so nobody re-runs it to find out:
 *
 *   0x40080000  clock and reset unit, the control - varied, plausible
 *   0x40036000  reads all-zero
 *   0x4010E000  reads all-zero
 *   0x40040000  VARIED AND LIVE - a real peripheral, confirmed
 *   0x40030000  reads all-zero
 *   0x40020000  reads all-zero
 *
 * Nothing faulted. All-zero means gated off in bootloader mode at least as
 * plausibly as absent, so a zero here is not evidence that a peripheral does
 * not exist - 0x40030000 in particular is heavily used by FIRM (§7c).
 */
#define START_AT 0

static int      cand;          /* which candidate                     */
static unsigned idx;           /* register index within it            */
static bool     done;

/* Per-candidate tallies, for the verdict at the end. */
static unsigned n_zero, n_ones, n_bytes, n_distinct;
static uint32_t first_seen[8];
static unsigned n_first;

static void beginCandidate(void)
{
    Serial.println();
    Serial.print("=== base 0x");
    Serial.print(candidates[cand].base, HEX);
    Serial.print("  (");
    Serial.print(candidates[cand].note);
    Serial.println(")");
    Serial.flush();

    idx = 0;
    n_zero = n_ones = n_bytes = n_distinct = n_first = 0;
}

/* Remember up to eight distinct values, enough to tell "varied" from "one
 * value repeating" without keeping a table of 131. */
static void tally(uint32_t v)
{
    unsigned i;

    if (v == 0)          n_zero++;
    if (v == 0xFFFFFFFFu) n_ones++;
    if (v <= 0xFFu)      n_bytes++;

    for (i = 0; i < n_first; i++)
        if (first_seen[i] == v)
            return;
    if (n_first < 8)
        first_seen[n_first++] = v;
    n_distinct++;
}

static void verdict(void)
{
    Serial.print("  -> ");
    Serial.print(n_zero);
    Serial.print(" zero, ");
    Serial.print(n_ones);
    Serial.print(" all-ones, ");
    Serial.print(n_bytes);
    Serial.print(" byte-sized, ");
    Serial.print(n_distinct >= 8 ? 8 : n_distinct);
    Serial.println(n_distinct >= 8 ? "+ distinct values" : " distinct values");

    if (n_ones >= REG_COUNT - 2) {
        Serial.println("     reads as all-ones: nothing is decoding this window.");
    } else if (n_zero >= REG_COUNT - 2) {
        Serial.println("     reads as all-zero: either unpowered, gated off, or");
        Serial.println("     not a register file.");
    } else if (n_distinct <= 2) {
        Serial.println("     one or two values repeating - a decoded window, but not");
        Serial.println("     131 independent registers.");
    } else if (n_bytes >= (REG_COUNT * 3) / 4) {
        Serial.println("     VARIED AND BYTE-SIZED ACROSS 0x00..0x82. That is what the");
        Serial.println("     firmware's masking implies the register file looks like.");
        Serial.println("     Worth following: compare the indices the clock driver");
        Serial.println("     touches - 0x03, 0x13, 0x16, 0x1B, 0x2C, 0x47.");
    } else {
        Serial.println("     varied, but wider than byte-sized - a real peripheral,");
        Serial.println("     though not obviously this register file.");
    }
}

/* Hold off printing while the console ring is more than half full, so a long
 * dump cannot outrun the host's polling and lose the very lines that matter.
 * Same lesson as examples/CameraDemo. */
static int roomToPrint(void)
{
    return sl6806_console_space() >= (int)(SL6806_CONSOLE_SIZE / 2);
}

void setup()
{
    Serial.begin(115200);
    Serial.println();
    Serial.println("=== SL6806 indexed register file probe ===");
    Serial.println("Read-only. Looking for ~131 byte-wide registers behind the");
    Serial.println("veneers at 0x00804EAC / 0x00804E44 - see notes 7h.");
    Serial.println("Each address is printed before it is read, so if this stops,");
    Serial.println("the last address shown is the one that faulted.");
    Serial.flush();

    cand = 0;
    beginCandidate();
}

/*
 * ANNOUNCING AN ADDRESS IS NOT THE SAME AS DELIVERING IT.
 *
 * The first version printed the address, called Serial.flush(), and read. It
 * lost the address anyway: flush() cannot make the host poll, so the line was
 * still sitting in the ring when the fault killed USB, and the run came back
 * naming the base but not the offset.
 *
 * Waiting for the ring to drain cannot be done by spinning either - in the
 * default run mode loop() *is* the ROM's idle callback, so blocking here
 * stops the USB service that would drain it, and the wait would deadlock.
 *
 * So the wait is a state instead: print, return, and only do the read on a
 * later call once the ring has actually emptied. Then a fault leaves the
 * exact address as the last line the host received.
 */
static bool announced;

void loop()
{
    unsigned n;

    if (done)
        return;

    if (!announced) {
        if (!roomToPrint())
            return;
        Serial.print("  +0x");
        Serial.print(idx * 4, HEX);
        Serial.println(" ..");
        announced = true;
        return;                  /* let the host collect it */
    }

    /* Ring empty means the host has taken everything, including the line
     * above. Only now is it safe to touch an address that may not decode. */
    if (sl6806_console_space() < (int)SL6806_CONSOLE_SIZE)
        return;

    announced = false;

    for (n = 0; n < REGS_PER_PASS && idx < REG_COUNT; n++, idx++) {
        uint32_t v = *(volatile uint32_t *)(candidates[cand].base + idx * 4);

        tally(v);
        Serial.print(" 0x");
        Serial.print(v, HEX);
    }
    Serial.println();

    if (idx >= REG_COUNT) {
        verdict();
        if (++cand < NCANDIDATES) {
            beginCandidate();
        } else {
            Serial.println();
            Serial.println("all candidates read. Nothing was written.");
            done = true;
        }
    }
}
