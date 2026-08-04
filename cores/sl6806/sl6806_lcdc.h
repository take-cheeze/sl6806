/*
 * sl6806_lcdc.h - the SL6806 LCD controller at 0x400D9000.
 *
 * =====================================================================
 *  THIS IS A MAP, NOT A DRIVER
 * =====================================================================
 * Nothing here is executed. It is the register map you need in order to
 * write the sl6806_lcd_bus_t that gfx/Panel.h is waiting for, written down
 * at the point where reading it stopped rather than where a driver would
 * start. Do not treat any offset as verified beyond what its marker says.
 *
 * HOW THE CONTROLLER WAS FOUND
 *
 * The application drives the LCD from SRAM-resident code that is not in the
 * flash image, which is where the previous analysis stopped. The way around
 * that turned out to be the HLKJ bootloader: it initialises the same
 * peripheral, and unlike the application it is stored verbatim in flash
 * (file 0x60 -> 0x0081FC00), so it disassembles.
 *
 * The identification is not circumstantial - the bootloader logs the
 * function's own name. `HAL_lcdc_module_init` at 0x0082E171 is printed by
 * the routine at 0x00829A28, and that routine's first act is to cache
 * 0x400D9000 in a driver struct at SRAM 0x0082EE80. Every other function
 * below reaches the controller through that cached pointer, which is why the
 * base appears as a literal only twice in the whole image.
 *
 * That the bootloader path is the LCD path is corroborated three ways: it
 * clock-gates through exactly the CRU registers the application's LCD code
 * uses (sl6806_cru.h), it enables the same interrupt (74), and it builds its
 * command list with the same magic words the application's
 * lcdc_set_descriptor uses.
 *
 * Markers: [V] read directly out of the bootloader, [I] inferred, [?] not
 * understood.
 */
#ifndef SL6806_LCDC_H
#define SL6806_LCDC_H

#include <stdint.h>

/* [V] Base address, from HAL_lcdc_module_init at 0x00829A28. */
#define SL6806_LCDC_BASE       0x400D9000u

/* [V] The bootloader's driver state; word 0 is the base, word 1 is the
 * config struct it was initialised with. Recorded because it is how you find
 * the rest of the driver in a disassembly, not because it is useful at run
 * time - the application keeps its own copy elsewhere. */
#define SL6806_LCDC_BL_STATE   0x0082EE80u

/* [V] Interrupt line, enabled by both the bootloader and the application
 * right after gating the controller on. */
#define SL6806_LCDC_IRQ        74

/*
 * Register map, offsets from SL6806_LCDC_BASE.
 */

/* [V] +0x00  Four 4-bit fields at bits [15:12], [11:8], [7:4], [3:0], loaded
 * from bytes 1..4 of the config struct. All four are 9 in the LCD case. [?]
 * What they select is not known - bus timing is the obvious guess. */
#define SL6806_LCDC_CFG0       0x00

/* [V] +0x04  Single-bit flags at 1, 2, 3, 5 and 6, from config bytes
 * 9, 0x0B, 0x0C, 0x0D and 0x0A respectively. [?] */
#define SL6806_LCDC_CFG1       0x04

/* [V] +0x08  Control and reset.
 *   bit 0     set from config byte 0 when the interface type is 0 or 1
 *   bit 3     set for interface type 2, cleared otherwise
 *   bit 4     a standalone setter at 0x00829A00 writes this from its argument
 *   bit 30    second reset: set, wait 1, clear
 *   bit 31    first reset:  set, wait 1, clear
 * The reset routine at 0x008299C4 does bit 31 then bit 30, one tick apart. */
#define SL6806_LCDC_CTRL       0x08
#define SL6806_LCDC_CTRL_RESET0  (1u << 31)   /* [V] */
#define SL6806_LCDC_CTRL_RESET1  (1u << 30)   /* [V] */

/* [V] +0x10  Status / interrupt mask. Readable on its own (0x00829C84).
 * The start routine sets bit 22, then clears bits [16:8], bit 31 and
 * bit 19. [?] */
#define SL6806_LCDC_STATUS     0x10

/* [V] +0x14  Interrupt flags, write-1-to-clear: 0x00829C9C writes the mask
 * and then spins until those bits read back as 0. A separate wait at
 * 0x00829A14 spins while bits [30:28] are set, so 0x70000000 is the
 * transfer-in-progress mask. [I] */
#define SL6806_LCDC_IRQFLAGS   0x14
#define SL6806_LCDC_BUSY_MASK  0x70000000u    /* [I] */

/* [V] +0x20  Geometry and format.
 *   bits [3:2]    written 2 when both counts below are zero
 *   bits [9:8]    (bytes per something) - 1
 *   bits [11:10]  (another count) - 1
 *   bits [15:12]  from config byte 6
 *   bits [21:20]  from config byte 5; also a standalone setter at 0x00829B1C
 *   bit 4         from config byte 8
 *   bits 16, 17   always cleared at init
 */
#define SL6806_LCDC_FORMAT     0x20

/* [V] +0x24, +0x2C  Transfer parameters, written from the caller's
 * arguments by the windowing routine at 0x00829BC8. [?] */
#define SL6806_LCDC_PARAM0     0x24
#define SL6806_LCDC_PARAM1     0x2C

/* [V] +0x28  bits [15:0] = length - 1. */
#define SL6806_LCDC_LENGTH     0x28

/* [V] +0x40, +0x44  A pair of words each packed from eight 4-bit values
 * taken from a 16-byte table (0x00829B30). [I] a per-lane command or pin
 * mapping; the vendor passes one fixed table. */
#define SL6806_LCDC_MAP0       0x40
#define SL6806_LCDC_MAP1       0x44

/* [V] +0x80  Start. The routine at 0x00829968 sets bit 0, ORs its argument
 * in at bits 2, 6 and 7, clears bits [23:8], and sets bits [11:8] to 0xF. */
#define SL6806_LCDC_START      0x80
#define SL6806_LCDC_START_GO   (1u << 0)      /* [V] */

/* [V] +0x84  bit 0, set last by the same routine - the actual trigger. */
#define SL6806_LCDC_TRIGGER    0x84
#define SL6806_LCDC_TRIGGER_GO (1u << 0)      /* [V] */

/* [V] +0x88  Command-list address. 0x00829B0C writes the descriptor pointer
 * here and nothing else.
 *
 * THIS IS THE ANSWER TO THE OLD OPEN QUESTION: "the code that hands the
 * descriptor to the LCDC and starts it" is a store to +0x88 followed by
 * +0x80 and +0x84. */
#define SL6806_LCDC_CMDLIST    0x88

/*
 * THE COMMAND LIST
 *
 * The list is built in SRAM - at 0x00829908 in the application, at
 * 0x0082ED9C in the bootloader - by the builder at 0x00827E18 (bootloader) /
 * 0x00D3E728 (application), whose signature is (x0, x1, y0, y1).
 *
 * [V] It is a sequence of variable-length records, each introduced by a tag
 * word, and terminated by 0xFFFFFFFC. The one the bootloader emits for a
 * windowed pixel write is:
 *
 *   +0x00  CDCD6203     tag
 *   +0x04  3            payload length - 1  (a window is 4 bytes)
 *   +0x08  desc[0x09]
 *   +0x0C  desc[0x0D] << 8
 *   +0x10  column window: bswap16(x0) | bswap16(x1-1) << 16
 *   +0x14  ABAB0005     end of record
 *   +0x18  CDCD6203     tag
 *   +0x1C  3
 *   +0x20  desc[0x09]
 *   +0x24  desc[0x0E] << 8
 *   +0x28  row window: bswap16(y0) | bswap16(y1-1) << 16
 *   +0x2C  ABAB0005
 *   +0x30  CDCD0A03     tag, chosen by interface type - see below
 *   +0x34  pixel count - 1
 *   +0x38  0x32         also byte +0x0C of the panel descriptor
 *   +0x3C  desc[0x0B] << 8
 *   +0x40  FFFFFFFC     end of list
 *
 * So the shape is `<tag> <len-1> <a> <b> [inline payload] <ABAB0005>`, with
 * the byte-swapping being exactly what CASET and RASET want. That the length
 * word is 3 for a 4-byte window and count-1 for the pixel record is what
 * pins it down as a length.
 *
 * [I] The tags are structured. The pixel-write tag is
 *
 *     0xCDCD_(0x0A + 8*(type-1))_03      transfers up to 0x10000 elements
 *     0xCDCD_(0x8A + 8*(type-1))_03      the long form
 *     0xCDCD_(0x08 + 8*(type-1))_02      the long form's second record
 *
 * for interface type 1, 2 or 3 - so bits of the middle byte select the
 * interface and bit 7 selects the long form, and the low byte is a small
 * opcode (2, 3, 5). Transfers over 0x10000 elements take the long branch and
 * emit a list ending at +0x50, so that is the per-record limit.
 *
 * [?] What is NOT decoded: what the tag's low byte actually means, and the
 * `a`/`b` fields. `b` is a descriptor byte shifted left 8 and differs between
 * the two window records (desc[0x0D] vs desc[0x0E]), which is the shape a
 * command opcode would have - but the panel descriptor the *application*
 * uses keeps CASET/RASET at +0x11/+0x12, not +0x0D/+0x0E, so either the
 * bootloader's descriptor has a different layout or these are not command
 * bytes. Do not guess: the two readings differ by whether the controller or
 * the driver emits the DCS opcode.
 *
 * WHAT IS STILL NEEDED FOR A WORKING BUS
 *   1. Resolve the `b` field above, which is the difference between "write a
 *      window" and "write a window preceded by the wrong command".
 *   2. The config struct passed to HAL_lcdc_module_init: 20 bytes, byte 0 =
 *      interface type, bytes 1..4 = 9, a 0x300 halfword at +6. Only partly
 *      understood.
 *   3. The clock setup in sl6806_cru.h, which is understood but untested.
 * None of that can be finished from a flash dump alone - it needs a device
 * and a logic analyser, or the mask ROM (docs/DUMPING.md).
 */

#endif /* SL6806_LCDC_H */
