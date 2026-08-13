/*
 * AudioWindow - which of three enables changes the audio block's registers,
 * and what else is behind the change.
 *
 * ---------------------------------------------------------------------
 *  WHY: THE EQ HOLD DID SOMETHING, AND IT WAS NOT WHAT WAS WANTED
 * ---------------------------------------------------------------------
 * examples/ToneDemo built with -DTONEDEMO_EQ_HOLD=1 holds the three enables
 * the vendor's init borrows around its coefficient-RAM clear: module clock
 * 32, romclk 45, and bit 2 of 0x40000020. That was the first thing in five
 * hardware runs to change the block's behaviour at all - and every change
 * pointed the wrong way:
 *
 *     +0x400 went from 0x00000000 to 0x003378B1
 *     +0x108 read back 0x00BC1F7A, not 0x12BC0910
 *     +0x008 route 0 read 0x00832083, not 0x20832083
 *     mean completion time 10 us -> 3 us
 *
 * The two register changes are one change. We write 4796 = 0x12BC into the
 * length field at +0x108 [31:16] and read back 0x00BC - the top byte stopped
 * holding. +0x008's channel-1 half lost bit 13, which is register bit 29 -
 * also the top byte. Bits [31:24] stopped accepting writes across both, and a
 * length truncated from 4796 to 188 bytes is exactly why the transfer got
 * *faster*: it got shorter, not better.
 *
 * ---------------------------------------------------------------------
 *  ANSWERED 2026-08-13: IT IS THE PAD-MUX BIT, ON ITS OWN
 * ---------------------------------------------------------------------
 *     m32 rc45 pad2 |  length reads  |  +0x400
 *      0   0    0   |  0x12BC ok     |  0x80
 *      0   0    1   |  0xBC  TRUNC   |  0x3378B1
 *      0   1    0   |  0x12BC ok     |  0x80
 *      0   1    1   |  0xBC  TRUNC   |  0x3378B1
 *      1   0    0   |  0x12BC ok     |  0x80
 *      1   0    1   |  0xBC  TRUNC   |  0x3378B1
 *      1   1    0   |  0x12BC ok     |  0x80
 *      1   1    1   |  0xBC  TRUNC   |  0x3378B1
 *
 * Module 32 and romclk 45 do nothing in any combination. **Bit 2 of
 * 0x40000020 is a register-window switch**, and 0x40000020 is the pad/pin
 * function mux (notes 7c), not a clock - which is why none of the clock
 * analysis was ever going to find it.
 *
 * The 0x80 at +0x400 in the normal window is the bit clock_start() writes
 * there, so with the switch clear +0x400 is an ordinary register holding what
 * it was given. That is why the vendor takes this bit for exactly as long as
 * it takes to clear +0x400..+0x5FC and then gives it straight back.
 *
 * And the baseline line above the table read 0xBC *before* the table's first
 * row cleared the bit: the previous ToneDemo EQ build left it set and it
 * survived the re-upload. Independent confirmation of the same cause, and a
 * gotcha worth keeping - sl6806_audio_begin() clears it now.
 *
 * ---------------------------------------------------------------------
 *  SO: WHAT ELSE IS BEHIND THE SWITCH?
 * ---------------------------------------------------------------------
 * If bit 2 selects a window rather than merely exposing a RAM, the rest of
 * the map may read differently too - and a control register nobody has seen
 * is exactly the shape of thing that would explain a block which accepts
 * every descriptor and moves no data.
 *
 * The second half of this sketch reads the whole register list twice, once in
 * each window, and prints only what differs. Read-only.
 *
 *     make SKETCH=examples/AudioWindow RUN_MODE=poll run
 *
 * WHAT COUNTS AS A RESULT
 *
 *   1. **Differences outside +0x400..+0x5FC.** Registers this framework has
 *      never seen, in a block that is configured and will not run. That is
 *      the best outcome available.
 *   2. **Differences only inside +0x400..+0x5FC.** Then the switch exposes a
 *      RAM and nothing more, the coefficient array is 128 words as the
 *      vendor's clear implies, and the missing piece is elsewhere again.
 */

#include <Arduino.h>
#include "sl6806_console.h"
#include "sl6806_audio.h"
#include "sl6806_module.h"
#include "sl6806_romclk.h"

#define PROBE_LEN   0x12BCu         /* 4796, the value ToneDemo submits */

/*
 * The map for the second half. Every register this framework names, plus the
 * gaps between them - a window switch has no reason to respect our list.
 */
static const uint32_t regmap[] = {
    0x000, 0x004, 0x008, 0x00C, 0x010, 0x014, 0x018, 0x01C,
    0x020, 0x024, 0x028, 0x02C, 0x030, 0x034, 0x038, 0x03C,
    0x07C, 0x080, 0x084, 0x088,
    0x100, 0x104, 0x108, 0x10C, 0x110, 0x114, 0x118, 0x11C,
    0x134, 0x138, 0x154, 0x158, 0x174, 0x178,
    0x200, 0x204, 0x208, 0x20C, 0x210, 0x214,
    0x224, 0x228, 0x254, 0x258, 0x284, 0x288, 0x2B4, 0x2B8,
    0x400, 0x404, 0x408, 0x40C, 0x410, 0x414, 0x418, 0x41C,
    0x500, 0x5F0, 0x5F4, 0x5F8, 0x5FC,
};
#define NMAP ((int)(sizeof(regmap) / sizeof(regmap[0])))

static bool up;
static bool done;
static int  combo;
static int  phase;
static int  diffs;

static void printHex(uint32_t v)
{
    Serial.print("0x");
    Serial.print(v, HEX);
}

static void romclk(unsigned id, int on)
{
    unsigned i;

    for (i = 0; i < SL6806_ROMCLK_COUNT; i++) {
        if (sl6806_romclk[i].id != id)
            continue;
        if (on)
            sl6806_mmio_set(sl6806_romclk[i].addr, sl6806_romclk[i].bit);
        else
            sl6806_mmio_clr(sl6806_romclk[i].addr, sl6806_romclk[i].bit);
        return;
    }
}

/* Write PROBE_LEN into the length field, read it back, put the old value
 * back. Returns what the field gave. */
static uint32_t lengthProbe(void)
{
    uint32_t saved = sl6806_mmio_read(SL6806_AUD_TX_CTRL);
    uint32_t back;

    sl6806_mmio_field(SL6806_AUD_TX_CTRL, SL6806_AUD_LEN_SHIFT, 16, PROBE_LEN);
    back = sl6806_mmio_read(SL6806_AUD_TX_CTRL) >> SL6806_AUD_LEN_SHIFT;
    sl6806_mmio_write(SL6806_AUD_TX_CTRL, saved);
    return back;
}

/* One register, both windows, printed only if they disagree. */
static void diffPass(void)
{
    uint32_t off = regmap[combo];
    uint32_t a, b;

    sl6806_audio_coeff_window(0);
    a = sl6806_mmio_read(SL6806_AUD_BASE + off);
    sl6806_audio_coeff_window(1);
    b = sl6806_mmio_read(SL6806_AUD_BASE + off);
    sl6806_audio_coeff_window(0);

    if (a == b)
        return;

    Serial.print("  +0x");
    Serial.print(off, HEX);
    Serial.print("   normal ");
    printHex(a);
    Serial.print("   switched ");
    printHex(b);
    Serial.println();
    diffs++;
}

static void finish(void)
{
    Serial.println();
    Serial.print(diffs);
    Serial.println(" register(s) read differently between the two windows.");
    Serial.println("Anything outside +0x400..+0x5FC is a register this");
    Serial.println("framework has never seen. Report those first.");
    sl6806_audio_coeff_window(0);
    sl6806_audio_end();
    done = true;
}

void setup()
{
    Serial.begin(115200);
    Serial.println();
    Serial.println("=== SL6806 audio register-window probe ===");
    Serial.println("Which of module 32 / romclk 45 / padmux bit 2 truncates");
    Serial.println("the length field at +0x108 [31:16]? Then: what else does");
    Serial.println("that bit change?");
    Serial.println();

    up = sl6806_audio_begin(48000) != 0;
    Serial.print("audio begin: ");
    Serial.println(up ? "ok" : "MODULE CLOCK REFUSED - stop here");
    if (!up)
        return;

    Serial.print("baseline: wrote ");
    printHex(PROBE_LEN);
    Serial.print(" read ");
    printHex(lengthProbe());
    Serial.println();
    Serial.println();
    Serial.println("  m32 rc45 pad2 |  length reads  |  +0x400");
    Serial.println("  --------------+----------------+-----------");

    combo = 0;
    phase = 0;
}

void loop()
{
    unsigned m32, rc45, pad2;
    uint32_t len, eq;

    if (!up || done)
        return;

    /* One step per call, so the console keeps up and a hang names the step. */
    if (sl6806_console_space() < (int)(SL6806_CONSOLE_SIZE / 2))
        return;

    if (phase == 1) {
        if (combo == 0) {
            Serial.println();
            Serial.println("--- the whole map, both windows, differences only ---");
        }
        diffPass();
        if (++combo >= NMAP)
            finish();
        return;
    }

    m32  = ((unsigned)combo >> 2) & 1u;
    rc45 = ((unsigned)combo >> 1) & 1u;
    pad2 = (unsigned)combo & 1u;

    if (m32)
        sl6806_module_enable(SL6806_AUD_EQ_MODULE_ID);
    romclk(SL6806_AUD_EQ_ROMCLK_ID, (int)rc45);
    sl6806_audio_coeff_window((int)pad2);
    delay(5);

    len = lengthProbe();
    eq  = sl6806_mmio_read(SL6806_AUD_EQ_CTRL);

    Serial.print("   ");
    Serial.print(m32);
    Serial.print("   ");
    Serial.print(rc45);
    Serial.print("    ");
    Serial.print(pad2);
    Serial.print("   |  ");
    printHex(len);
    Serial.print(len == PROBE_LEN ? " ok       " : " TRUNCATED ");
    Serial.print("|  ");
    printHex(eq);
    Serial.println();

    /* Put everything back before the next combination. Only module 32 is ever
     * switched off, and only because this sketch switched it on. */
    if (m32)
        sl6806_module_disable(SL6806_AUD_EQ_MODULE_ID);
    romclk(SL6806_AUD_EQ_ROMCLK_ID, 0);
    sl6806_audio_coeff_window(0);
    delay(5);

    if (++combo < 8)
        return;

    Serial.println();
    Serial.print("restored: length reads ");
    printHex(lengthProbe());
    Serial.print(", 0x40000020 = ");
    printHex(sl6806_mmio_read(SL6806_AUD_PADMUX_REG));
    Serial.println();

    combo = 0;
    phase = 1;
}
