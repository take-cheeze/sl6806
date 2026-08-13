/*
 * sl6806_bt.h - the SL6806's candidate Bluetooth register window at
 * 0x400E2000, and the gate in front of it.
 *
 * =====================================================================
 *  STILL A MAP OF A GUESS - BUT THE GATE IS NO LONGER A GUESS
 * =====================================================================
 * Nothing in this window has been read on hardware with anything switched on.
 * What has changed since the first version of this file is that the two
 * things it listed as unknown - "what unlocks it" and "which bit in
 * 0x400E0000 it needs" - are now both read out of the dump, exactly, and are
 * things a payload can perform:
 *
 *     the block's functional clock is 0x400E0000 bit 0
 *     its reset is 0x400E0008 bit 0
 *
 * from 0x00D98CAE and 0x00D98CB4, the two calls immediately after the module
 * registration stores its base. docs/BLUETOOTH.md used to say "anything at
 * 0x400E2000 most likely needs its own bit there first" and leave the bit
 * open; it is bit 0, and sl6806_bt_begin() below sets it.
 *
 * Still true, and worth keeping in front of anyone reading this: no bit in
 * this window has been correlated with any Bluetooth concept. What follows is
 * "the vendor's code touches this, in this order, with these masks".
 *
 * =====================================================================
 *  WHY 0x400E2000 IS THE CANDIDATE
 * =====================================================================
 * Unchanged, and still the strongest part of the case. The application's HCI
 * command dispatcher - a 64-entry jump table between 0x00DA0000 and
 * 0x00DA9BCA that logs "-hci cmd0x%x" for every opcode it does not implement
 * - sits a few hundred bytes from the only code in the whole 1.8 MB FIRM
 * image that loads the literal 0x400E2000, and that code is a module
 * registration with the same reset-then-config-fanout shape as every other
 * peripheral bring-up in this codebase.
 *
 * One thing that case got wrong in the telling. "0x400E2000 is loaded exactly
 * once in the entire application" is true of that one literal and gave the
 * impression of a lonely, barely-used address. Scanning the neighbourhood for
 * every literal in the 0x400E2000..0x400E2FFF range instead turns up **about
 * ninety load sites across some thirty distinct registers**, spread over
 * three clusters with their own bring-up routines. This is a busy window, not
 * a stub, and the register list below is the whole of it.
 *
 * =====================================================================
 *  AND A SECOND CORROBORATION: FREE-RUNNING COUNTERS
 * =====================================================================
 * 0x00D9A8F4 is a four-way accessor over +0x228, +0x22C, +0x230 and +0x234,
 * and it masks the values it returns to **25, 30, 30 and 30 bits**. Odd
 * widths like that on registers a driver only ever reads are counters, and a
 * ~28-bit free-running counter is what a Bluetooth link controller keeps its
 * native clock in. That is still a shape argument - but it is a different
 * shape argument from the one the address was found by, which is worth
 * something.
 *
 * =====================================================================
 *  [M] MEASURED 2026-08-13: THE GATE OPENS, THE BLOCK DOES NOT RUN
 * =====================================================================
 * examples/BtProbe on a P20 Player. sl6806_bt_begin() ran, the device stayed
 * on the USB bus, and:
 *
 *     0x40080008 = 0xD0010C04   written 0xC0000C04 - bit 28 LOCK came back
 *     0x400E0000 = 0x00000021   bits 5 and 0, both HELD
 *     0x400E0008 = 0x00000001
 *
 * **The PLL locks from a payload**, which 14a could not get it to do in
 * bootloader mode, and 0x4008011C = 0x31 does not reparent the core or USB
 * clock - the write 14a flagged as plausibly safe but untried. Four registers
 * in the window came up from zero:
 *
 *     +0x078 = 0x0C019A14       +0x1B4 = 0x00003301
 *     +0x07C = 0x06060502       +0x21C = 0x00400000
 *
 * and they persist across a re-upload, so a later sketch's "cold" pass will
 * find them already up unless the device has been unplugged.
 *
 * **The four counters stayed at zero**, all eight samples. The window is
 * powered and partly readable; the link controller is not running. Nothing
 * here has configured it, so that is the expected result rather than a
 * negative - but it does mean nothing below has been confirmed to *mean*
 * anything yet.
 *
 * Provenance markers as elsewhere: [V] verified against the dump,
 * [M] measured on hardware, [I] inferred, [?] unknown.
 */
#ifndef SL6806_BT_H
#define SL6806_BT_H

#include <stdint.h>
#include "sl6806_mmio.h"
#include "sl6806_module.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* The gate                                                            */
/* ------------------------------------------------------------------ */

/*
 * [V] The shared bring-up, 0x00D9A7FC, decoded in full. Every peripheral in
 * the 0x400Exxxx group calls it first, and it is guarded so it runs once:
 *
 *     if (module_clock_is_enabled(46))   return;     ROM 0x1E54
 *     power_request();                               0x00D9A7AC, below
 *     0x40080008 = 0xC0000C04;                       the PLL
 *     while (!(0x40080008 & (1 << 28))) ;            spin on lock
 *     0x40080008 |= 0x10000;                         output enable
 *     module_clock_enable(46);
 *     delay(10);
 *     0x4008011C = 0x31;
 *     delay(10);
 *     periph_enable(5);                              0x400E0000 bit 5
 *
 * Three things fall out of that, none of which was in the earlier writeup:
 *
 *   1. **Module 46 is not "the camera front end".** It is the gate for this
 *      whole address group - which is precisely what 7n measured when
 *      sl6806_module_enable(46) made 0x400E0000 start holding bits, and it
 *      explains why one module id appeared to belong to a peripheral it
 *      shares nothing else with.
 *   2. **0x400E0000 bit 5 is set by the shared path**, before any individual
 *      peripheral's bit. Nothing else in the group claims bit 5, so it is
 *      plausibly the group's own clock rather than a peripheral's.
 *   3. **0x4008011C's mystery value is 0x31**, and sl6806_pwm.h already has
 *      it and already argues it is a plain enable rather than a reparent.
 *      This is the second, independent sighting of the same write.
 */
#define SL6806_BT_GROUP_MODULE_ID   SL6806_PERIPH_GROUP_MODULE_ID
#define SL6806_BT_GROUP_PERIPH_ID   SL6806_PERIPH_GROUP_PERIPH_ID

/* [V] This block's own two ids, from 0x00D98CAE / 0x00D98CB4. */
#define SL6806_BT_PERIPH_ID         0       /* [V] 0x400E0000 bit 0          */
#define SL6806_BT_RESET_ID          0       /* [V] 0x400E0008 bit 0          */

/* Moved to sl6806_module.h, where the group bring-up now lives: the power
 * handshake at 0x40000070 belongs to the whole 0x400Exxxx window rather than
 * to this block. Kept as aliases so the probe still reads. */
#define SL6806_BT_PWR_REQ           SL6806_PWR_REQ
#define SL6806_BT_PWR_ACK_SHIFT     SL6806_PWR_ACK_SHIFT
#define SL6806_BT_PWR_DOMAINS       SL6806_PWR_DOMAINS
#define SL6806_BT_PWR_RELEASE       SL6806_PWR_RELEASE

/* ------------------------------------------------------------------ */
/* The window                                                          */
/* ------------------------------------------------------------------ */

/* [I] Base. The one literal load of this constant anywhere in FIRM is at
 * 0x00D98C9E, inside the module registration described above. */
#define SL6806_BT_BASE          0x400E2000u

#define SL6806_BT_REG(off)      (*(volatile uint32_t *)(SL6806_BT_BASE + (off)))
#define SL6806_BT_ADDR(off)     (SL6806_BT_BASE + (uint32_t)(off))

/*
 * [I] Reset/enable toggle. 0x00D98C64/0x00D98C7A: bit 31 cleared, delayed
 * ~10 units via the shared delay veneer (0x00807214), then set again. Note
 * this is a *second* reset, inside the block, distinct from the 0x400E0008
 * bit 0 pulse above - the vendor does both.
 */
#define SL6806_BT_RESET         SL6806_BT_REG(0x228)
#define SL6806_BT_RESET_BIT     (1u << 31)

/* [I] Written 0xFFFFFFFF during the reset window above (0x00D98C6C/0x74).
 * Bit 0 is read back as a boolean by the accessor at 0x00D98C8C. */
#define SL6806_BT_CTRL          SL6806_BT_REG(0x200)

/* [I] OR'd with bit 24 (0x00D98CBC) immediately before the config struct is
 * unpacked into the offsets below - plausibly "load config" or "start". The
 * same register is written by nine other sites across all three clusters,
 * with bits 24 and 25 both appearing, so it is the window's shared control
 * word rather than this cluster's. */
#define SL6806_BT_LOAD          SL6806_BT_REG(0x214)
#define SL6806_BT_LOAD_BIT      (1u << 24)
#define SL6806_BT_LOAD_BIT2     (1u << 25)  /* [V] 0x00D989F8, cluster 3     */

/* [I] Read (0x00D98BF2), top nibble compared against 4 and used to index a
 * small table - the shape of a status/mode field, not decoded further. */
#define SL6806_BT_STATUS        SL6806_BT_REG(0x218)

/*
 * [I] THE COUNTERS. 0x00D9A8F4 is a switch over 0..3 returning these four
 * registers masked to 25, 30, 30 and 30 bits respectively, and nothing writes
 * any of them. See the note at the top of this file: free-running counters of
 * those widths next to an HCI stack read as a link controller's clocks.
 *
 * These are also the most useful registers in the whole window for a first
 * hardware run, for a reason that has nothing to do with Bluetooth: a counter
 * is the one kind of register that proves a block is *running* rather than
 * merely readable. examples/BtProbe samples them twice.
 */
#define SL6806_BT_CLK0          SL6806_BT_REG(0x228)    /* 25 bits */
#define SL6806_BT_CLK1          SL6806_BT_REG(0x22C)    /* 30 bits */
#define SL6806_BT_CLK2          SL6806_BT_REG(0x230)    /* 30 bits */
#define SL6806_BT_CLK3          SL6806_BT_REG(0x234)    /* 30 bits */
#define SL6806_BT_CLK0_MASK     0x01FFFFFFu
#define SL6806_BT_CLK_MASK      0x3FFFFFFFu

/*
 * [V] +0x22C carries two flags as well as its counter, which is unusual
 * enough to be worth naming: 0x00D9A8BC sets bit 31 and spins until it reads
 * back (and clears it and spins until it does not), a request/grant
 * handshake; 0x00D9A930 reads bit 30 as a boolean.
 */
#define SL6806_BT_REQ_BIT       (1u << 31)
#define SL6806_BT_GRANT_BIT     (1u << 30)

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

/*
 * [V] The other two clusters. Neither is loaded by the code next to the HCI
 * dispatcher, so neither is Bluetooth on the evidence that named this window
 * - they are simply the rest of what lives here, and a probe should read
 * them because a window where one cluster is live and another is not says
 * more than either alone.
 *
 * Cluster 2, +0x180..+0x1B8, is driven from 0x00D98EAC..0x00D990D4, with
 * +0x188 by far the most-used register in the whole window.
 *
 * Cluster 3, +0x300..+0x32C, has its own registration at 0x00D989D0 that
 * calls the same shared bring-up and then takes **0x400E0000 bit 1** and
 * resets both bit 1 and bit 0 - i.e. it resets this block as well as its own,
 * which is the one hint in the dump that the two are related hardware.
 */
#define SL6806_BT_C2_180        SL6806_BT_REG(0x180)
#define SL6806_BT_C2_188        SL6806_BT_REG(0x188)
#define SL6806_BT_C2_198        SL6806_BT_REG(0x198)
#define SL6806_BT_C3_300        SL6806_BT_REG(0x300)
#define SL6806_BT_C3_310        SL6806_BT_REG(0x310)
#define SL6806_BT_C3_PERIPH_ID  1           /* [V] 0x400E0000 bit 1          */

/* ------------------------------------------------------------------ */
/* The little that can be called a driver                              */
/* ------------------------------------------------------------------ */

/*
 * Perform the vendor's gate sequence and this block's own enable and reset.
 * In order: the power-domain handshake, the PLL, module clock 46,
 * 0x4008011C, functional clock bit 5, then bit 0 and the reset pulse.
 *
 * Returns 1 if 0x400E0000 read this block's bit back, 0 if it did not - and
 * a 0 is a real result, not a failure of this function: it means the gate is
 * still shut and the block is still where 14a and 15 left it.
 *
 * WHAT THIS WRITES, AND WHY THAT IS DEFENSIBLE. The PLL and 0x4008011C are
 * the two writes with any reach outside this block. sl6806_pwm.h's reading of
 * 0x4008011C - a plain enable, 0x31 on and 0x30 off, one bit apart - is why
 * it is done here rather than left to a future session; a reparent would not
 * differ from its own disabled value by a single bit. The PLL is started, not
 * switched onto anything. Neither touches a divider the core or USB is on.
 *
 * It is still the most invasive thing in this framework outside the register
 * file's LDOs. In payload mode it cannot brick anything - flash is never
 * written - but it can plausibly hang the device, and a hang takes the
 * console with it. Unplug, hold the boot button, plug back in.
 */
int sl6806_bt_begin(void);

/* Read the four counters into `out[4]`, masked to their widths. Returns 1 if
 * any of them differs from the previous call, which is the only test in this
 * file that could distinguish a running block from a readable one. */
int sl6806_bt_clocks(uint32_t out[4]);

#ifdef __cplusplus
}
#endif

#endif /* SL6806_BT_H */
