/*
 * sl6806_sd.c - the SD/MMC host at 0x40003000, polled.
 *
 * Every sequence here is a transcription of the mask ROM; sl6806_sd.h says
 * which routine each one comes from and why they are transcribed rather than
 * called. The two rules that shaped the code:
 *
 *   - **Every poll is bounded.** The ROM spins forever on the command-done
 *     bit, on the FIFO, and on the reset. In payload mode those spins happen
 *     inside the boot ROM's USB handler, and a device that never returns from
 *     one is off the bus until it is unplugged, with no log. A bounded poll
 *     that reports a timeout is strictly better than a correct answer nobody
 *     can read.
 *
 *   - **The order is the operation**, as with the module gates
 *     (sl6806_module.h). Argument before command, command before start bit,
 *     data-enable before the length, length before the command that moves
 *     data. Where the ROM performs two accesses this performs two accesses.
 */

#include <stddef.h>

#include "sl6806_sd.h"

#include "sl6806_mmio.h"
#include "sl6806_module.h"
#include "sl6806_padctl.h"
#include "wiring_time.h"

#define SD(off) (SL6806_SD_BASE + (off))

/*
 * Poll bounds. The command one is the mask ROM's own (ROM 0x427C counts down
 * from 0x10000); the rest are this file's, chosen to be far longer than the
 * hardware can plausibly take and still finite. At 64 MHz a bounded loop of
 * 0x10000 MMIO reads is a few milliseconds, which is longer than any response
 * on a bus running at hundreds of kHz.
 */
#define SD_POLL_CMD     0x10000u
#define SD_POLL_FIFO    0x40000u
#define SD_POLL_RESET   0x10000u
#define SD_ACMD41_TRIES 0xFFFFu     /* [V] ROM 0x4310 */
#define SD_ACMD41_MS    1000u       /* the SD specification's own limit */

#define SD_BLOCK_SIZE   512u

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static sl6806_sd_card_t g_card;
static int              g_ready;

/* ------------------------------------------------------------------ */
/* The command word table - [V] mask ROM 0x000040F4                    */
/* ------------------------------------------------------------------ */

/*
 * The ROM turns a command index into a 16-bit word and writes it to CMD with
 * bit 31 added. The table is transcribed verbatim; the field meanings below
 * it are [I], read off the table rather than out of a document, and are
 * recorded because they are what tells you an index this file does not list
 * is not simply "index | 0x2540".
 *
 *   [5:0]   the command index
 *   bit 6   set for every command except CMD0 - [I] expect a response
 *   bit 7   set for CMD2 and CMD9 only - [I] expect a 136-bit response
 *   bit 8   set for everything but CMD0 and CMD1 - [I] check the response CRC
 *           (CMD1 and ACMD41 both answer R3, which has no CRC, yet only CMD1
 *           has the bit clear, so this one is [?] rather than [I])
 *   bit 9   set for CMD17, CMD18, CMD24, CMD25 - [I] a data phase follows
 *   bit 10  clear only for CMD17 and CMD18 - [I] the data direction, clear
 *           meaning card to host
 *   bit 12  set only in the multi-block forms of CMD18 and CMD25
 *   bit 13  set for every command except CMD12
 *   bit 14  set for CMD12 alone - [I] the abort command
 *   bit 15  set for CMD0 alone - [I] no response expected
 *
 * The ROM's multi-block variants (CMD18 as 0x3352, CMD25 as 0x3759) are not
 * here because this driver does not issue them; see sl6806_sd_read_blocks().
 */
static uint32_t sd_cmd_word(unsigned index)
{
    switch (index) {
    case 0:  return 0xA400u;
    case 1:  return 0x2441u;
    case 2:  return 0x25C2u;
    case 3:  return 0x2543u;
    case 6:  return 0x2546u;
    case 7:  return 0x2547u;
    case 8:  return 0x2548u;
    case 9:  return 0x25C9u;
    case 12: return 0x454Cu;
    case 13: return 0x254Du;
    case 16: return 0x2550u;
    case 17: return 0x2351u;
    case 18: return 0x2352u;
    case 24: return 0x2758u;
    case 25: return 0x2759u;
    case 41: return 0x2569u;
    case 55: return 0x2577u;
    default: return 0u;
    }
}

/* ------------------------------------------------------------------ */
/* Sending, and the six ways the ROM waits for an answer                */
/* ------------------------------------------------------------------ */

/*
 * [V] ROM 0x000040D2. Argument first, then wait out any command still in
 * flight, then the word with the start bit. The ROM's wait is unbounded;
 * this one is not, and a busy controller is reported rather than spun on.
 */
static int sd_send(unsigned index, uint32_t arg)
{
    uint32_t word = sd_cmd_word(index);
    unsigned n;

    if (word == 0u)
        return SL6806_SD_ERR_STATE;

    sl6806_mmio_write(SD(SL6806_SD_ARG), arg);

    for (n = 0; n < SD_POLL_CMD; n++)
        if (!(sl6806_mmio_read(SD(SL6806_SD_CMD)) & 0x80000000u))
            break;
    if (n == SD_POLL_CMD)
        return SL6806_SD_ERR_CMD_TMOUT;

    sl6806_mmio_write(SD(SL6806_SD_CMD), word | 0x80000000u);
    return SL6806_SD_OK;
}

/* Wait for the command-done bit. Returns 0, or 1 on timeout. */
static int sd_wait_done(void)
{
    unsigned n;

    for (n = 0; n < SD_POLL_CMD; n++)
        if (sl6806_mmio_read(SD(SL6806_SD_STA)) & SL6806_SD_STA_CMDDONE)
            return 0;
    return 1;
}

static void sd_clear(uint32_t bits)
{
    sl6806_mmio_write(SD(SL6806_SD_STA), bits);
}

/*
 * [V] ROM 0x0000427C - CMD0, which expects no response at all. The bit still
 * sets, because it means "the command went out".
 */
static int sd_wait_sent(void)
{
    if (sd_wait_done())
        return SL6806_SD_ERR_CMD_TMOUT;
    sd_clear(SL6806_SD_STA_CLEAR_ALL);
    return SL6806_SD_OK;
}

/*
 * THE ONE PLACE THIS DEPARTS FROM THE ROM, and why.
 *
 * Every one of the ROM's response waits tests a wide error mask before it
 * tests the timeout bit - and the timeout bit, bit 8, is inside the wide
 * mask. So a command nobody answered comes back as the generic error 32, and
 * the ROM recovers the real cause afterwards by reading the status register a
 * second time and looking at bit 8 by hand (0x00004388, in the middle of the
 * identification sequence, to tell an MMC from an SD card).
 *
 * Two changes, both deliberate:
 *
 *   - the timeout is tested first, so "nobody answered" is reported as itself
 *     rather than reconstructed by a caller. That distinction is the whole of
 *     how an empty slot is told from a controller that is not running, and it
 *     is the first thing examples/SdProbe reports;
 *
 *   - every path clears the ROM's full acknowledge mask, 0xBFC6, rather than
 *     the one bit it happened to test. The ROM's paths clear variously
 *     nothing, 0x40, 0x100 or 0xBFC6, which is what leaves bit 8 standing for
 *     it to find later. Since nothing here reads the status after the fact, a
 *     surviving error bit would only be there to poison the next command's
 *     check.
 *
 * `error_mask` and `check_crc` are still each function's own, and are the
 * ROM's: the masks differ between response types on purpose, and R3 has no
 * CRC to check.
 */
static int sd_check(uint32_t sta, uint32_t error_mask, int check_crc)
{
    if (sta & SL6806_SD_STA_CMDTMOUT) {
        sd_clear(SL6806_SD_STA_CLEAR_ALL);
        return SL6806_SD_ERR_CMD_TMOUT;
    }
    if (check_crc && (sta & SL6806_SD_STA_CMDCRC)) {
        sd_clear(SL6806_SD_STA_CLEAR_ALL);
        return SL6806_SD_ERR_CMD_CRC;
    }
    if (sta & error_mask) {
        sd_clear(SL6806_SD_STA_CLEAR_ALL);
        return SL6806_SD_ERR_GENERAL;
    }
    return SL6806_SD_OK;
}

/*
 * [V] ROM 0x00004C90 - the R1 wait, and the only one that checks the echoed
 * command index. The ROM then decodes 18 distinct card-status errors out of
 * R1; this reports one, because the caller can read R1 itself and the
 * distinction has never mattered to anything here. The mask is the ROM's own
 * (the literal at 0x00004D80).
 */
#define SD_R1_ERRORS 0xFDFFE008u

static int sd_wait_r1(unsigned index)
{
    int err;

    if (sd_wait_done())
        return SL6806_SD_ERR_CMD_TMOUT;

    err = sd_check(sl6806_mmio_read(SD(SL6806_SD_STA)),
                   SL6806_SD_STA_ERRORS, 1);
    if (err != SL6806_SD_OK)
        return err;

    if (SL6806_SD_STA2_RESPCMD(sl6806_mmio_read(SD(SL6806_SD_STA2))) != index)
        return SL6806_SD_ERR_CMD_INDEX;

    sd_clear(SL6806_SD_STA_CMDDONE | SL6806_SD_STA_ERRORS);

    if (sl6806_mmio_read(SD(SL6806_SD_RESP0)) & SD_R1_ERRORS)
        return SL6806_SD_ERR_GENERAL;
    return SL6806_SD_OK;
}

/* [V] ROM 0x0000440C - the 136-bit response, CMD2 and CMD9. No index check:
 * a long response carries no index to echo. */
static int sd_wait_r2(void)
{
    int err;

    if (sd_wait_done())
        return SL6806_SD_ERR_CMD_TMOUT;

    err = sd_check(sl6806_mmio_read(SD(SL6806_SD_STA)),
                   SL6806_SD_STA_ERRORS_NC, 1);
    if (err != SL6806_SD_OK)
        return err;

    sd_clear(SL6806_SD_STA_CLEAR_ALL);
    return SL6806_SD_OK;
}

/*
 * [V] ROM 0x000042E2 - R3, the OCR. The error mask is 0xBF80 rather than
 * 0xBFC2: bit 6 is left out because R3 carries no CRC and the controller
 * will flag one. Do not "unify" this with the others.
 */
static int sd_wait_r3(void)
{
    int err;

    if (sd_wait_done())
        return SL6806_SD_ERR_CMD_TMOUT;

    err = sd_check(sl6806_mmio_read(SD(SL6806_SD_STA)), 0x0000BF80u, 0);
    if (err != SL6806_SD_OK)
        return err;

    sd_clear(SL6806_SD_STA_CLEAR_ALL);
    return SL6806_SD_OK;
}

/* [V] ROM 0x000042A2 - R7, the CMD8 echo. A timeout here is the ordinary
 * answer from a v1 card and from an empty slot, so it is not an error to the
 * caller, only to this function. */
static int sd_wait_r7(void)
{
    int err;

    if (sd_wait_done())
        return SL6806_SD_ERR_CMD_TMOUT;

    /* No CRC test: the ROM does not make one here either, and a v1 card
     * simply does not answer, which is the timeout above. */
    err = sd_check(sl6806_mmio_read(SD(SL6806_SD_STA)),
                   SL6806_SD_STA_ERRORS_NC, 0);
    if (err != SL6806_SD_OK)
        return err;

    sd_clear(SL6806_SD_STA_CLEAR_ALL);
    return SL6806_SD_OK;
}

/* [V] ROM 0x00004448 - R6, CMD3's published address. The card can refuse
 * here, in bits [15:13] of the response. */
static int sd_wait_r6(uint16_t *rca)
{
    uint32_t resp;
    int err;

    if (sd_wait_done())
        return SL6806_SD_ERR_CMD_TMOUT;

    err = sd_check(sl6806_mmio_read(SD(SL6806_SD_STA)),
                   SL6806_SD_STA_ERRORS_NC, 1);
    if (err != SL6806_SD_OK)
        return err;

    if (SL6806_SD_STA2_RESPCMD(sl6806_mmio_read(SD(SL6806_SD_STA2))) != 3u)
        return SL6806_SD_ERR_CMD_INDEX;

    sd_clear(SL6806_SD_STA_CLEAR_ALL);

    resp = sl6806_mmio_read(SD(SL6806_SD_RESP0));
    if (resp & 0x0000E000u)
        return SL6806_SD_ERR_GENERAL;

    if (rca)
        *rca = (uint16_t)(resp >> 16);
    return SL6806_SD_OK;
}

/* ------------------------------------------------------------------ */
/* Public command layer                                                */
/* ------------------------------------------------------------------ */

int sl6806_sd_command(unsigned index, uint32_t arg)
{
    int err = sd_send(index, arg);

    if (err != SL6806_SD_OK)
        return err;

    /* Which wait belongs to which index is fixed by the command word: the
     * table above sets bit 7 for the two long responses, bit 15 for the one
     * command with no response, and the SD specification fixes the rest. */
    switch (index) {
    case 0:            return sd_wait_sent();
    case 2:  case 9:   return sd_wait_r2();
    case 3:            return sd_wait_r6(NULL);
    case 8:            return sd_wait_r7();
    case 1:  case 41:  return sd_wait_r3();
    default:           return sd_wait_r1(index);
    }
}

uint32_t sl6806_sd_response(unsigned which)
{
    static const uint32_t off[4] = {
        SL6806_SD_RESP0, SL6806_SD_RESP1, SL6806_SD_RESP2, SL6806_SD_RESP3
    };

    if (which > 3u)
        return 0u;
    return sl6806_mmio_read(SD(off[which]));
}

uint32_t sl6806_sd_status(void)
{
    return sl6806_mmio_read(SD(SL6806_SD_STA));
}

/* ------------------------------------------------------------------ */
/* Bring-up                                                            */
/* ------------------------------------------------------------------ */

/*
 * [V] mask ROM 0x0003D3A0. Pads, then the module gate, then the ROM clock.
 *
 * The 5 ms off-and-on pulse on module 36 is the bootloader's rather than the
 * ROM's (0x008227D4, around a delay of 5): the ROM's own bring-up assumes a
 * cold block, and a payload arrives at one that the boot ROM may have used
 * already. Turning a module clock off is the dangerous direction in general -
 * see sl6806_module.h - and this is the case the warning allows for, a module
 * that belongs to the peripheral being started.
 */
static int sd_clocks_on(void)
{
    /* [V] the bootloader's six, 0x0082167C - the product's own bus, not the
     * mask ROM's generic three. See sl6806_sd.h. */
    sl6806_pad_configure(SL6806_SD_PAD_CLK_BL);
    sl6806_pad_configure(SL6806_SD_PAD_CMD_BL);
    sl6806_pad_configure(SL6806_SD_PAD_D0);
    sl6806_pad_configure(SL6806_SD_PAD_D1);
    sl6806_pad_configure(SL6806_SD_PAD_D2);
    sl6806_pad_configure(SL6806_SD_PAD_D3);

    sl6806_module_disable(SL6806_SD_MODULE_ID);
    delay(5);
    if (!sl6806_module_enable(SL6806_SD_MODULE_ID))
        return SL6806_SD_ERR_CLOCK;

    /* The ROM waits here with a bare countdown of 2000 (0x0000BC10) before
     * touching the clock, which is a handful of microseconds. */
    delayMicroseconds(200);

    /* ROM 0x2E5C is called as (17, 10) first. For this id the dispatcher only
     * recognises 8 and 59, which set and clear bit 4 of the same register, so
     * 10 selects neither and nothing is written. */
    sl6806_mmio_set(SL6806_SD_ROMCLK_REG, SL6806_SD_ROMCLK_BIT);

    return SL6806_SD_OK;
}

/* [V] ROM 0x00003D88. Bits 0..2 together; bit 2 clears itself when the reset
 * finishes, and the ROM's wait for it is the unbounded one this bounds. */
static int sd_reset(void)
{
    unsigned n;

    sl6806_mmio_set(SD(SL6806_SD_CTRL),
                    SL6806_SD_CTRL_ENABLE | SL6806_SD_CTRL_RESET);

    for (n = 0; n < SD_POLL_RESET; n++)
        if (!(sl6806_mmio_read(SD(SL6806_SD_CTRL)) & SL6806_SD_CTRL_RESET))
            return SL6806_SD_OK;
    return SL6806_SD_ERR_CLOCK;
}

/*
 * [V] ROM 0x00003D9A, with the operands its two callers supply. `edge` is the
 * word that goes into CTRL[31:30]: the ROM passes 0 in both phases, the
 * bootloader passes 0 for identification and 0x80000000 to run
 * (0x0082A7BC). This follows the ROM, so that the whole sequence comes from
 * one source - if a card is identified and then fails at speed, the
 * bootloader's extra bit is the first thing to try.
 *
 * The write to CMD of 0x8020 with no start bit, followed by a wait for the
 * busy bit, is the ROM's and its purpose is [?]. Bit 15 marks a command that
 * expects no response and index 0x20 is not an SD command, so the best guess
 * is that it makes the controller emit initialisation clocks.
 */
static int sd_clock_config(uint32_t edge, uint32_t clkcr)
{
    unsigned n;

    sl6806_mmio_write(SD(SL6806_SD_CTRL),
                      (sl6806_mmio_read(SD(SL6806_SD_CTRL)) & ~0xC0000000u)
                      | edge | SL6806_SD_CTRL_CLKEN);

    sl6806_mmio_write(SD(SL6806_SD_CMD), 0x00008020u);
    for (n = 0; n < SD_POLL_CMD; n++)
        if (!(sl6806_mmio_read(SD(SL6806_SD_CMD)) & 0x80000000u))
            break;
    if (n == SD_POLL_CMD)
        return SL6806_SD_ERR_CMD_TMOUT;

    sd_clear(sl6806_mmio_read(SD(SL6806_SD_STA)) & SL6806_SD_STA_CMDDONE);

    sl6806_mmio_write(SD(SL6806_SD_CLKCR),
                      (sl6806_mmio_read(SD(SL6806_SD_CLKCR))
                       & ~SL6806_SD_CLKCR_MASK) | clkcr);

    sl6806_mmio_write(SD(SL6806_SD_REG64), SL6806_SD_REG64_VALUE);
    sl6806_mmio_set(SD(SL6806_SD_REG100), 2u);
    sl6806_mmio_write(SD(SL6806_SD_DTIMER), SL6806_SD_DTIMER_DEFAULT);

    return SL6806_SD_OK;
}

/*
 * [V] ROM 0x00004310. CMD0, then CMD8 to find out whether the card
 * understands version 2, then CMD55+ACMD41 until the card reports it has
 * finished powering up. The argument the ROM builds is 0x80100000 with
 * 0x40000000 added when CMD8 answered - the host-capacity-support bit - and
 * bit 31 in an ACMD41 argument is the vendor's own oddity, not a typo here.
 */
static int sd_power_on(void)
{
    uint32_t arg = 0x80100000u;
    uint32_t resp;
    uint32_t t0;
    unsigned tries;
    int err;

    g_card.type = SL6806_SD_TYPE_SDSC_V1;
    sd_clear(SL6806_SD_STA_CLEAR_ALL);

    /* CMD0 expects no response, so the bit it raises means "the command went
     * out" and nothing more. An empty slot does not fail here - it fails at
     * CMD8 and CMD55 below - so a timeout on CMD0 says the controller is not
     * running, which is a different fault and gets a different code. */
    err = sd_send(0, 0);
    if (err != SL6806_SD_OK)
        return err;
    err = sd_wait_sent();
    if (err != SL6806_SD_OK)
        return err;

    err = sd_send(8, 0x1AAu);
    if (err != SL6806_SD_OK)
        return err;
    if (sd_wait_r7() == SL6806_SD_OK) {
        g_card.type = SL6806_SD_TYPE_SDSC_V2;
        arg |= 0x40000000u;
    }

    /*
     * The ROM's loop runs 65535 times with no delay and no clock. A card that
     * answers every ACMD41 with "still busy" would hold the CPU for seconds,
     * and in RUN_MODE=poll that is time the boot ROM's USB handler does not
     * get. The SD specification gives a card one second to finish powering
     * up, so that is the second bound here - and if millis() is frozen (see
     * wiring_time.h) the ROM's count still ends the loop.
     */
    t0 = millis();
    for (tries = 0; tries < SD_ACMD41_TRIES; tries++) {
        if (tries && (millis() - t0) > SD_ACMD41_MS)
            break;

        err = sd_send(55, 0);
        if (err != SL6806_SD_OK)
            return err;
        err = sd_wait_r1(55);
        if (err != SL6806_SD_OK) {
            /*
             * [V] a timeout on CMD55 is where the ROM branches to CMD1 and
             * brings the card up as an MMC. This driver does not: MMC is a
             * second identification path, and nothing in this slot has ever
             * answered anything, so there would be no way to tell a working
             * transcription from a broken one.
             *
             * A card that answered CMD8 and then ignores CMD55 is an SD card
             * behaving strangely, and gets the timeout. One that answered
             * neither is an empty slot or an MMC - indistinguishable without
             * sending CMD1 - and gets "no card", which is the far commoner
             * of the two and the one worth naming.
             */
            if (err == SL6806_SD_ERR_CMD_TMOUT)
                return (g_card.type == SL6806_SD_TYPE_SDSC_V1)
                       ? SL6806_SD_ERR_NO_CARD : err;
            if (err != SL6806_SD_ERR_CMD_INDEX)
                return err;
        }

        err = sd_send(41, arg);
        if (err != SL6806_SD_OK)
            return err;
        err = sd_wait_r3();
        if (err != SL6806_SD_OK)
            return err;

        resp = sl6806_mmio_read(SD(SL6806_SD_RESP0));
        if (resp & 0x80000000u) {
            g_card.ocr = resp;
            if (resp & 0x40000000u)
                g_card.type = SL6806_SD_TYPE_SDHC;
            return SL6806_SD_OK;
        }
    }
    return SL6806_SD_ERR_BUSY;
}

/*
 * [V] ROM 0x000044BC. CMD2 for the CID, CMD3 for the address the card picks,
 * CMD9 for the CSD. The responses are stored most-significant word first,
 * which is RESP3 down to RESP0 - established by the ROM's own CSD parse
 * taking CSD_STRUCTURE out of what it stored from RESP3.
 */
static int sd_identify(void)
{
    int err;

    err = sd_send(2, 0);
    if (err != SL6806_SD_OK)
        return err;
    err = sd_wait_r2();
    if (err != SL6806_SD_OK)
        return err;

    g_card.cid[0] = sl6806_mmio_read(SD(SL6806_SD_RESP3));
    g_card.cid[1] = sl6806_mmio_read(SD(SL6806_SD_RESP2));
    g_card.cid[2] = sl6806_mmio_read(SD(SL6806_SD_RESP1));
    g_card.cid[3] = sl6806_mmio_read(SD(SL6806_SD_RESP0));

    err = sd_send(3, 0x10000u);
    if (err != SL6806_SD_OK)
        return err;
    err = sd_wait_r6(&g_card.rca);
    if (err != SL6806_SD_OK)
        return err;

    err = sd_send(9, (uint32_t)g_card.rca << 16);
    if (err != SL6806_SD_OK)
        return err;
    err = sd_wait_r2();
    if (err != SL6806_SD_OK)
        return err;

    g_card.csd[0] = sl6806_mmio_read(SD(SL6806_SD_RESP3));
    g_card.csd[1] = sl6806_mmio_read(SD(SL6806_SD_RESP2));
    g_card.csd[2] = sl6806_mmio_read(SD(SL6806_SD_RESP1));
    g_card.csd[3] = sl6806_mmio_read(SD(SL6806_SD_RESP0));

    g_card.csd_version = (uint8_t)(g_card.csd[0] >> 30);
    g_card.block_count = sl6806_sd_capacity(g_card.csd);

    return SL6806_SD_OK;
}

int sl6806_sd_controller_begin(uint32_t edge, uint32_t clkcr)
{
    int err;

    err = sd_clocks_on();
    if (err != SL6806_SD_OK)
        return err;

    err = sd_reset();
    if (err != SL6806_SD_OK)
        return err;

    return sd_clock_config(edge, clkcr ? clkcr : SL6806_SD_CLKCR_IDENT);
}

int sl6806_sd_begin(sl6806_sd_card_t *card)
{
    int err;
    unsigned i;

    g_ready = 0;
    for (i = 0; i < sizeof(g_card); i++)
        ((unsigned char *)&g_card)[i] = 0;

    err = sl6806_sd_controller_begin(0u, SL6806_SD_CLKCR_IDENT);
    if (err != SL6806_SD_OK)
        return err;

    err = sd_power_on();
    if (err != SL6806_SD_OK)
        goto out;

    /* [V] the ROM's 30 ms between power-up and identification (0x000045E0). */
    delay(30);

    err = sd_identify();
    if (err != SL6806_SD_OK)
        goto out;

    /* CMD7, and only then the run-speed clock - the ROM's order (0x0000460A
     * then 0x00004628). Selecting the card at the identification clock and
     * changing the clock afterwards is the sequence; doing it the other way
     * round changes the bus rate while the card is still deselected. */
    err = sd_send(7, (uint32_t)g_card.rca << 16);
    if (err != SL6806_SD_OK)
        goto out;
    err = sd_wait_r1(7);
    if (err != SL6806_SD_OK)
        goto out;

    err = sd_clock_config(0u, SL6806_SD_CLKCR_RUN);
    if (err != SL6806_SD_OK)
        goto out;

    /* [V] ROM 0x000047C4 sends this the first time a read happens; hoisted
     * here so a failure lands in begin() where it can be reported. */
    err = sd_send(16, SD_BLOCK_SIZE);
    if (err != SL6806_SD_OK)
        goto out;
    err = sd_wait_r1(16);
    if (err != SL6806_SD_OK)
        goto out;

    g_ready = 1;

out:
    if (card)
        *card = g_card;
    return err;
}

void sl6806_sd_end(void)
{
    /* [V] ROM 0x0003D2F0: the same three pads on function 15, then the
     * module gate, then the ROM clock. */
    sl6806_pad_configure(0x00016790u);
    sl6806_pad_configure(0x00017790u);
    sl6806_pad_configure(0x00016F90u);

    sl6806_module_disable(SL6806_SD_MODULE_ID);
    sl6806_mmio_clr(SL6806_SD_ROMCLK_REG, SL6806_SD_ROMCLK_BIT);

    g_ready = 0;
}

/* ------------------------------------------------------------------ */
/* Capacity - [V] ROM 0x00003E5E and 0x00004086                        */
/* ------------------------------------------------------------------ */

/*
 * The ROM switches on its own CardType here rather than on the CSD; this
 * switches on CSD_STRUCTURE, which is the same distinction taken from the
 * card instead of from the driver's bookkeeping. Both arrive at the same two
 * cases, and this one can be tested without a controller.
 */
uint32_t sl6806_sd_capacity(const uint32_t csd[4])
{
    uint32_t c_size, mult, read_bl_len, blocks;

    switch (csd[0] >> 30) {
    case 0u:
        /* C_SIZE is CSD[73:62], C_SIZE_MULT is [49:47], READ_BL_LEN [83:80]. */
        c_size      = ((csd[1] & 0x000003FFu) << 2) | (csd[2] >> 30);
        mult        = (csd[2] >> 15) & 0x7u;
        read_bl_len = (csd[1] >> 16) & 0xFu;

        if (read_bl_len < 9u || read_bl_len > 11u)
            return 0u;

        /* (C_SIZE + 1) << (C_SIZE_MULT + 2) blocks of 1 << READ_BL_LEN bytes,
         * expressed in 512-byte blocks. */
        blocks = (c_size + 1u) << (mult + 2u);
        return blocks << (read_bl_len - 9u);

    case 1u:
        /* C_SIZE is CSD[69:48] and the unit is 512 KB. */
        c_size = ((csd[1] & 0x0000003Fu) << 16) | (csd[2] >> 16);
        return (c_size + 1u) << 10;

    default:
        return 0u;
    }
}

/* ------------------------------------------------------------------ */
/* Reading                                                             */
/* ------------------------------------------------------------------ */

/* [V] ROM 0x0000464A: wait for the receive FIFO to stop reading empty, then
 * take one word out of the data port. */
static int sd_fifo_read(uint32_t *out)
{
    unsigned n;

    for (n = 0; n < SD_POLL_FIFO; n++) {
        if (!(sl6806_mmio_read(SD(SL6806_SD_STA2)) & SL6806_SD_STA2_RXEMPTY)) {
            *out = sl6806_mmio_read(SD(SL6806_SD_FIFO));
            return SL6806_SD_OK;
        }
    }
    return SL6806_SD_ERR_DATA_TMOUT;
}

/*
 * [V] ROM 0x00004806. Eight words at a time, because the status bit the loop
 * waits on says eight words are there, and the acknowledgement of that bit is
 * what asks for the next eight. Reading one word per status check would be a
 * different conversation with the block.
 */
static int sd_drain(uint32_t *p)
{
    uint32_t *end = p + SD_BLOCK_SIZE / 4u;
    unsigned n = 0;
    uint32_t sta;
    int err;

    while (p != end) {
        sta = sl6806_mmio_read(SD(SL6806_SD_STA));
        if (sta & SL6806_SD_STA_ERRORS) {
            sd_clear(SL6806_SD_STA_ERRORS);
            return SL6806_SD_ERR_DATA;
        }
        if (!(sta & SL6806_SD_STA_RXHALF)) {
            if (++n >= SD_POLL_FIFO)
                return SL6806_SD_ERR_DATA_TMOUT;
            continue;
        }
        n = 0;

        {
            unsigned i;
            for (i = 0; i < 8u; i++) {
                err = sd_fifo_read(p);
                if (err != SL6806_SD_OK)
                    return err;
                p++;
            }
        }
        sd_clear(SL6806_SD_STA_RXHALF);
    }

    if (sl6806_mmio_read(SD(SL6806_SD_STA)) & SL6806_SD_STA_ERRORS) {
        sd_clear(SL6806_SD_STA_ERRORS);
        return SL6806_SD_ERR_DATA;
    }
    return SL6806_SD_OK;
}

int sl6806_sd_read_block(uint32_t lba, void *buf)
{
    uint32_t addr;
    int err;

    if (!g_ready)
        return SL6806_SD_ERR_STATE;
    if (!buf || ((uintptr_t)buf & 3u))
        return SL6806_SD_ERR_STATE;

    /* [V] ROM 0x00004790: everything but a high-capacity card is addressed
     * in bytes. */
    addr = (g_card.type == SL6806_SD_TYPE_SDHC) ? lba : (lba << 9);

    sd_clear(SL6806_SD_STA_CLEAR_ALL);

    /* [V] ROM 0x0000479C then 0x00004680: the data-enable bit first, then the
     * timeout, the block size and the length, then the command. */
    sl6806_mmio_set(SD(SL6806_SD_DCTRL), SL6806_SD_DCTRL_DATAEN);
    sl6806_mmio_write(SD(SL6806_SD_DTIMER), SL6806_SD_DTIMER_DEFAULT);
    sl6806_mmio_write(SD(SL6806_SD_BLKSIZE), SD_BLOCK_SIZE);
    sl6806_mmio_write(SD(SL6806_SD_DLEN), SD_BLOCK_SIZE);

    err = sd_send(17, addr);
    if (err != SL6806_SD_OK)
        return err;
    err = sd_wait_r1(17);
    if (err != SL6806_SD_OK)
        return err;

    return sd_drain((uint32_t *)buf);
}

int sl6806_sd_read_blocks(uint32_t lba, void *buf, uint32_t count,
                          uint32_t *done)
{
    unsigned char *p = (unsigned char *)buf;
    uint32_t i;
    int err = SL6806_SD_OK;

    for (i = 0; i < count; i++) {
        err = sl6806_sd_read_block(lba + i, p);
        if (err != SL6806_SD_OK)
            break;
        p += SD_BLOCK_SIZE;
    }
    if (done)
        *done = i;
    return err;
}

const char *sl6806_sd_strerror(int err)
{
    switch (err) {
    case SL6806_SD_OK:             return "ok";
    case SL6806_SD_ERR_CMD_CRC:    return "response CRC failed";
    case SL6806_SD_ERR_CMD_TMOUT:  return "no response";
    case SL6806_SD_ERR_CMD_INDEX:  return "wrong command echoed";
    case SL6806_SD_ERR_BUSY:       return "card never finished powering up";
    case SL6806_SD_ERR_GENERAL:    return "controller or card reported an error";
    case SL6806_SD_ERR_NO_CARD:    return "no card (or an MMC)";
    case SL6806_SD_ERR_STATE:      return "not initialised, or bad argument";
    case SL6806_SD_ERR_DATA:       return "data phase failed";
    case SL6806_SD_ERR_DATA_TMOUT: return "data phase never delivered";
    case SL6806_SD_ERR_CLOCK:      return "controller clock would not come up";
    default:                       return "unknown";
    }
}
