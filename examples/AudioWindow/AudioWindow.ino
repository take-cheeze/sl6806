/*
 * AudioWindow - which of three enables changes the audio block's registers,
 * and how.
 *
 * ---------------------------------------------------------------------
 *  WHY: THE EQ HOLD DID SOMETHING, AND IT WAS NOT WHAT WAS WANTED
 * ---------------------------------------------------------------------
 * examples/ToneDemo built with -DTONEDEMO_EQ_HOLD=1 holds the three enables
 * the vendor's init borrows around its coefficient-RAM clear: module clock
 * 32, romclk 45, and bit 2 of 0x40000020. Measured 2026-08-13, that is the
 * first thing all session to change the block's behaviour - and every change
 * points the wrong way:
 *
 *     +0x400 went from 0x00000000 to 0x003378B1     (the sub-block woke)
 *     +0x108 read back 0x00BC1F7A, not 0x12BC0910
 *     +0x008 route 0 read 0x00832083, not 0x20832083
 *     mean completion time 10 us -> 3 us
 *
 * The two register changes are the same change. We write 4796 = 0x12BC into
 * the length field at +0x108 [31:16] and read back 0x00BC - the top byte of
 * the register stopped holding. And +0x008's channel-1 half lost bit 13,
 * which is register bit 29 - also in the top byte. **Bits [31:24] stopped
 * accepting writes across at least two registers**, and a length field
 * truncated from 4796 to 188 bytes is exactly why the transfer got faster
 * rather than slower.
 *
 * So the EQ hypothesis is dead: holding those enables makes things worse, and
 * the vendor borrows them briefly for a reason. But *something* in there
 * moves the register window, and that is worth knowing precisely - it is the
 * only lever found so far that the block responds to at all.
 *
 * ---------------------------------------------------------------------
 *  WHAT THIS DOES
 * ---------------------------------------------------------------------
 * Eight combinations of the three enables. For each: write a known 16-bit
 * value into the length field, read it back, and report whether the top byte
 * survived. Also reads +0x400, which is the sub-block that wakes.
 *
 * The three are turned off again between combinations - which is why
 * sl6806_module_disable() exists now. Only module 32 is ever switched off,
 * and only after this sketch switched it on.
 *
 *     make SKETCH=examples/AudioWindow RUN_MODE=poll run
 *
 * WHAT COUNTS AS A RESULT
 *
 *   1. **Exactly one of the three truncates the field.** Then it is that one,
 *      and if it is the pad-mux bit that is a register-window switch rather
 *      than a clock - which would say the +0x400 region is a second view of
 *      the same address space, and explain why the vendor only ever holds it
 *      for the length of a memset.
 *   2. **The pad-mux bit alone wakes +0x400.** Same conclusion from the other
 *      side.
 *   3. **Nothing truncates until all three are on.** Then it is the sub-block
 *      being clocked, not the window, and +0x400 is genuinely coefficient RAM
 *      whose engine contends for the register bus.
 */

#include <Arduino.h>
#include "sl6806_console.h"
#include "sl6806_audio.h"
#include "sl6806_module.h"
#include "sl6806_romclk.h"

#define PROBE_LEN   0x12BCu         /* 4796, the value ToneDemo submits */

static bool up;
static bool done;
static int  combo;

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

void setup()
{
    Serial.begin(115200);
    Serial.println();
    Serial.println("=== SL6806 audio register-window probe ===");
    Serial.println("Which of module 32 / romclk 45 / padmux bit 2 truncates");
    Serial.println("the length field at +0x108 [31:16]?");
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
    Serial.println("  m32 rc45 pad2 |  length reads |  +0x400");
    Serial.println("  --------------+---------------+-----------");

    combo = 0;
}

void loop()
{
    unsigned m32, rc45, pad2;
    uint32_t len, eq;

    if (!up || done)
        return;

    if (sl6806_console_space() < (int)(SL6806_CONSOLE_SIZE / 2))
        return;

    m32  = (combo >> 2) & 1u;
    rc45 = (combo >> 1) & 1u;
    pad2 = combo & 1u;

    if (m32)
        sl6806_module_enable(SL6806_AUD_EQ_MODULE_ID);
    romclk(SL6806_AUD_EQ_ROMCLK_ID, rc45);
    if (pad2)
        sl6806_mmio_set(SL6806_AUD_PADMUX_REG, SL6806_AUD_PADMUX_EQ);
    else
        sl6806_mmio_clr(SL6806_AUD_PADMUX_REG, SL6806_AUD_PADMUX_EQ);
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
    Serial.print(len == PROBE_LEN ? " ok      " : " TRUNCATED ");
    Serial.print("|  ");
    printHex(eq);
    Serial.println();

    /* Put everything back before the next combination. Only module 32 is
     * ever switched off, and only because this sketch switched it on. */
    if (m32)
        sl6806_module_disable(SL6806_AUD_EQ_MODULE_ID);
    romclk(SL6806_AUD_EQ_ROMCLK_ID, 0);
    sl6806_mmio_clr(SL6806_AUD_PADMUX_REG, SL6806_AUD_PADMUX_EQ);
    delay(5);

    combo++;
    if (combo < 8)
        return;

    Serial.println();
    Serial.print("restored: length reads ");
    printHex(lengthProbe());
    Serial.print(", 0x40000020 = ");
    printHex(sl6806_mmio_read(SL6806_AUD_PADMUX_REG));
    Serial.println();
    Serial.println("done. Report the whole table - which rows truncate, and");
    Serial.println("whether +0x400 wakes on the same rows or different ones.");
    sl6806_audio_end();
    done = true;
}
