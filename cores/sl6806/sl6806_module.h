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

#ifdef __cplusplus
}
#endif

#endif /* SL6806_MODULE_H */
