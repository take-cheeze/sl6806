/*
 * SdClock - the SD host has no functional-clock bit. Sweep its clock instead.
 *
 * ===================================================================
 *  WHERE THIS COMES FROM
 * ===================================================================
 * examples/SdWall, 2026-08-13, walked 0x400E0000 with CMD0 as the witness and
 * came back with a clean negative - plus one fact worth more than the walk:
 *
 *     bits 0..6 hold. Bits 7..31 do not.
 *
 * **0x400E0000 is a seven-bit register**, not a 32-bit one, and four of those
 * seven are already attributed (0 Bluetooth, 1 the 0x400E2300 cluster,
 * 5 the group's own, 6 camera). So the walk did not fail to find a bit; it
 * tried every bit the register has. The SD host does not have one. That is
 * this chip's usual answer eliminated rather than untested, and it also
 * shrinks the open question the audio block and the PWM are waiting on from
 * 28 bits to three - 2, 3 and 4.
 *
 * Which leaves the clock the SD host does have. ROM clock id 17 is
 * `0x40080080`, and reading the mask ROM's own accessors for it decodes the
 * whole register:
 *
 *     bit 0        enable          (ROM 0x223C sets it, 0x2614 clears it)
 *     bit 4        source select   (ROM 0x372E: set -> source 8,
 *                                   clear -> source 59)
 *     bit 8        divide by 8     (ROM 0x2D64 reads it as a x8 prescale)
 *     bits [19:16] divider n       (ROM 0x2A2A writes it; the rate query at
 *                                   0x2D64 computes (n + 1) * (bit8 ? 8 : 1))
 *
 * **The mask ROM's SD bring-up sets bit 0 and nothing else.** It never
 * programs a divider for this clock, and its one call to the source selector
 * passes 10 - a value that dispatcher does not recognise for id 17, which
 * only knows 8 and 59 - so the call writes nothing. The whole SD clock
 * therefore comes up on whatever the reset defaults are, and if a payload
 * arrives in bootloader mode with a different clock tree than the ROM's SD
 * code was written against, "no card clock" is exactly what it would look
 * like: the command register takes the word, the state machine never shifts
 * it out, and the status register never says anything.
 *
 * ===================================================================
 *  WHAT THIS SKETCH DOES
 * ===================================================================
 *   1. Prints the clock tree as found: `0x40080080` decoded field by field,
 *      and the CRU words around it. Nobody has ever recorded these.
 *   2. Reports whether CTRL bits 0 and 1 stick. Both the ROM's bring-up and
 *      this framework's set them alongside the reset bit, and both read back
 *      zero - self-clearing kick or not writable, and it matters.
 *   3. Sweeps all 64 settings of that register - source x prescale x
 *      divider - and sends one CMD0 at each, reporting every case where the
 *      command-done bit finally sets.
 *   4. Sweeps CLKCR's own divider field across eight values at the setting
 *      that looked best, in case the block's internal divide is the problem
 *      rather than the clock into it.
 *
 * The witness stays CMD0: no card, no data path, no response, nothing but
 * "did the controller shift 48 bits out".
 *
 *     make SKETCH=examples/SdClock RUN_MODE=poll run
 *
 * ===================================================================
 *  READ THIS BEFORE TRUSTING ANY "COLD" DUMP, INCLUDING THIS ONE
 * ===================================================================
 * Uploading a payload does not reset the chip. The boot ROM keeps running and
 * drops the new image into SRAM, so **every register another sketch touched
 * is still touched when the next one starts**, and a "cold" dump taken by the
 * second SD sketch of a session is not cold at all.
 *
 * That matters here because the first SdProbe run found the block already
 * holding the mask ROM's bring-up values, which reads like evidence that the
 * ROM brings the SD host up at boot - and would be, except that an earlier
 * payload in the same power session leaves exactly the same fingerprint.
 *
 * There is a clean way to tell, and it is worth one power cycle:
 *
 *     power off, hold the boot key, plug in, and run examples/SdProbe FIRST.
 *
 * The ROM's own SD bring-up (0x0003D5CC) tears itself down when it fails: it
 * parks the three pads on function 15, disables module 36 and clears
 * `0x40080080` bit 0 (0x0003D2F0). So on a genuinely fresh boot:
 *
 *   - module 36 ENABLED and the registers holding 0x20070008 / 0xA400
 *     means the ROM's init RAN AND SUCCEEDED, which with no card in the slot
 *     should not happen - and would say the failure is somewhere in what a
 *     payload does differently;
 *   - module 36 disabled means the ROM tried and tore down, and the values
 *     seen last time were a previous payload's;
 *   - module 36 disabled and the registers zero means the ROM never touched
 *     the SD host at all in bootloader mode, and the block is simply cold.
 *
 * All three are useful and they are not the same. One power cycle settles it.
 */

#include <Arduino.h>

extern "C" {
#include "sl6806_sd.h"
#include "sl6806_mmio.h"
#include "sl6806_module.h"
}

#define CRU 0x40080000u

static bool done;

static void printHex(uint32_t v, int digits = 8)
{
    static const char *hex = "0123456789ABCDEF";

    Serial.print("0x");
    for (int i = digits - 1; i >= 0; i--)
        Serial.print(hex[(v >> (i * 4)) & 0xF]);
}

/* The witness: bring the controller up, send CMD0, say whether it completed. */
static int cmd0(uint32_t *sta_out)
{
    unsigned n;

    if (sl6806_sd_controller_begin(0u, SL6806_SD_CLKCR_IDENT) != SL6806_SD_OK) {
        *sta_out = 0;
        return 0;
    }

    sl6806_mmio_write(SL6806_SD_BASE + SL6806_SD_STA, SL6806_SD_STA_CLEAR_ALL);
    sl6806_mmio_write(SL6806_SD_BASE + SL6806_SD_ARG, 0);
    sl6806_mmio_write(SL6806_SD_BASE + SL6806_SD_CMD, 0x8000A400u);

    for (n = 0; n < 20000u; n++)
        if (sl6806_mmio_read(SL6806_SD_BASE + SL6806_SD_STA)
            & SL6806_SD_STA_CMDDONE)
            break;

    *sta_out = sl6806_mmio_read(SL6806_SD_BASE + SL6806_SD_STA);
    return (*sta_out & SL6806_SD_STA_CMDDONE) ? 1 : 0;
}

/* Same, but leaving the clock register alone and varying CLKCR instead. */
static int cmd0_clkcr(uint32_t clkcr, uint32_t *sta_out)
{
    unsigned n;

    if (sl6806_sd_controller_begin(0u, clkcr) != SL6806_SD_OK) {
        *sta_out = 0;
        return 0;
    }

    sl6806_mmio_write(SL6806_SD_BASE + SL6806_SD_STA, SL6806_SD_STA_CLEAR_ALL);
    sl6806_mmio_write(SL6806_SD_BASE + SL6806_SD_ARG, 0);
    sl6806_mmio_write(SL6806_SD_BASE + SL6806_SD_CMD, 0x8000A400u);

    for (n = 0; n < 20000u; n++)
        if (sl6806_mmio_read(SL6806_SD_BASE + SL6806_SD_STA)
            & SL6806_SD_STA_CMDDONE)
            break;

    *sta_out = sl6806_mmio_read(SL6806_SD_BASE + SL6806_SD_STA);
    return (*sta_out & SL6806_SD_STA_CMDDONE) ? 1 : 0;
}

void setup()
{
    Serial.begin(115200);
    Serial.println();
    Serial.println("=== SL6806 SD clock sweep, CMD0 as the witness ===");
    Serial.println("0x400E0000 is seven bits wide and none of them is the");
    Serial.println("SD host's. This sweeps ROM clock 17 instead - see the");
    Serial.println("sketch header and docs/SD.md.");
    Serial.println();
    Serial.flush();

    done = false;
}

void loop()
{
    uint32_t saved, v, sta;
    unsigned found = 0;

    if (done)
        return;
    done = true;

    /* --- 1: the clock tree as found ------------------------------- */

    Serial.println("--- the clock, as found ---");
    Serial.print("  module 36: ");
    Serial.println(sl6806_module_enabled(SL6806_SD_MODULE_ID) ? "ENABLED"
                                                              : "disabled");

    saved = sl6806_mmio_read(SL6806_SD_ROMCLK_REG);
    Serial.print("  0x40080080 = ");
    printHex(saved);
    Serial.println();
    Serial.print("    enable (bit 0)   ");
    Serial.println((saved & 1u) ? "on" : "off");
    Serial.print("    source (bit 4)   ");
    Serial.println((saved & 0x10u) ? "8" : "59");
    Serial.print("    prescale (bit 8) ");
    Serial.println((saved & 0x100u) ? "divide by 8" : "none");
    Serial.print("    divider [19:16]  ");
    Serial.print((saved >> 16) & 0xFu);
    Serial.print("   total divide ");
    Serial.println((((saved >> 16) & 0xFu) + 1u) * ((saved & 0x100u) ? 8u : 1u));

    Serial.println("  CRU around it, +0x40 to +0x60 and +0x7C to +0x8C:");
    for (uint32_t off = 0x40; off <= 0x60; off += 4) {
        Serial.print("    +");
        printHex(off, 3);
        Serial.print("  ");
        printHex(sl6806_mmio_read(CRU + off));
        Serial.println();
    }
    for (uint32_t off = 0x7C; off <= 0x8C; off += 4) {
        Serial.print("    +");
        printHex(off, 3);
        Serial.print("  ");
        printHex(sl6806_mmio_read(CRU + off));
        Serial.println();
    }
    Serial.println();
    Serial.flush();

    /* --- 2: do CTRL's low bits stick? ----------------------------- */

    Serial.println("--- CTRL bits 0 and 1 ---");
    sl6806_sd_controller_begin(0u, SL6806_SD_CLKCR_IDENT);
    sl6806_mmio_write(SL6806_SD_BASE + SL6806_SD_CTRL, 0x40000003u);
    v = sl6806_mmio_read(SL6806_SD_BASE + SL6806_SD_CTRL);
    Serial.print("  CTRL <- 0x40000003 (no reset bit)  reads ");
    printHex(v);
    Serial.println((v & 3u) ? "   they stick" : "   they DO NOT stick");
    Serial.println();
    Serial.flush();

    /* --- 3: sweep the clock --------------------------------------- */

    Serial.println("--- sweeping 0x40080080: source x prescale x divider ---");
    Serial.println("  Every line is one CMD0. A blank result column is a");
    Serial.println("  command that never completed.");
    Serial.println();

    for (unsigned src = 0; src < 2u; src++) {
        for (unsigned pre = 0; pre < 2u; pre++) {
            for (unsigned div = 0; div < 16u; div++) {
                uint32_t w = 1u | (src ? 0x10u : 0u) | (pre ? 0x100u : 0u)
                           | ((uint32_t)div << 16);
                int ok;

                sl6806_mmio_write(SL6806_SD_ROMCLK_REG, w);
                if (sl6806_mmio_read(SL6806_SD_ROMCLK_REG) != w) {
                    if (div == 0) {
                        Serial.print("  src ");
                        Serial.print(src ? 8 : 59);
                        Serial.print(" pre ");
                        Serial.print(pre);
                        Serial.println("  register will not take this");
                    }
                    continue;
                }

                ok = cmd0(&sta);
                if (ok || div == 0) {
                    Serial.print("  src ");
                    Serial.print(src ? 8 : 59);
                    Serial.print("  pre ");
                    Serial.print(pre);
                    Serial.print("  div ");
                    Serial.print(div);
                    Serial.print("\t STA ");
                    printHex(sta);
                    Serial.println(ok ? "   *** CMD0 COMPLETED ***" : "");
                    Serial.flush();
                }
                if (ok)
                    found++;
            }
        }
    }

    sl6806_mmio_write(SL6806_SD_ROMCLK_REG, saved);
    Serial.print("  0x40080080 restored to ");
    printHex(sl6806_mmio_read(SL6806_SD_ROMCLK_REG));
    Serial.println();
    Serial.println();

    /* --- 4: sweep CLKCR's own divider ----------------------------- */

    Serial.println("--- sweeping CLKCR's divider field [22:16] ---");
    for (unsigned d = 0; d < 8u; d++) {
        static const unsigned DIV[8] = { 0, 1, 2, 3, 7, 15, 63, 127 };
        uint32_t clkcr = 0x20000000u | ((uint32_t)DIV[d] << 16) | 8u;
        int ok = cmd0_clkcr(clkcr, &sta);

        Serial.print("  CLKCR ");
        printHex(clkcr);
        Serial.print("  STA ");
        printHex(sta);
        Serial.println(ok ? "   *** CMD0 COMPLETED ***" : "");
        Serial.flush();
        if (ok)
            found++;
    }

    /* And the other source bit in CLKCR's [30:23] field, which is the one
     * difference between the identification and run configurations that is
     * not the divider. */
    Serial.println("  and the run configuration's 0x10000000 in [30:23]:");
    for (unsigned d = 0; d < 4u; d++) {
        static const unsigned DIV[4] = { 0, 3, 7, 15 };
        uint32_t clkcr = 0x10000000u | ((uint32_t)DIV[d] << 16) | 12u;
        int ok = cmd0_clkcr(clkcr, &sta);

        Serial.print("  CLKCR ");
        printHex(clkcr);
        Serial.print("  STA ");
        printHex(sta);
        Serial.println(ok ? "   *** CMD0 COMPLETED ***" : "");
        Serial.flush();
        if (ok)
            found++;
    }

    Serial.println();
    Serial.print("=== ");
    Serial.print(found);
    Serial.println(" setting(s) made CMD0 complete ===");
    if (!found) {
        Serial.println("Then the clock into the block is not the missing");
        Serial.println("piece either, and what is left is the power-cycle");
        Serial.println("test in this sketch's header - run SdProbe first");
        Serial.println("thing after a fresh boot and report module 36's");
        Serial.println("state. That says whether the mask ROM can drive");
        Serial.println("this block in bootloader mode at all.");
    }
    Serial.println("done.");
}
