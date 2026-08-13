/*
 * sl6806_module.h - turning a peripheral's clock on, the way the mask ROM does.
 *
 * =====================================================================
 *  THE ORDER IS THE OPERATION
 * =====================================================================
 * Read this before writing a gate register by hand. A peripheral on this chip
 * is enabled by writing its bit to a *shadow* register, then to a *gate*
 * register, then waiting for the gate to read the bit back. Writing both at
 * once and moving on leaves the peripheral exactly as dead as writing
 * neither - and a dead peripheral here still answers reads with plausible
 * values, so the failure is invisible without hardware.
 *
 * That combination cost four hardware runs: a sweep set the ADC's correct bit
 * in both registers, read the block back, saw sensible numbers, and recorded
 * the bit as not working. See docs/sl6806_re_notes.md 15b.
 *
 * WHERE IT COMES FROM. 0x00001C5C in the mask ROM is module_clock_enable(id),
 * and 0x00001CE8 is its disable. Between them they say the module space is
 * 128 ids across four register pairs, and the fourth is not in the CRU:
 *
 *     id  0.. 31   CRU +0x60 gate, +0x70 shadow
 *     id 32.. 63   CRU +0x64 gate, +0x74 shadow
 *     id 64.. 95   CRU +0x68 gate, +0x78 shadow
 *     id 96..127   0x400F1000 +0x20 gate, +0x30 shadow
 *
 * Nothing in the ROM, the bootloader or the application calls it. The
 * bootloader gates the LCDC through the CRU by hand; the application uses
 * 0x400E0000, which a payload cannot reach at all. So the routine documents
 * the mechanism without revealing any peripheral's id, and those have to be
 * found by walking - which is what examples/Buttons did to find the ADC's.
 *
 * KNOWN IDS. Only what has been measured:
 *
 *     84   the ADC (sl6806_adc.h) - verified, the keys work through it
 *     68   the PWM's registers (sl6806_pwm.h) - they become writable, though
 *          its counter still does not run
 *     46   the camera front end (sl6806_dvp.h) - the block at 0x400E1000
 *          goes from reading zeros and dropping writes to holding them.
 *          Found 2026-08-13 by examples/DvpProbe, and worth reading next to
 *          what it displaced: the vendor's own enable at 0x400E0000 will not
 *          hold its own bit from a payload, so the firmware's mechanism and
 *          the reachable one are different mechanisms.
 *
 * Provenance markers as elsewhere: [V] verified against the dump,
 * [M] measured on hardware.
 */
#ifndef SL6806_MODULE_H
#define SL6806_MODULE_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Enable one module's clock. Returns 1 if the gate acknowledged, 0 on
 * timeout or if the id is out of range.
 *
 * Reimplemented rather than calling the ROM, because the ROM's poll is
 * unbounded: an id nothing implements would spin there forever, and in a
 * payload that means inside the boot ROM's USB handler, which takes the
 * device off the bus. The poll here is short on purpose - an acknowledgement
 * is a few cycles of hardware, and a walk pays the timeout for every miss.
 */
int sl6806_module_enable(unsigned id);

/*
 * Is a module's clock already on? Both the shadow and the gate, which is what
 * ROM 0x1E54 tests and what "enabled" has to mean given the order above.
 *
 * The vendor uses exactly this as a once-guard: 0x00D9A7FC checks module 46
 * and returns without starting the PLL if it is already up.
 */
int sl6806_module_enabled(unsigned id);

/*
 * Turn one off. ROM 0x1CE8, whose order is the reverse of the enable's: gate
 * first, then shadow.
 *
 * This is the dangerous direction - a functional clock something else is
 * using, the USB the console rides on for instance, is not yours to switch
 * off. Call it only on an id you enabled yourself, in the same sketch.
 */
void sl6806_module_disable(unsigned id);

/*
 * ===================================================================
 *  THE SECOND ENABLE, AND THE ORDER BETWEEN THEM
 * ===================================================================
 * A module clock gives a peripheral its REGISTERS. It does not give it its
 * FUNCTION. That distinction cost this project two peripherals and a lot of
 * bench time, and both failures looked identical from the outside:
 *
 *   - the PWM (id 68) took every register write and its counter never ran,
 *     so the backlight lit but would not dim (14a);
 *   - the camera front end (id 46) took a full geometry configuration,
 *     reported it back, and drove no MCLK, so the sensor stayed mute
 *     through eight pad combinations and sixteen clock settings (7n).
 *
 * The functional clock is a bit in 0x400E0000, one per peripheral - the
 * register 15 recorded as "dead from a payload" after it would not hold a
 * bit. **It is not dead. It is gated, and its own gate is a module clock.**
 *
 * MEASURED 2026-08-13: cold, 0x400E0000 ignores writes. After
 * sl6806_module_enable(46) it holds them. So the order is
 *
 *     sl6806_module_enable(<the module id>);      registers
 *     sl6806_periph_enable(<the peripheral id>);  function
 *
 * and the two ids are from different spaces - 46 and 6 for the camera. Every
 * sweep that ever wrote 0x400E0000 did it from a cold chip and concluded the
 * register was unreachable; none had a peripheral awake enough to gate it
 * open first.
 *
 * WHICH BIT IS WHICH PERIPHERAL is known only for the camera, id 6, from
 * 0x00D98236. The others have to be walked, the same way the module ids were.
 */
#define SL6806_PERIPH_ENABLE_REG   0x400E0000u
#define SL6806_PERIPH_RESET_REG    0x400E0008u

/*
 * ===================================================================
 *  OPENING THE GATE IN FRONT OF 0x400E0000
 * ===================================================================
 * [M] 2026-08-13, examples/BtProbe: the vendor's own group bring-up
 * (0x00D9A7FC) runs from a payload, the PLL locks - 0x40080008 written
 * 0xC0000C04 comes back 0xD0010C04 with bit 28 set - and 0x400E0000 holds
 * 0x21 afterwards. The console survives it. That closes two open items §14a
 * left: the PLL was only ever seen stopped before, and 0x4008011C = 0x31 was
 * flagged "plausibly safe but untried".
 *
 * This used to live in sl6806_bt.c, which was the wrong home: module 46 gates
 * the whole 0x400Exxxx window, not Bluetooth, and every peripheral in the
 * group calls the same routine first.
 *
 * Returns 1 if module 46 acknowledged. Guarded like the vendor's: does
 * nothing if module 46 is already up, so a second call is cheap and does not
 * restart a PLL something else may be using.
 */
#define SL6806_PERIPH_GROUP_MODULE_ID  46   /* [V] gates 0x400E0000 itself */
#define SL6806_PERIPH_GROUP_PERIPH_ID  5    /* [V] taken by the shared path */

/*
 * [V] The power-domain handshake the group bring-up does before the PLL
 * (0x00D9A7AC), and it is at the pad-mux base, not the CRU:
 *
 *     for (i = 0; i < 10; i++)
 *         if (!(0x40000070 & (1 << (16 + i)))) {
 *             0x40000070 |= (1 << i);
 *             while (!(0x40000070 & (1 << (16 + i)))) ;
 *         }
 *     0x40000074 = 0;
 *
 * Ten request bits in [9:0], ten acknowledgements in [25:16]. [?] what the
 * ten domains are - this is the only code in the dump that touches either
 * register, so there is nothing to cross-reference it against.
 */
#define SL6806_PWR_REQ                 0x40000070u
#define SL6806_PWR_ACK_SHIFT           16
#define SL6806_PWR_DOMAINS             10
#define SL6806_PWR_RELEASE             0x40000074u

int sl6806_periph_group_begin(void);

/*
 * Set a peripheral's functional clock bit and wait the vendor's 10 ms
 * (0x00D9A734). Returns 1 if the bit read back, 0 if it did not - which
 * means the register is still gated, not that the id is wrong.
 */
int sl6806_periph_enable(unsigned id);

/*
 * The vendor's reset pulse on the *other* register, 0x400E0008: clear the
 * bit, wait, set it again (0x00D9A768). Easy to miss - it is a different
 * address from the enable, and nothing in this tree noticed it until the
 * camera front end was decoded.
 */
void sl6806_periph_reset(unsigned id);

#ifdef __cplusplus
}
#endif

#endif /* SL6806_MODULE_H */
