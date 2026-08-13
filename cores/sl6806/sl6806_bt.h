/*
 * sl6806_bt.h - the SL6806's candidate Bluetooth register block at
 * 0x400E2000.
 *
 * =====================================================================
 *  THIS IS A MAP OF A GUESS, NOT A DRIVER
 * =====================================================================
 * Nothing in the framework programs these registers, and unlike
 * sl6806_lcdc.h this has not been read back on hardware even once. Treat
 * every offset below as "code in the application touches this," not as
 * "this is known to do what its name suggests" - most of these have no name
 * yet, only an offset and a citation. See docs/BLUETOOTH.md for the full
 * writeup and docs/sl6806_re_notes.md for the reproduction trail.
 *
 * WHY 0x400E2000 IS THE CANDIDATE
 *
 * README's own peripheral scan (§7c of the notes) already listed
 * 0x400E2000 among the unidentified blocks - present in the FIRM-wide
 * literal-load count, absent from every peripheral this framework drives.
 * What ties it to Bluetooth specifically: the application's HCI command
 * dispatcher - a 64-entry jump table between 0x00DA0000 and 0x00DA9BCA that
 * logs "-hci cmd0x%x" (file offset 0xC0921) for every opcode it does not
 * yet implement - sits a few hundred bytes from a small cluster of
 * functions (0x00D98B18 .. 0x00D98D80ish) that are the only code anywhere
 * in the 1.8 MB FIRM image that loads the literal 0x400E2000. That address
 * is referenced exactly once in the whole firmware (confirmed with
 * `tools/sl6806-xref dump.bin 0x400E2000`), which is the same kind of
 * single-owner signature that identified the LCD controller.
 *
 * WHAT THE CODE AT 0x00D98C9C DOES
 *
 * It is a module-registration routine: it stores {base=0x400E2000,
 * config_ptr=r0} into a two-word descriptor at SRAM 0x0082B3A8, does a
 * clear-bit31 / delay(10) / set-bit31 reset pulse on +0x228, writes
 * 0xFFFFFFFF to +0x200 during that window, calls 0x00D9A7FC (r0=42),
 * 0x00D9A734 (r0=0) and 0x00D9A768 (r0=0), sets bit 24 of +0x214, and then
 * unpacks roughly a dozen narrow bitfields out of the caller's config struct
 * into +0x10, +0x14, +0x20, +0x44, +0x48, +0x4C, +0x50, +0x54, +0x58, +0x70,
 * +0x78 and +0x7C. That shape - a reset handshake through the same veneer
 * used elsewhere (0x00807214 is already documented as "delay" in the notes,
 * 39 call sites), followed by a config struct fanned out into a dozen small
 * register fields - is what every other peripheral bring-up in this
 * codebase looks like (compare the LCD panel init and the CRU divider
 * setters), which is why this reads as real hardware and not a
 * software-only table.
 *
 * WHAT §14a/§15 SETTLE ABOUT THE OTHER TWO CALLS
 *
 * A later pass through this codebase (docs/sl6806_re_notes.md §14a/§15,
 * chasing the backlight and the ADC) independently named two of the three
 * calls above: 0x00D9A7FC is "the first thing the vendor's module bring-up
 * does" - it starts the PLL at 0x40080008 and spins on its lock bit - and
 * 0x00D9A734 is the routine "the application enables most peripherals
 * through 0x400E0000" with, confirmed dead from a payload today. Its other
 * call sites use 0/1/2/3/4/6; this call's argument, 0, is inside that range.
 * §14a's own host-side read of 0x400E2000 in bootloader mode already came
 * back all zeros, alongside 0x400E0000 and 0x40084000 - the same wall every
 * other peripheral behind that gate hits, not evidence against this being
 * real hardware. 0x00D9A768 is still unexamined.
 *
 * WHAT IS NOT KNOWN
 *
 *   - What any bit in any of these registers actually means. The bitfield
 *     widths (12, 16, 8, 7, 18, 10, 14 bits, several packed two-per-register)
 *     are consistent with radio/link timing parameters, but that is a shape
 *     argument, not a decode.
 *   - Whether this is a full baseband/link-controller register file or a
 *     front end to a companion radio die - "Bluetooth hardware confirmed
 *     present" in the README predates this finding and was not specific
 *     about which.
 *   - What unlocks the PLL and the 0x400E0000 gate without reparenting the
 *     core or USB clock out from under the session - the same open item
 *     §14a/§15 leave for the backlight and ADC's neighbouring wall.
 *     examples/BtProbe reproduces §14a's zero-read from a payload; it is
 *     the sketch to re-run once that unlock work lands.
 *
 * Provenance markers as elsewhere: [V] verified against the dump,
 * [I] inferred, [?] unknown. Nothing here is [V] yet.
 */
#ifndef SL6806_BT_H
#define SL6806_BT_H

#include <stdint.h>

/* [I] Base. The one literal load of this constant anywhere in FIRM is at
 * 0x00D98C9E, inside the module-registration routine described above. */
#define SL6806_BT_BASE          0x400E2000u

#define SL6806_BT_REG(off)      (*(volatile uint32_t *)(SL6806_BT_BASE + (off)))

/* [I] Reset/enable toggle. 0x00D98C64/0x00D98C7A: bit 31 cleared, delayed
 * ~10 units via the shared delay veneer (0x00807214), then set again. */
#define SL6806_BT_RESET         SL6806_BT_REG(0x228)
#define SL6806_BT_RESET_BIT     (1u << 31)

/* [I] Written 0xFFFFFFFF during the reset window above (0x00D98C6C/0x74).
 * Also the register a separate accessor (0x00D98C88) reads bit 1 out of. */
#define SL6806_BT_CTRL          SL6806_BT_REG(0x200)

/* [I] OR'd with bit 24 (0x00D98CBC) immediately before the config struct is
 * unpacked into the offsets below - plausibly "load config" or "start". */
#define SL6806_BT_LOAD          SL6806_BT_REG(0x214)
#define SL6806_BT_LOAD_BIT      (1u << 24)

/* [I] Read (0x00D98BF2), top nibble compared against 4 and used to index a
 * small table - the shape of a status/mode field, not decoded further. */
#define SL6806_BT_STATUS        SL6806_BT_REG(0x218)

/* [?] Also touched by the same accessor cluster; role unknown. */
#define SL6806_BT_UNKNOWN_228_ALT   SL6806_BT_REG(0x228)

/*
 * [I] Config-struct fanout targets, all written by 0x00D98CC4..0x00D98D80
 * from a caller-supplied descriptor. Field widths noted where the bfi
 * instruction pins them down; nothing here has a name yet.
 */
#define SL6806_BT_CFG_10        SL6806_BT_REG(0x10)  /* 12-bit field */
#define SL6806_BT_CFG_14        SL6806_BT_REG(0x14)  /* 12-bit field, shares source word with CFG_10 */
#define SL6806_BT_CFG_20        SL6806_BT_REG(0x20)  /* 8-bit field */
#define SL6806_BT_CFG_44        SL6806_BT_REG(0x44)  /* 8-bit field */
#define SL6806_BT_CFG_48        SL6806_BT_REG(0x48)  /* 7-bit field, constant 30 (0x1E) observed at this call site */
#define SL6806_BT_CFG_4C        SL6806_BT_REG(0x4C)  /* 7-bit field */
#define SL6806_BT_CFG_50        SL6806_BT_REG(0x50)  /* 16-bit field */
#define SL6806_BT_CFG_54        SL6806_BT_REG(0x54)  /* 12-bit field */
#define SL6806_BT_CFG_58        SL6806_BT_REG(0x58)  /* 12-bit field */
#define SL6806_BT_CFG_70        SL6806_BT_REG(0x70)  /* 18-bit field */
#define SL6806_BT_CFG_78        SL6806_BT_REG(0x78)  /* three sub-fields: 8, 10, 14 bits */
#define SL6806_BT_CFG_7C        SL6806_BT_REG(0x7C)  /* four packed byte fields */

#endif /* SL6806_BT_H */
