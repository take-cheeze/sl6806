/*
 * sl6806_sd.h - the SD/MMC host controller, and a polled block reader on it.
 *
 * =====================================================================
 *  WHERE THIS CAME FROM, AND WHAT IT CORRECTS
 * =====================================================================
 * docs/sl6806_re_notes.md 7c files `0x400F7000` as "storage host (SD/MMC +
 * SPI flash)" on the strength of "+0x104 command with a bit-31 start/busy,
 * +0x108 argument, +0x10C/+0x110 response, `sdio(e):rx error` strings
 * nearby". Two of those three are now known to be something else:
 *
 *   - the registers at `0x400F7000+0x100` are the mailbox to the chip that
 *     carries the power rails (7m, cores/sl6806/sl6806_regfile.h). They do
 *     have a bit-31 start and a command/argument/response shape, which is
 *     exactly why they read as an SD host;
 *   - the `sdio(...)` strings are in the *bootloader*, at file offsets below
 *     0x10000, and the code that loads them lives at 0x00822xxx.
 *
 * **The SD/MMC host is `0x40003000`.** It is written down once in the whole
 * dump, at the bootloader's `HAL_sd_disk_init` (0x00822768), which stores it
 * into the handle it hands to the driver; and once in the mask ROM, at
 * 0x0003D398. Everything else reaches it through that handle, which is why a
 * literal scan for peripheral bases never ranked it (7c's own blind-spot
 * note: "if a peripheral is missing, look for the table it is cached in").
 *
 * THE DRIVER IS IN THE MASK ROM. As with GPIO (7f) and the pad controller,
 * flash contains only the callers. The command layer, the identification
 * sequence, the CSD parse and a polled FIFO drain are all ROM routines:
 *
 *     ROM 0x000040D2   write ARG then CMD, spin on the start bit
 *     ROM 0x000040F4   command index -> the command word (the table below)
 *     ROM 0x000040E6   data timeout, block size, length
 *     ROM 0x00004310   CMD0 / CMD8 / CMD55+ACMD41, or CMD1 for MMC
 *     ROM 0x000044BC   CMD2 (CID), CMD3 (RCA), CMD9 (CSD)
 *     ROM 0x00003E5E   the CSD parse and the capacity arithmetic
 *     ROM 0x00004778   set up a block read: DCTRL, DLEN, then CMD17/CMD18
 *     ROM 0x00004806   drain 512 bytes out of the FIFO, eight words at a time
 *     ROM 0x00004C90   wait for a response and turn the status into an errno
 *     ROM 0x00003D3A0  the bring-up: pads, module 36, ROM clock 17
 *
 * All of it is transcribed here rather than called. The ROM's routines take a
 * 0xCC-byte handle whose layout is only partly understood, several of them
 * spin without a bound - fatal in payload mode, where an unbounded spin runs
 * inside the boot ROM's USB handler and takes the device off the bus - and
 * two of them dispatch through a callback the handle carries. Transcribing
 * costs a page of C and makes every poll finite.
 *
 * Provenance markers as elsewhere: [V] verified against the dump or the mask
 * ROM, [M] measured on hardware, [I] inferred, [?] unknown.
 *
 * =====================================================================
 *  NOTHING HERE HAS RUN ON HARDWARE
 * =====================================================================
 * As of 2026-08-13 this is a decode, not a result. The block has never been
 * read from a payload, the card slot has never answered anything, and the
 * two things most likely to be wrong are named up front:
 *
 *   1. **The bring-up may be incomplete.** It is the mask ROM's own, which is
 *      a strong provenance - but the PWM (14a) and the camera front end (7n)
 *      both took a full register configuration and did not run, and for both
 *      the missing piece was a functional-clock bit in `0x400E0000` that no
 *      transcription mentioned because the vendor's code for those blocks
 *      does not mention it either. If the controller here accepts writes and
 *      no command ever completes, that register is where to look next, and
 *      sl6806_module.h says how to reach it.
 *
 *   2. **The card-detect pin is not known.** There is no card-detect bit in
 *      this block that anything in the dump reads. The bootloader configures
 *      a bank-2 pad as an input with a pull (0x0002280B, at 0x00822310) next
 *      to code that touches the *other* storage host, and that is a guess,
 *      not a finding. sl6806_sd_begin() therefore reports "no card" as a
 *      timeout on CMD0/ACMD41 rather than by reading a pin.
 *
 * =====================================================================
 *  REGISTER MAP
 * =====================================================================
 * Offsets from SL6806_SD_BASE. Everything marked [V] is read directly out of
 * the mask ROM at the address given.
 *
 *   +0x000  CTRL     [V] bits 0..2 set together and bit 2 self-clears - a
 *                        reset the ROM waits on (0x3D88). Bits 31:30 come
 *                        from the caller's config; bit 25 and bit 2 are set
 *                        again around a transfer (0x4734), bit 3 is toggled
 *                        on its own (0x4668 / 0x4674). [?] individually.
 *   +0x004  CMD      [V] the command word, bit 31 start/busy (0x40D2)
 *   +0x008  ARG      [V] the command argument (0x40D2)
 *   +0x00C  BLKSIZE  [V] 512 in every use (0x40E6, 0x4680)
 *   +0x010  DLEN     [V] byte count for the data phase
 *   +0x014  DTIMER   [V] data timeout, 0xFFFFFF40 out of reset (0x3E22)
 *   +0x018  [?]      the bootloader clears bit 0 here (0x0082A82C)
 *   +0x02C  DCTRL    [V] bit 3 enables the data phase, bit 14 marks a
 *                        multi-block transfer (0x4778)
 *   +0x034  STA      [V] status, write-1-to-clear. Bits below.
 *   +0x038  RESP0    [V] response bits [31:0]
 *   +0x03C  RESP1    [V] response bits [63:32]
 *   +0x040  RESP2    [V] response bits [95:64]
 *   +0x044  RESP3    [V] response bits [127:96] - the MSB, confirmed by the
 *                        ROM's CSD parse reading CSD_STRUCTURE out of it
 *   +0x048  STA2     [V] bit 2 receive FIFO empty, bit 3 transmit FIFO full
 *                        (0x464A / 0x4658); bits [16:11] echo the index of
 *                        the command that was answered (0x41D8)
 *   +0x04C  CLKCR    [V] clock control. [22:16] is a divider - 7 while the
 *                        card is being identified and 3 afterwards - and
 *                        [30:23] takes one of two bits, 0x20000000 during
 *                        identification and 0x10000000 to run. [?] which is
 *                        which; the values are transcribed, not derived.
 *   +0x064  [?]      [V] written 0xFFF (bootloader) or 0xFFFFFF (ROM)
 *   +0x080  DMACTL   [V] the DMA path's, unused here (0x46D8, 0x4760)
 *   +0x100  [?]      [V] bit 1 is set during clock setup (0x3E16)
 *   +0x200  FIFO     [V] the data port, 32 bits at a time (0x464A / 0x4658)
 */
#ifndef SL6806_SD_H
#define SL6806_SD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* [V] mask ROM 0x0003D398, bootloader 0x00822958. */
#define SL6806_SD_BASE          0x40003000u

#define SL6806_SD_CTRL          0x000u
#define SL6806_SD_CMD           0x004u
#define SL6806_SD_ARG           0x008u
#define SL6806_SD_BLKSIZE       0x00Cu
#define SL6806_SD_DLEN          0x010u
#define SL6806_SD_DTIMER        0x014u
#define SL6806_SD_REG18         0x018u
#define SL6806_SD_DCTRL         0x02Cu
#define SL6806_SD_STA           0x034u
#define SL6806_SD_RESP0         0x038u
#define SL6806_SD_RESP1         0x03Cu
#define SL6806_SD_RESP2         0x040u
#define SL6806_SD_RESP3         0x044u
#define SL6806_SD_STA2          0x048u
#define SL6806_SD_CLKCR         0x04Cu
#define SL6806_SD_REG64         0x064u
#define SL6806_SD_DMACTL        0x080u
#define SL6806_SD_REG100        0x100u
#define SL6806_SD_FIFO          0x200u

/* CTRL. [V] the three the ROM sets together, and the reset it waits on. */
#define SL6806_SD_CTRL_ENABLE   0x00000003u   /* [I] bits 0 and 1 */
#define SL6806_SD_CTRL_RESET    0x00000004u   /* [V] self-clearing, 0x3D88 */
#define SL6806_SD_CTRL_CLKEN    0x40000000u   /* [I] always or'd in, 0x3D9A */

/*
 * STA. [V] every bit here is one the ROM tests or clears by name.
 *
 * The clear-all mask is the ROM's own 0xBFC6 (0x4310, 0x427C, 0x42A2), which
 * is the union of everything it ever acknowledges. Bit 0 and bits 3..5 are
 * not in it: bit 5 is cleared on its own inside the FIFO drain, and 0 and
 * 3..4 are never written.
 */
#define SL6806_SD_STA_CMDDONE   (1u << 2)     /* [V] response received */
#define SL6806_SD_STA_RXHALF    (1u << 5)     /* [V] >= 8 words in the FIFO */
#define SL6806_SD_STA_CMDCRC    (1u << 6)     /* [V] -> error 1 (0x4CB4) */
#define SL6806_SD_STA_CMDTMOUT  (1u << 8)     /* [V] -> error 3 (0x4CA4) */
#define SL6806_SD_STA_ERRORS    0x00000300u   /* [V] tested as a pair, 0x4C9C */
#define SL6806_SD_STA_ERRORS_NC 0x0000BFC2u   /* [V] the wider mask, 0x440C */
#define SL6806_SD_STA_CLEAR_ALL 0x0000BFC6u   /* [V] 0x4310 */

/* STA2. [V] 0x464A, 0x4658, 0x41D8. */
#define SL6806_SD_STA2_RXEMPTY  (1u << 2)
#define SL6806_SD_STA2_TXFULL   (1u << 3)
#define SL6806_SD_STA2_RESPCMD(v) (((v) >> 11) & 0x3Fu)

/* DCTRL. [V] 0x4778. */
#define SL6806_SD_DCTRL_DATAEN  (1u << 3)
#define SL6806_SD_DCTRL_MULTI   (1u << 14)

/*
 * [V] CLKCR, both configurations, transcribed from ROM 0x3D9A and the two
 * callers that supply its operands (ROM 0x455A, bootloader 0x0082A7BC).
 * The fields the ROM clears before or-ing these in are 0x7F800FFF and
 * 0x007F0000, which is what CLKCR_MASK is.
 */
#define SL6806_SD_CLKCR_MASK    0x7FFF0FFFu
#define SL6806_SD_CLKCR_IDENT   (0x20000000u | (7u << 16) | 8u)
#define SL6806_SD_CLKCR_RUN     (0x10000000u | (3u << 16) | 12u)

/* [V] the value the ROM leaves in DTIMER after clock setup, 0x3E22. */
#define SL6806_SD_DTIMER_DEFAULT 0xFFFFFF40u
/* [V] +0x64, the bootloader's value. The ROM writes 0x00FFFFFF. */
#define SL6806_SD_REG64_VALUE   0x00000FFFu

/*
 * [V] The pads, from the mask ROM's bring-up at 0x0003D3A0. Bank 1, pins 12,
 * 13 and 14, all on pad function 2 at drive 1. Twelve carries no pull and the
 * other two carry pull selector 11, which is the difference you would expect
 * between a clock and the two lines that need a pull-up - so pin 12 is [I]
 * the clock and 13/14 are CMD and DAT0 in an order nothing establishes.
 *
 * Three pads is a one-bit bus. The ROM never configures a fourth, so
 * four-bit mode would need a pad this file does not know; see the note on
 * sl6806_sd_begin().
 *
 * The same three ids appear again with function 15 (ROM 0x0003D2F0), which
 * is that routine parking them - so function 15 is [I] the off state and 2 is
 * the SD function.
 */
#define SL6806_SD_PAD_CLK       0x00016110u
#define SL6806_SD_PAD_A         0x0001711Bu
#define SL6806_SD_PAD_B         0x0001691Bu

/*
 * [V] AND THE BOOTLOADER CONFIGURES SIX, which is what this driver uses.
 *
 * The mask ROM's three pads above are the chip's minimum - a one-bit bus. The
 * HLKJ bootloader, at 0x0082167C and immediately before it calls
 * `HAL_sd_disk_init`, configures **bank 1 pins 12 through 17**:
 *
 *     pin 12   function 2, drive 3, no pull      [I] the clock
 *     pin 13   function 2, drive 2, pull 11      [I] the command line
 *     pin 14   function 2, drive 3, pull 8
 *     pin 15   function 2, drive 3, pull 11
 *     pin 16   function 2, drive 3, pull 11
 *     pin 17   function 2, drive 3, pull 11      [I] four data lines
 *
 * Six pads is a four-bit bus, and the bootloader is the code that actually
 * reads cards on this board - it is what `sdupdate` runs on. The ROM's
 * version is the generic one; this is the product's.
 *
 * Note the drive strength as well as the count: 3 where the ROM uses 1, and
 * pin 13 at 2. A bus driven at a third of the strength the vendor uses is
 * worth knowing about even if it is not the current problem.
 *
 * The driver configures all six and still talks one-bit, which is the
 * bootloader's own order of business: it configures the pads once at
 * bring-up and only later decides a bus width with ACMD6. Configuring a pad
 * the driver does not use costs nothing; switching the card to a bus width
 * the pads are not ready for would lose the card.
 */
#define SL6806_SD_PAD_D0        0x00017138u   /* pin 14 */
#define SL6806_SD_PAD_D1        0x0001793Bu   /* pin 15 */
#define SL6806_SD_PAD_D2        0x0001813Bu   /* pin 16 */
#define SL6806_SD_PAD_D3        0x0001893Bu   /* pin 17 */
#define SL6806_SD_PAD_CLK_BL    0x00016130u   /* pin 12, drive 3 */
#define SL6806_SD_PAD_CMD_BL    0x0001692Bu   /* pin 13, drive 2 */

/*
 * [V] The clocks. Module id 36 gates the registers (15b's mechanism, and the
 * bootloader pulses it off-and-on around a 5 ms delay at 0x008227D4 to reset
 * the block); ROM clock id 17 - `0x40080080` bit 0 - is the functional one.
 *
 * ROM 0x2E5C is also called as (17, 10) before the clock is enabled. That
 * dispatcher only recognises 8 and 59 for this id, which set and clear bit 4
 * of the same register, so the vendor's 10 selects neither and the call is a
 * no-op. It is transcribed as a comment rather than as code because
 * performing a no-op is not a fact about the hardware.
 */
#define SL6806_SD_MODULE_ID     36u
#define SL6806_SD_ROMCLK_ID     17u
#define SL6806_SD_ROMCLK_REG    0x40080080u
#define SL6806_SD_ROMCLK_BIT    0x00000001u

/* [V] IRQ 44, from the ROM installing a handler and enabling NVIC bit 12 of
 * the second ISER word (0x0003D3D0). This driver polls and never enables it;
 * it is recorded because a future DMA path would need it. */
#define SL6806_SD_IRQ           44u

/* ------------------------------------------------------------- errors */

/*
 * The first five are the mask ROM's own numbers, returned by ROM 0x4C90 and
 * kept identical so a value seen here means the same thing as a value in the
 * vendor's `errorstate` logging. The driver's own start at 64, above
 * anything the ROM produces.
 */
enum {
    SL6806_SD_OK            = 0,
    SL6806_SD_ERR_CMD_CRC   = 1,    /* [V] response CRC failed */
    SL6806_SD_ERR_CMD_TMOUT = 3,    /* [V] no response */
    SL6806_SD_ERR_CMD_INDEX = 16,   /* [V] the card answered a different index */
    SL6806_SD_ERR_BUSY      = 27,   /* [V] ACMD41 never reported ready */
    SL6806_SD_ERR_GENERAL   = 32,   /* [V] a status error bit was set */

    SL6806_SD_ERR_NO_CARD   = 64,   /* CMD0 got no acknowledgement at all */
    SL6806_SD_ERR_STATE     = 65,   /* read attempted before a successful begin */
    SL6806_SD_ERR_DATA      = 66,   /* the data phase reported an error */
    SL6806_SD_ERR_DATA_TMOUT = 67,  /* the FIFO never filled */
    SL6806_SD_ERR_CLOCK     = 69    /* module 36 would not acknowledge */
};

/* ---------------------------------------------------------- card info */

/* [V] the ROM's CardType, handle+0x40, set in 0x4310. */
enum {
    SL6806_SD_TYPE_SDSC_V1  = 0,
    SL6806_SD_TYPE_SDSC_V2  = 1,
    SL6806_SD_TYPE_SDHC     = 2,    /* [V] the only one addressed by block */
    SL6806_SD_TYPE_MMC      = 7
};

typedef struct {
    uint32_t cid[4];        /* [0] is bits [127:96] */
    uint32_t csd[4];        /* likewise */
    uint32_t ocr;           /* the ACMD41 response */
    uint16_t rca;           /* from CMD3 */
    uint8_t  type;          /* SL6806_SD_TYPE_* */
    uint8_t  csd_version;   /* CSD_STRUCTURE, 0 or 1 */
    uint32_t block_count;   /* capacity in 512-byte blocks */
} sl6806_sd_card_t;

/* ---------------------------------------------------------------- API */

/*
 * Bring the controller up and identify the card. Transcribes the mask ROM's
 * 0x3D3A0 (pads and clocks), 0x455A (reset, clock configuration), 0x4310
 * (CMD0, CMD8, CMD55+ACMD41) and 0x44BC (CMD2, CMD3, CMD9), then selects the
 * card with CMD7 and sets a 512-byte block length with CMD16.
 *
 * `card` may be NULL if the caller only wants the return value.
 *
 * Returns SL6806_SD_OK, or one of the errors above. SL6806_SD_ERR_NO_CARD is
 * the ordinary answer for an empty slot: CMD0 goes out into an idle bus and
 * completes, because it expects no response, and then neither CMD8 nor CMD55
 * is answered. An MMC looks identical at that point - the ROM tries CMD1
 * there and this does not - so "no card" is what both get told.
 *
 * ONE-BIT MODE ONLY, and deliberately. The ROM configures three pads, which
 * is CLK, CMD and one data line; a fourth would have to be found before
 * ACMD6 could be sent, and sending ACMD6 without the pads would switch the
 * card to a bus this chip is not driving and lose it until the next power
 * cycle. Slow and working beats fast and unrecoverable on a peripheral
 * nobody has read yet.
 */
int sl6806_sd_begin(sl6806_sd_card_t *card);

/*
 * Bring the *controller* up and stop there: pads, module 36, ROM clock 17,
 * the self-clearing reset, and the identification clock. No command is sent
 * and no card is touched.
 *
 * This is the first half of sl6806_sd_begin(), split out for one reason: a
 * walk over 0x400E0000 looking for a functional-clock bit has to redo exactly
 * this after each candidate bit, and then send one CMD0 as the witness.
 * examples/SdWall is that walk.
 *
 * `edge` and `clkcr` default to the mask ROM's values when passed as zero;
 * pass the bootloader's (0x80000000 and SL6806_SD_CLKCR_RUN) to try the one
 * documented difference between the two vendor sequences.
 */
int sl6806_sd_controller_begin(uint32_t edge, uint32_t clkcr);

/*
 * Read one 512-byte block. `lba` is a block number; the driver multiplies by
 * 512 itself for cards that are byte-addressed, which is every card except
 * SDHC and SDXC ([V] ROM 0x4790).
 *
 * `buf` must be 4-byte aligned - the FIFO is read a word at a time and the
 * drain stores words, exactly as ROM 0x4806 does.
 */
int sl6806_sd_read_block(uint32_t lba, void *buf);

/*
 * Read `count` consecutive blocks, one CMD17 each. Not CMD18: multi-block
 * needs the stop command and its own status handling, and single-block reads
 * are the thing worth getting working first. Returns on the first failure,
 * with `*done` (if not NULL) set to the number of blocks that did land.
 */
int sl6806_sd_read_blocks(uint32_t lba, void *buf, uint32_t count,
                          uint32_t *done);

/* Park the pads and drop the clocks, ROM 0x0003D2F0's half of the pair. */
void sl6806_sd_end(void);

/* A short description of an error code, for probes and logging. */
const char *sl6806_sd_strerror(int err);

/*
 * The command layer, exposed because a probe wants it: send `index` with
 * `arg` and wait for the answer. How long an answer to expect comes from the
 * index - the command word carries it (bit 7 for the two 136-bit responses,
 * bit 15 for the one command with none) and the .c file's table is where
 * that lives, so a caller never has to say.
 *
 * Returns an SL6806_SD_ERR_* value, or SL6806_SD_ERR_STATE for an index the
 * table does not carry. The responses stay in the RESP registers and can be
 * read with sl6806_sd_response().
 */
int sl6806_sd_command(unsigned index, uint32_t arg);

/* Read one of the four response registers, 0..3, 0 being bits [31:0]. */
uint32_t sl6806_sd_response(unsigned which);

/* The raw status word, for a probe that wants to print it. */
uint32_t sl6806_sd_status(void);

/*
 * Decode a CSD into a block count, the way ROM 0x3E5E does. Split out
 * because it is pure arithmetic over four words and is therefore the one
 * part of this file that host tests can check against known cards.
 *
 * Returns 0 if the CSD structure version is not one this understands.
 */
uint32_t sl6806_sd_capacity(const uint32_t csd[4]);

#ifdef __cplusplus
}
#endif

#endif /* SL6806_SD_H */
