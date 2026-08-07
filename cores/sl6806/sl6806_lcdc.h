/*
 * sl6806_lcdc.h - the SL6806 LCD controller at 0x400D9000.
 *
 * =====================================================================
 *  THE PANEL IS ON A QSPI BUS, AND THIS DRIVES IT
 * =====================================================================
 * This used to be a register map with a note saying a driver could not be
 * written from it. It can now, and sl6806_lcdc.c is that driver. What
 * changed is one observation:
 *
 *     The bootloader's lcd_write_cmd sends four bytes for every command:
 *     0x02, 0x00, cmd, 0x00.
 *
 * That is not an SL6806 invention, it is the standard QSPI display command
 * frame - a one-lane 0x02 write opcode, a 24-bit address whose middle byte
 * is the DCS command, and pixel data streamed afterwards on four lanes
 * behind a 0x32 opcode. Every unexplained field in the old notes falls out
 * of it at once:
 *
 *   - the panel descriptor's +0x0C = 0x32 and +0x0D = 2 are the quad-lane
 *     and single-lane write opcodes, not a magic number and a device id;
 *   - the command list's `b` field, "a descriptor byte shifted left 8", is
 *     the DCS command inside that 24-bit address - so it IS the opcode, and
 *     the two window records carry CASET and RASET because the fields they
 *     read, +0x0D and +0x0E of the bootloader's descriptor, are CASET and
 *     RASET. The old note could not settle this because it compared them
 *     against the *application's* descriptor, which has an extra four bytes
 *     at the front and so numbers everything four higher;
 *   - the `a` field is which opcode to use: 0x02 for the window records,
 *     0x32 for the pixel record.
 *
 * HOW THE CONTROLLER WAS FOUND, AND WHERE THE DRIVER CAME FROM
 *
 * The application drives the LCD from SRAM-resident code that is not in the
 * flash image. The HLKJ bootloader drives the same peripheral and IS stored
 * verbatim (file 0x60 -> 0x0081FC00), so it disassembles, and it identifies
 * itself: `HAL_lcdc_module_init` at 0x0082E171 is logged by the routine at
 * 0x00829A28, whose first act is to cache 0x400D9000 in a driver struct.
 *
 * Everything sl6806_lcdc.c does is a transcription of that bootloader code:
 *
 *     0x008218D4  send one byte of a command frame
 *     0x00821940  send data bytes out of the FIFO
 *     0x00821B88  assert CS / 0x00821B9C release CS
 *     0x00821BB2  lcd_write_cmd(last, opcode, cmd)
 *     0x00821C00  lcd_write_data(last, byte)
 *     0x00829A28  HAL_lcdc_module_init(config)
 *     0x008299C4  soft reset
 *     0x00829B30  lane map
 *     0x00827FAC  clock gating and module init
 *     0x00828068  pad setup
 *
 * The application's own copies at 0x00D3E944 / 0x00D3E9DC / 0x00D3EAC0 are
 * the same code with the P20's values, and those are the values the variant
 * uses - see variants/p20_player/lcdc.c. Where the two disagree it is worth
 * knowing: the bootloader clocks the panel at 0x310 and the application at
 * 0x910, and the bootloader's pads are bank 4 while the P20's are bank 1.
 *
 * WHAT IS NOT DONE HERE
 *
 * The vendor streams pixels with DMA and an interrupt. This driver polls and
 * pushes 16 bytes at a time, which is what the FIFO holds (the bootloader's
 * own short-write helper at 0x00821AA8 refuses anything longer). That is
 * roughly 8900 transfers for a full 240x296 frame. It is correct, it needs
 * no DMA driver, and it is the obvious thing to make faster later.
 *
 * The command-list/DMA path - descriptor to +0x88, then +0x80 and +0x84 - is
 * mapped below but not used. Its record format is documented at the bottom
 * of this file, because that is where the QSPI reading came from.
 *
 * Markers: [V] read directly out of the bootloader or the mask ROM,
 * [I] inferred, [?] not understood. Nothing here has been checked against a
 * logic analyser.
 */
#ifndef SL6806_LCDC_H
#define SL6806_LCDC_H

#include <stdint.h>
#include "gfx/Panel.h"

#ifdef __cplusplus
extern "C" {
#endif

/* [V] Base address, from HAL_lcdc_module_init at 0x00829A28. */
#define SL6806_LCDC_BASE       0x400D9000u

/* [V] Interrupt line, enabled by both the bootloader and the application
 * right after gating the controller on. Unused: this driver polls. */
#define SL6806_LCDC_IRQ        74

/* ------------------------------------------------------- register map */

/* [V] +0x00  Four 4-bit fields at [15:12] [11:8] [7:4] [3:0], loaded from
 * config bytes 1..4. All four are 9 in the LCD case. [?] What they select is
 * not known; bus timing is the obvious guess. */
#define SL6806_LCDC_CFG0       0x00

/* [V] +0x04  Single-bit flags at 1, 2, 3, 5 and 6, from config bytes 9,
 * 0x0B, 0x0C, 0x0D and 0x0A respectively. [?] */
#define SL6806_LCDC_CFG1       0x04

/* [V] +0x08  Control and reset.
 *   bit 0     set from config byte 0 when the interface type is 0 or 1
 *   bit 3     set for interface type 2, cleared otherwise
 *   bit 4     [V] command-list mode. The vendor sets it before every command
 *             list (0x00829A00). MEASURED on hardware: set it and the
 *             register-driven transfer path never reports completion at all,
 *             so it really does switch the controller between the two ways of
 *             being fed. This driver leaves it clear.
 *   bit 30    second reset: set, wait, clear
 *   bit 31    first reset:  set, wait, clear
 * The reset routine at 0x008299C4 does bit 31 then bit 30, one tick apart. */
#define SL6806_LCDC_CTRL       0x08
#define SL6806_LCDC_CTRL_RESET0  (1u << 31)   /* [V] */
#define SL6806_LCDC_CTRL_RESET1  (1u << 30)   /* [V] */
#define SL6806_LCDC_CTRL_CMDLIST (1u << 4)    /* [V] */

/* [V] +0x10  Interrupt enable. Every polled transfer clears bits [16:8],
 * 31, 22 and 19 first, which is how the vendor keeps the IRQ quiet while it
 * waits on the status bit itself. */
#define SL6806_LCDC_IE         0x10

/* [V] +0x14  Status and interrupt flags, write-1-to-clear.
 *   bit 31        transfer complete; the polled path waits for it, writes it
 *                 back, and waits for it to read as 0 again
 *   bits [30:29]  busy; every transfer waits for these to clear first
 *   bits [30:28]  the DMA path's busy mask (0x00829A14) */
#define SL6806_LCDC_STATUS     0x14
#define SL6806_LCDC_STAT_DONE  (1u << 31)     /* [V] */
#define SL6806_LCDC_STAT_BUSY  0x60000000u    /* [V] */
#define SL6806_LCDC_STAT_DMA_BUSY 0x70000000u /* [I] */

/*
 * [V] +0x20  Transfer control. The busiest register on the part.
 *
 *   bit 0         start
 *   bit 1         set for FIFO data writes, cleared for reads
 *   bits [3:2]    source: 1 = the single word in +0x24, 2 = the FIFO,
 *                 3 = both (the vendor's combined command+data transfer)
 *   bits [9:8]    length-1 of the +0x2C command field
 *   bits [11:10]  length-1 of the +0x24 command field
 *   bit 17        [I] emit the command frame. Set for every command except
 *                 RAMWR, cleared for RAMWR - which is exactly "start a new
 *                 QSPI transaction, or continue the pixel stream".
 *   bit 18        [I] hold CS. Set before a command frame; cleared before
 *                 the final byte of the last transfer of a transaction.
 *                 This is what the vendor's `last` argument controls.
 *   bits [21:20]  pixel format. Set from the config at init and then zeroed
 *                 by every byte-wise transfer, so it is 0 during real
 *                 traffic. [?]
 */
#define SL6806_LCDC_XFER       0x20
#define SL6806_LCDC_XFER_GO    (1u << 0)      /* [V] */
#define SL6806_LCDC_XFER_WRITE (1u << 1)      /* [V] */
#define SL6806_LCDC_XFER_FRAME (1u << 17)     /* [I] */
#define SL6806_LCDC_XFER_CS    (1u << 18)     /* [I] */

/* [V] +0x24, +0x2C  The two command words. In a QSPI frame +0x24 holds the
 * lane opcode (0x02 or 0x32) and +0x2C the DCS command. */
#define SL6806_LCDC_CMD0       0x24
#define SL6806_LCDC_CMD1       0x2C

/* [V] +0x28  bits [15:0] = length - 1, in bytes. */
#define SL6806_LCDC_LENGTH     0x28

/* [V] +0x30 write FIFO, +0x34 read FIFO. Word wide, and the byte at the
 * bottom of a word goes out first - which is what makes the byte-swapped
 * coordinate words in the command list come out as CASET wants them. */
#define SL6806_LCDC_FIFO_WR    0x30
#define SL6806_LCDC_FIFO_RD    0x34

/* [V] +0x40, +0x44  Sixteen 4-bit lane assignments, packed low nibble
 * first. The vendor builds them from a 16-byte table; see the variant. */
#define SL6806_LCDC_MAP0       0x40
#define SL6806_LCDC_MAP1       0x44

/* [V] +0x80 / +0x84 / +0x88  The command-list path: store the list address
 * to +0x88, set bit 0 of +0x80, set bit 0 of +0x84. Not used by this
 * driver. */
#define SL6806_LCDC_START      0x80
#define SL6806_LCDC_TRIGGER    0x84
#define SL6806_LCDC_CMDLIST    0x88

/* -------------------------------------------------------- the driver */

/*
 * Everything about the LCDC that is board-specific. A variant fills one in
 * and calls sl6806_lcdc_begin(); nothing else in the framework needs to know
 * the controller exists.
 */
typedef struct {
    /* [V] Config byte 5 of HAL_lcdc_module_init, which is the panel
     * descriptor's colour mode: 1 = RGB565, 2 = RGB888. */
    uint8_t  color_format;

    /* [V] QSPI write opcodes, from the panel descriptor. 0x02 addresses the
     * panel on one lane and carries commands and their parameters; 0x32
     * addresses it on four and carries pixels. */
    uint8_t  cmd_opcode;
    uint8_t  pixel_opcode;

    /*
     * Send each RGB565 pixel most significant byte first.
     *
     * A word pushed into the FIFO goes out low byte first, and the
     * framebuffer stores RGB565 little-endian, so without this the panel
     * receives every pixel byte-swapped.
     *
     * [V] The vendor swaps. Its single-pixel write at 0x0082837C computes
     * `(colour >> 8) | (colour << 8)` and pushes that, so most significant
     * byte first is what the panel wants. Kept as a field anyway because it
     * is one bit, it is board-specific in principle, and a wrong guess here
     * is the one failure that produces a picture rather than nothing.
     */
    uint8_t  swap_pixel_bytes;

    /* [V] CRU 0x4008010C field 0xF10. The application writes 0x910 for this
     * panel; the bootloader writes 0x310 and runs it slower. */
    uint32_t modclk;

    /* [V] The lane map, +0x40 and +0x44. */
    uint32_t map0, map1;

    /* [V] Pad ids to configure before use, in the vendor's order - the LCD
     * data and clock pads, and usually the reset line too. See
     * sl6806_padctl.h for the encoding. */
    const uint32_t *pads;
    uint8_t         npads;

    /* [V] The panel reset line, as a pad id, or 0 if the board does not have
     * one. Exposed through the bus's reset() so the panel's own reset pulse
     * shape stays in the panel description. */
    uint32_t reset_pad;

    /*
     * THE THREE THINGS THAT WERE INFERRED, MADE SWITCHABLE
     *
     * Everything else in this driver was read out of the vendor's code. These
     * three were deduced from where the vendor sets and clears bits, which is
     * good evidence about *what changes* and no evidence at all about
     * polarity or necessity. On a board where the panel stays dark and the
     * bus cannot be observed - the pad input buffers are off for a pad in an
     * alternate function, so software cannot watch the lines - trying the
     * eight combinations against the actual glass is faster and more honest
     * than arguing about which reading is more plausible.
     *
     * All three default to 0, which is the vendor's behaviour as transcribed.
     * examples/LcdProbe walks them.
     */

    /* Invert bit 17. As transcribed it is set for every command and cleared
     * for RAMWR, read as "emit a new frame". If that reading is backwards,
     * every command has been going out without its QSPI header and the panel
     * has been ignoring all of it - which looks exactly like this. */
    uint8_t quirk_invert_frame;

    /* Invert bit 18, read as "hold chip select". If backwards, chip select is
     * released during each frame and asserted between them, and again the
     * panel sees nothing it recognises. */
    uint8_t quirk_invert_cs;

    /* Set CTRL bit 4 at bring-up. RESOLVED - leave this 0. Measured: with it
     * set, no polled transfer ever completes, which is what identifies bit 4
     * as command-list mode rather than an output enable. Kept only so the
     * experiment can be repeated. */
    uint8_t quirk_ctrl_bit4;

    /*
     * Clear status bits 27 and 22 after every transfer, not just bit 31.
     *
     * The vendor's interrupt handler at 0x00827D6C clears three flags - 31,
     * 27 and 22 - each by writing it back and spinning until it reads 0. That
     * handler is not only for the DMA path: 0x00827FAC enables interrupt 74
     * *before* the panel init sequence runs, so on the vendor it fires
     * throughout, and bit 27 is never left standing.
     *
     * This driver polls, never enables the interrupt, and clears only bit 31.
     * So after its first transfer, status bit 27 is latched and stays latched
     * - measured 0x08020000 on hardware. Whether a pending completion blocks
     * anything is unknown, but it is a state the vendor's controller is never
     * in, which makes it worth one experiment.
     */
    uint8_t quirk_clear_status;
} sl6806_lcdc_config_t;

/*
 * Bring the controller up and register its bus.
 *
 * Gates the clock, initialises the module, configures the pads and the lane
 * map, soft-resets the controller, and then calls
 * sl6806_lcd_bus_register(), so that a successful return means Screen and
 * the whole display stack are live.
 *
 * Returns 0, or -1 if `cfg` is missing or the controller never reports a
 * transfer complete. It does not spin forever waiting for the status bit:
 * this code can run inside the boot ROM's USB handler in RUN_MODE=poll,
 * where an unbounded wait takes the device off the bus until it is
 * unplugged.
 */
int sl6806_lcdc_begin(const sl6806_lcdc_config_t *cfg);

/* The LCDC bus, or NULL before a successful begin(). */
const sl6806_lcd_bus_t *sl6806_lcdc_bus(void);

/*
 * Set once the controller stops answering - the polled wait timed out and
 * the driver gave up rather than hang. When this is non-zero the picture is
 * gone and the reason is a hardware handshake, not the drawing code.
 */
extern uint8_t sl6806_lcdc_timed_out;

/* The configuration begin() was last called with, or NULL. A bring-up probe
 * copies this, changes one field and calls begin() again, which is how you
 * bisect the values that were inferred rather than read. */
const sl6806_lcdc_config_t *sl6806_lcdc_config(void);

/*
 * Counters, for working out what a dark screen means.
 *
 * The useful one is `last_polls`. A transfer that reports complete on the
 * very first poll never happened - either the status register is not the
 * status register, or it is stuck. A transfer that takes a plausible handful
 * of polls did happen, which moves the fault past the controller and onto
 * the pads, the panel or the backlight.
 */
typedef struct {
    uint32_t transfers;    /* transfers that completed                     */
    uint32_t timeouts;     /* transfers that never reported complete       */
    uint32_t last_polls;   /* polls the last completion took, 0 = instant  */
    uint32_t min_polls;    /* over all transfers so far                    */
    uint32_t max_polls;
    uint32_t busy_waits;   /* transfers that had to wait for the busy bits */
} sl6806_lcdc_stats_t;

const sl6806_lcdc_stats_t *sl6806_lcdc_stats(void);
void sl6806_lcdc_stats_reset(void);

/*
 * Bring-up diagnostic: watch a register while transfers run.
 *
 * The question a dark screen cannot answer on its own is whether the
 * controller is driving the pins at all. Point this at a GPIO bank's input
 * register and every poll of the completion wait samples it, so after a
 * burst of transfers `watch_ones` holds every bit that was ever seen high
 * and `watch_zeros` every bit ever seen low. A pad that appears in both is
 * moving. A pad in neither direction is not.
 *
 * Sampling from the CPU is far slower than the bus, so this cannot show a
 * waveform - only whether a line ever changed, which is the question.
 *
 * sl6806_lcdc_watch(addr) starts a watch and clears the accumulators;
 * sl6806_lcdc_watch(0) stops it and leaves them intact to be read. It costs
 * one register read per poll while running.
 */
extern uint32_t sl6806_lcdc_watch_addr;
extern uint32_t sl6806_lcdc_watch_ones;
extern uint32_t sl6806_lcdc_watch_zeros;
extern uint32_t sl6806_lcdc_watch_samples;
void sl6806_lcdc_watch(uint32_t addr);

/*
 * Read from the panel: `n` bytes behind a QSPI read frame.
 *
 * The vendor has this path - 0x00821C16, the read twin of the write at
 * 0x00821CC8 - but never uses it on the panel, so the read opcode is not
 * recorded anywhere. 0x03 is the one-lane read opcode in the QSPI display
 * convention that 0x02 and 0x32 come from; `opcode` is a parameter so a
 * probe can try others.
 *
 * This exists because it is the only way to prove from software that the
 * panel is listening. Returns 0, or -1 on a timeout or a bad argument.
 */
int sl6806_lcdc_read(uint8_t opcode, uint8_t cmd, unsigned addr_bytes,
                     uint8_t *dest, unsigned n);

/*
 * THE COMMAND LIST - not used, recorded because it is the evidence
 *
 * The DMA path takes a list built in SRAM (0x00829908 in the application,
 * 0x0082ED9C in the bootloader) by the builder at 0x00827E18, whose
 * signature is (x0, x1, y0, y1) with the ends exclusive. Records are
 *
 *     <tag> <length-1> <opcode> <cmd << 8> [inline payload] 0xABAB0005
 *
 * and the list ends with 0xFFFFFFFC. A windowed pixel write is three of
 * them: CASET, RASET, then the pixel record, whose length field is the pixel
 * count minus one and which has no payload because the pixels arrive by DMA
 * through the FIFO.
 *
 * The tag is 0xCDCD_mm_ll. Reading the builder's three branches together,
 * the middle byte is (format << 3) | 2 for a pixel record - 0x0A, 0x12, 0x1A
 * for formats 1, 2 and 3 - with bit 7 marking the long form used above
 * 0x10000 elements, and 0x62 for the window records, whose payload is raw
 * bytes rather than pixels. The low byte is 3 for a record that carries a
 * command and 2 for the continuation record of a long transfer. [I]
 *
 * The coordinates are byte-swapped into the record - bswap16(x0) |
 * bswap16(x1-1) << 16 - which is CASET's own big-endian parameter order, and
 * is what shows that the FIFO emits a word low byte first.
 */

#ifdef __cplusplus
}
#endif

#endif /* SL6806_LCDC_H */
