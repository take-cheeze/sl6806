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
 * The descriptor is a list of 32-bit word pairs built in SRAM - at
 * 0x00829908 in the application, on the stack in the bootloader - by the
 * builder at 0x00827E?? (bootloader) / 0x00D3E728 (application). Layout as
 * read from the bootloader:
 *
 *   +0x00  opcode              +0x04  3
 *   +0x08  opcode              +0x0C  operand
 *   +0x10  column window, coordinates byte-swapped into big-endian pairs
 *   +0x14  opcode              +0x18  opcode
 *   +0x1C  3                   +0x20  operand
 *   +0x24  MADCTL value << 8   (config byte 0x0E)
 *   +0x28  row window, same packing
 *   +0x2C  opcode
 *   +0x30  opcode chosen by interface type (1, 2 or 3)
 *   +0x34  pixel count - 1
 *   +0x38  0x32                (also byte +0x0C of the panel descriptor)
 *   +0x3C  config byte 0x0B << 8
 *   +0x40  0xFFFFFFFC
 *
 * Transfers longer than 0x10000 pixels take a second branch that emits a
 * longer list ending at +0x50, so the controller has a 64 K element limit
 * per entry.
 *
 * [?] The opcodes themselves are not decoded. The observed values are
 * 0xABAB0005 and 0xCDCDxx03 / 0xCDCDxx02 with xx in
 * {0x0A, 0x12, 0x8A, 0x92, 0x9A, 0x62, 0x08, 0x18}; the two 16-bit halves
 * are clearly a tag and a small field, but which is which, and whether the
 * low byte is a length or a register index, is guesswork until someone can
 * watch the bus.
 *
 * WHAT IS STILL NEEDED FOR A WORKING BUS
 *   1. Decode the opcodes above.
 *   2. The config struct the vendor passes to HAL_lcdc_module_init: 20
 *      bytes, byte 0 = interface type, bytes 1..4 = 9, and a 0x300 halfword
 *      at +6. Its meaning per byte is only partly known.
 *   3. The clock setup in sl6806_cru.h, which is understood but untested.
 * None of that can be finished from a flash dump alone - it needs a device
 * and a logic analyser, or the mask ROM (docs/DUMPING.md).
 */

#endif /* SL6806_LCDC_H */
