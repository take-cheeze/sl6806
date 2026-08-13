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
 * 0xFFFFFFFF to +0x200 during that window, calls three FIRM-resident
 * routines (0x00D9A7FC, 0x00D9A734, 0x00D9A768 - not yet examined), sets
 * bit 24 of +0x214, and then unpacks roughly a dozen narrow bitfields out of
 * the caller's config struct into +0x10, +0x14, +0x20, +0x44, +0x48, +0x4C,
 * +0x50, +0x54, +0x58, +0x70, +0x78 and +0x7C. That shape - a reset
 * handshake through the same veneer used elsewhere (0x00807214 is already
 * documented as "delay" in the notes, 39 call sites), followed by a config
 * struct fanned out into a dozen small register fields - is what every
 * other peripheral bring-up in this codebase looks like (compare the LCD
 * panel init and the CRU divider setters), which is why this reads as real
 * hardware and not a software-only table.
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
 *   - Whether any of this is readable/writable at all in payload mode. LCD
 *     and GPIO both needed SRAM-resident vendor code that only exists after
 *     the application has booted; this may be the same. examples/BtProbe
 *     is the read-only test for that.
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
