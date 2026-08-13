/*
 * SdProbe - bring the SD/MMC host at 0x40003000 up and read the card.
 *
 * ===================================================================
 *  WHAT THIS IS FOR
 * ===================================================================
 * The TF slot has been "hardware confirmed present, no driver" in this
 * tree's capability table since the beginning. cores/sl6806/sl6806_sd.h now
 * carries a driver, transcribed from the mask ROM - which is where the whole
 * SD stack lives, the same way GPIO does - and this sketch is the first thing
 * that has ever tried to use it. Nothing in it has run on hardware.
 *
 * It answers, in order, five questions that get progressively harder to fake:
 *
 *   1. **Does the block exist?** Read a handful of registers cold, before
 *      anything is gated on, and again after. A block whose registers change
 *      from all-zero to holding written values is being clocked; one that
 *      reads plausibly in both states was probably never off, and one that
 *      reads zero in both is still gated. This is the pattern examples/
 *      DvpProbe used to find module id 46, and it is the cheapest real
 *      evidence there is.
 *
 *   2. **Does a command go out?** CMD0 raises the command-done bit without
 *      any card being present, because it expects no response. If that bit
 *      never sets, the controller is not running and everything below is
 *      moot - which is exactly the failure the PWM and the camera front end
 *      turned out to have, and the sketch says so in those words.
 *
 *   3. **Is there a card?** CMD8 and ACMD41. A card that answers has been
 *      powered, clocked and addressed over four wires this project has never
 *      driven before.
 *
 *   4. **Who is it?** CID, CSD, the published address, the capacity. A
 *      capacity that comes out as a real card size - 4 GB, 32 GB - out of
 *      four words of CSD is worth more than any amount of "looks live",
 *      the same way the register file's rails decoding to 2.8 V was.
 *
 *   5. **Can it be read?** Sector 0, dumped, and then checked for the two
 *      signatures a card formatted by anything at all will carry: the 0x55AA
 *      at the end of the sector, and either a partition table or a FAT boot
 *      sector at the start. Random bytes with 0x55AA in the right place do
 *      not happen; that pair is the proof that the data path works.
 *
 *     make SKETCH=examples/SdProbe RUN_MODE=poll run
 *
 * RUN_MODE=poll matters here for the usual reason - see below.
 *
 * ===================================================================
 *  IF IT STOPS AT STEP 2
 * ===================================================================
 * "Configured and not running" is this chip's signature failure, and it has
 * happened twice: the PWM takes a full configuration and its counter does not
 * move (14a), and the camera front end takes a full geometry and drives no
 * clock (7n). Both times the missing piece was a functional-clock bit in
 * 0x400E0000, a register that a payload can only write once module 46 has
 * gated it open (sl6806_module.h).
 *
 * So if the registers hold their values and no command ever completes, the
 * next thing to try is not a different clock divider. It is
 * sl6806_periph_group_begin() followed by a walk of 0x400E0000's 32 bits with
 * "did CMD0 complete" as the witness - the shape examples/AudioWall uses. The
 * driver's bring-up is the mask ROM's own and there is no reason to doubt the
 * part of it that is written down; what is missing would be the part the
 * vendor's code never needed to say.
 *
 * ===================================================================
 *  WHAT THIS SKETCH DOES NOT DO
 * ===================================================================
 * No writes to the card. Not one. A card in this slot may well be the only
 * copy of something, and every question above is answerable by reading.
 *
 * No four-bit bus. The mask ROM configures three pads, which is one data
 * line, and sending ACMD6 without knowing the fourth pad would switch the
 * card onto wires this chip is not driving.
 *
 * No filesystem. Sector 0 is decoded far enough to say "this is a partition
 * table" or "this is a FAT boot sector", which is a check on the data path
 * rather than the beginning of a VFS.
 */

#include <Arduino.h>

extern "C" {
#include "sl6806_sd.h"
#include "sl6806_mmio.h"
#include "sl6806_module.h"
}

static bool done;

static void printHex(uint32_t v, int digits = 8)
{
    static const char *hex = "0123456789ABCDEF";

    Serial.print("0x");
    for (int i = digits - 1; i >= 0; i--)
        Serial.print(hex[(v >> (i * 4)) & 0xF]);
}

/* The registers worth watching across the gate. Chosen because each has a
 * different reason to be non-zero: CTRL and CLKCR are written by bring-up,
 * STA and STA2 are the block's own state, and DTIMER comes out of reset with
 * a value nothing wrote. */
static const struct { const char *name; uint32_t off; } WATCH[] = {
    { "CTRL   ", SL6806_SD_CTRL   },
    { "CMD    ", SL6806_SD_CMD    },
    { "DTIMER ", SL6806_SD_DTIMER },
    { "DCTRL  ", SL6806_SD_DCTRL  },
    { "STA    ", SL6806_SD_STA    },
    { "STA2   ", SL6806_SD_STA2   },
    { "CLKCR  ", SL6806_SD_CLKCR  },
};
#define N_WATCH (sizeof(WATCH) / sizeof(WATCH[0]))

static uint32_t cold[N_WATCH];

static void dumpRegisters(const char *what, uint32_t *into)
{
    Serial.print("  ");
    Serial.println(what);
    for (unsigned i = 0; i < N_WATCH; i++) {
        uint32_t v = sl6806_mmio_read(SL6806_SD_BASE + WATCH[i].off);

        if (into)
            into[i] = v;
        Serial.print("    ");
        Serial.print(WATCH[i].name);
        printHex(v);
        if (!into && v != cold[i])
            Serial.print("   <-- changed");
        Serial.println();
    }
}

static void dumpSector(const uint8_t *s)
{
    static const char *hex = "0123456789ABCDEF";

    /* The first 64 bytes and the last 16. The middle of a boot sector is
     * either code or zeros and says nothing a probe can use. */
    for (unsigned row = 0; row < 4; row++) {
        Serial.print("    ");
        printHex(row * 16, 3);
        Serial.print("  ");
        for (unsigned i = 0; i < 16; i++) {
            uint8_t b = s[row * 16 + i];

            Serial.print(hex[b >> 4]);
            Serial.print(hex[b & 0xF]);
            Serial.print(' ');
        }
        Serial.println();
    }
    Serial.print("    1F0  ");
    for (unsigned i = 496; i < 512; i++) {
        Serial.print(hex[s[i] >> 4]);
        Serial.print(hex[s[i] & 0xF]);
        Serial.print(' ');
    }
    Serial.println();
}

/* Say what sector 0 looks like. Deliberately conservative: this is a check on
 * the data path, so it only claims what it can see. */
static void describeSector(const uint8_t *s)
{
    bool sig = (s[510] == 0x55 && s[511] == 0xAA);

    Serial.print("    boot signature 0x55AA: ");
    Serial.println(sig ? "present" : "ABSENT");

    if (!sig) {
        Serial.println("    Without it this is not a formatted card's sector 0,");
        Serial.println("    or the data path delivered something else.");
        return;
    }

    if (s[0] == 0xEB || s[0] == 0xE9) {
        Serial.println("    starts with a jump: this is a FAT boot sector,");
        Serial.print("    OEM name: ");
        for (unsigned i = 3; i < 11; i++)
            Serial.print((char)(s[i] >= 0x20 && s[i] < 0x7F ? s[i] : '.'));
        Serial.println();
        return;
    }

    /* Otherwise treat it as an MBR and decode the four partition entries. A
     * card straight from a camera or a phone has exactly one. */
    Serial.println("    partition table:");
    for (unsigned p = 0; p < 4; p++) {
        const uint8_t *e = s + 446 + p * 16;
        uint32_t lba = (uint32_t)e[8] | ((uint32_t)e[9] << 8)
                     | ((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24);
        uint32_t n   = (uint32_t)e[12] | ((uint32_t)e[13] << 8)
                     | ((uint32_t)e[14] << 16) | ((uint32_t)e[15] << 24);

        if (e[4] == 0 && n == 0)
            continue;
        Serial.print("      #");
        Serial.print(p);
        Serial.print("  type ");
        printHex(e[4], 2);
        Serial.print("  first LBA ");
        Serial.print(lba);
        Serial.print("  sectors ");
        Serial.println(n);
    }
}

/*
 * Write a nonce into ARG and read it back. ARG is the safe register to do
 * this in: nothing latches it until a command starts, and the next command
 * writes it anyway. Two different nonces, because a register stuck at one
 * value would pass a single-nonce test half the time.
 */
static void gateTest()
{
    static const uint32_t NONCE[2] = { 0xA5A5F00Du, 0x5A5A0FF0u };
    uint32_t saved = sl6806_mmio_read(SL6806_SD_BASE + SL6806_SD_ARG);
    int stuck = 0;

    for (unsigned i = 0; i < 2; i++) {
        uint32_t got;

        sl6806_mmio_write(SL6806_SD_BASE + SL6806_SD_ARG, NONCE[i]);
        got = sl6806_mmio_read(SL6806_SD_BASE + SL6806_SD_ARG);
        Serial.print("    ARG <- ");
        printHex(NONCE[i]);
        Serial.print("  reads ");
        printHex(got);
        Serial.println(got == NONCE[i] ? "   ok" : "   DROPPED");
        if (got != NONCE[i])
            stuck++;
    }
    sl6806_mmio_write(SL6806_SD_BASE + SL6806_SD_ARG, saved);

    Serial.print("    verdict: ");
    if (stuck == 0)
        Serial.println("the block takes writes - it is not gated off");
    else
        Serial.println("WRITES ARE DROPPED - the block is gated, and every"
                       " register above is a leftover");
}

/*
 * Send CMD0 by hand and watch what the controller does with it, instruction
 * by instruction. The driver reports "no response"; this says which of the
 * three possible silences it is:
 *
 *   - the start bit never latches      -> the command register is not live
 *   - the start bit latches and stays  -> the state machine is stalled,
 *                                         which means no card clock
 *   - the start bit latches and clears -> the command went out and the
 *                                         status bit is the thing that is
 *                                         wrong
 *
 * Those need different next moves, and the driver's one error code cannot
 * tell them apart.
 */
static void commandScope()
{
    uint32_t v;

    Serial.println("--- step 2b: CMD0, watched ---");

    /* Does CTRL keep the two enable bits, or are they a self-clearing kick?
     * Cold they read zero even after the ROM's own bring-up, which is worth
     * pinning down rather than assuming. */
    sl6806_mmio_write(SL6806_SD_BASE + SL6806_SD_CTRL, 0x40000007u);
    v = sl6806_mmio_read(SL6806_SD_BASE + SL6806_SD_CTRL);
    Serial.print("    CTRL <- 0x40000007  reads ");
    printHex(v);
    /* Report the two bits separately. An earlier version of this test asked
     * whether *either* had stuck, printed "they stick", and hid the finding:
     * bit 1 holds and bit 0 does not. See examples/SdCtrl. */
    Serial.print("   bit 0 ");
    Serial.print((v & 1u) ? "stuck" : "gone");
    Serial.print(", bit 1 ");
    Serial.println((v & 2u) ? "stuck" : "gone");

    for (unsigned n = 0; n < 4000u; n++) {
        if (!(sl6806_mmio_read(SL6806_SD_BASE + SL6806_SD_CTRL) & 4u))
            break;
    }
    Serial.print("    after the reset bit clears, CTRL = ");
    printHex(sl6806_mmio_read(SL6806_SD_BASE + SL6806_SD_CTRL));
    Serial.println();

    sl6806_mmio_write(SL6806_SD_BASE + SL6806_SD_STA, 0x0000BFC6u);
    Serial.print("    STA after acknowledging everything: ");
    printHex(sl6806_mmio_read(SL6806_SD_BASE + SL6806_SD_STA));
    Serial.println();

    sl6806_mmio_write(SL6806_SD_BASE + SL6806_SD_ARG, 0);
    sl6806_mmio_write(SL6806_SD_BASE + SL6806_SD_CMD, 0x8000A400u);

    v = sl6806_mmio_read(SL6806_SD_BASE + SL6806_SD_CMD);
    Serial.print("    CMD <- 0x8000A400  reads back ");
    printHex(v);
    Serial.println((v & 0x80000000u) ? "   START LATCHED"
                                     : "   start bit did not latch");

    Serial.println("    STA and CMD, sampled while it should be going out:");
    for (unsigned i = 0; i < 8u; i++) {
        Serial.print("      ");
        printHex(sl6806_mmio_read(SL6806_SD_BASE + SL6806_SD_STA));
        Serial.print("  ");
        printHex(sl6806_mmio_read(SL6806_SD_BASE + SL6806_SD_CMD));
        Serial.print("  ");
        printHex(sl6806_mmio_read(SL6806_SD_BASE + SL6806_SD_STA2));
        Serial.println();
        delayMicroseconds(200);
    }

    Serial.println();
    Serial.println("    If the start bit latched and then cleared with STA");
    Serial.println("    still zero, the block ran the command and reported");
    Serial.println("    nothing - which is the functional-clock failure the");
    Serial.println("    PWM and the camera both have. examples/SdWall walks");
    Serial.println("    0x400E0000 with this exact CMD0 as the witness.");
}

void setup()
{
    Serial.begin(115200);
    Serial.println();
    Serial.println("=== SL6806 SD/MMC probe ===");
    Serial.print("Host at ");
    printHex(SL6806_SD_BASE);
    Serial.println(" - see docs/SD.md.");
    Serial.println("Read-only. Nothing here writes to the card.");
    Serial.println();
    Serial.flush();

    done = false;
}

void loop()
{
    static uint32_t sector[128];
    sl6806_sd_card_t card;
    int err;

    if (done)
        return;                 /* loop() is driven by USB polls; just yield */
    done = true;

    /* --- 1: is the block there at all? ---------------------------- */

    Serial.println("--- step 1: the block, cold ---");
    Serial.print("  module ");
    Serial.print(SL6806_SD_MODULE_ID);
    Serial.print(" reads ");
    Serial.println(sl6806_module_enabled(SL6806_SD_MODULE_ID) ? "ENABLED"
                                                              : "disabled");
    Serial.print("  ROM clock 17 (0x40080080 bit 0) reads ");
    Serial.println((sl6806_mmio_read(SL6806_SD_ROMCLK_REG)
                    & SL6806_SD_ROMCLK_BIT) ? "ENABLED" : "disabled");
    dumpRegisters("registers before any gate:", cold);
    Serial.println();
    Serial.flush();

    /* --- 1b: DO WRITES STICK? ------------------------------------- */

    /*
     * The measurement the first version of this sketch was missing, and the
     * one everything else depends on. The registers above may be holding
     * values the boot ROM wrote before this payload started - the mask ROM
     * brings the SD host up itself - in which case "unchanged after
     * bring-up" says nothing at all, because the driver would be writing
     * what is already there.
     *
     * A gated block on this chip reads plausible values and drops writes
     * silently. That is how the ADC was written off for four hardware runs
     * (sl6806_module.h). So: write a value nothing else would produce, read
     * it back, put the original back.
     */
    Serial.println("--- step 1b: do writes stick? ---");
    gateTest();
    Serial.println();
    Serial.flush();

    /* --- 2, 3, 4: bring-up and identification --------------------- */

    Serial.println("--- step 2: bring-up and identification ---");
    err = sl6806_sd_begin(&card);

    dumpRegisters("registers after bring-up:", NULL);
    Serial.println();
    gateTest();
    Serial.println();

    Serial.print("  result: ");
    Serial.print(err);
    Serial.print(" (");
    Serial.print(sl6806_sd_strerror(err));
    Serial.println(")");
    Serial.print("  status now ");
    printHex(sl6806_sd_status());
    Serial.println();

    if (err == SL6806_SD_ERR_CMD_TMOUT) {
        Serial.println();
        Serial.println("  CMD0 never completed. That is the controller, not");
        Serial.println("  the card: CMD0 expects no response, so its done bit");
        Serial.println("  sets with an empty slot too. If the registers above");
        Serial.println("  changed, this is the PWM's and the camera's failure -");
        Serial.println("  clocked registers, no function - and the next move is");
        Serial.println("  0x400E0000, not a different divider. Read this");
        Serial.println("  sketch's header.");
    }
    if (err != SL6806_SD_OK) {
        Serial.println();
        commandScope();
        Serial.println();
        Serial.println("done.");
        return;
    }

    Serial.println();
    Serial.println("--- step 3: the card ---");
    Serial.print("  type          ");
    switch (card.type) {
    case SL6806_SD_TYPE_SDSC_V1: Serial.println("SD, version 1"); break;
    case SL6806_SD_TYPE_SDSC_V2: Serial.println("SD, version 2"); break;
    case SL6806_SD_TYPE_SDHC:    Serial.println("SDHC/SDXC"); break;
    default:                     Serial.println("unknown"); break;
    }
    Serial.print("  address       ");
    printHex(card.rca, 4);
    Serial.println();
    Serial.print("  OCR           ");
    printHex(card.ocr);
    Serial.println();
    Serial.print("  CID           ");
    for (int i = 0; i < 4; i++) { printHex(card.cid[i]); Serial.print(' '); }
    Serial.println();
    Serial.print("  CSD           ");
    for (int i = 0; i < 4; i++) { printHex(card.csd[i]); Serial.print(' '); }
    Serial.println();
    Serial.print("  CSD version   ");
    Serial.println(card.csd_version);
    Serial.print("  blocks        ");
    Serial.println(card.block_count);
    Serial.print("  which is      ");
    Serial.print(card.block_count / 2048u);   /* 512-byte blocks -> MiB */
    Serial.println(" MiB");

    /* The name in the CID is printable ASCII on every card that has one, and
     * a wrong decode of the CID makes that obvious immediately. */
    Serial.print("  CID name      ");
    {
        uint32_t w0 = card.cid[0], w1 = card.cid[1];
        char name[6];

        name[0] = (char)(w0 & 0xFF);
        name[1] = (char)((w1 >> 24) & 0xFF);
        name[2] = (char)((w1 >> 16) & 0xFF);
        name[3] = (char)((w1 >> 8) & 0xFF);
        name[4] = (char)(w1 & 0xFF);
        name[5] = 0;
        for (int i = 0; i < 5; i++)
            if (name[i] < 0x20 || name[i] >= 0x7F)
                name[i] = '.';
        Serial.println(name);
    }

    /* --- 5: read sector 0 ----------------------------------------- */

    Serial.println();
    Serial.println("--- step 4: sector 0 ---");
    err = sl6806_sd_read_block(0, sector);
    Serial.print("  read: ");
    Serial.print(err);
    Serial.print(" (");
    Serial.print(sl6806_sd_strerror(err));
    Serial.println(")");

    if (err == SL6806_SD_OK) {
        dumpSector((const uint8_t *)sector);
        describeSector((const uint8_t *)sector);

        /* Read it again. A block that comes back identical twice is a block,
         * not a stale FIFO - the mistake 7l made with a register that changed
         * on every read, and cheap to rule out here. */
        {
            static uint32_t again[128];
            bool same = true;

            if (sl6806_sd_read_block(0, again) == SL6806_SD_OK) {
                for (unsigned i = 0; i < 128; i++)
                    if (again[i] != sector[i])
                        same = false;
                Serial.print("    re-read is ");
                Serial.println(same ? "identical" : "DIFFERENT - not stable");
            }
        }
    }

    Serial.println();
    Serial.println("done.");
}
