/*
 * sl6806_padctl.h - the SL6806 pad controller (GPIO and pin multiplexing).
 *
 * =====================================================================
 *  THIS IS A DRIVER. THE REGISTERS ARE KNOWN.
 * =====================================================================
 * Earlier notes in this tree say the GPIO registers had not been found. They
 * have. They are not in the flash image because no code in flash touches
 * them directly: the bootloader and the application both reach GPIO through
 * thunks into the mask ROM, and the ROM is where the driver lives.
 *
 *     bootloader 0x00823C6C  ->  ROM 0x0000099E   set level
 *     bootloader 0x00823C68  ->  ROM 0x0000097C   read level
 *     bootloader 0x008207B0  ->  ROM 0x0000093C   configure a pad
 *
 * Every one of those starts by calling ROM 0x00000902, which splits a packed
 * pin id into a bank base and a pin index, and the bank bases are a six-entry
 * table at ROM 0x00065004. That table is the thing that was missing:
 *
 *     0x40081000  0x40081040  0x400F6080  0x400810C0  0x40081100  0x400F6000
 *
 * All of the below is read directly out of the mask ROM (maskrom.bin, see
 * docs/DUMPING.md), so it carries [V]. What it has not had is a meter on a
 * pin - see the note on sl6806_pad_write().
 *
 * THE PACKED PIN ID
 *
 * The vendor never passes a pin number. It passes one word that names the
 * pad and says how to configure it, and every call site in both images has
 * it as an immediate - which is why ids like 0x13800 were recoverable long
 * before the registers were. ROM 0x902 and ROM 0x93c between them decode it:
 *
 *     bits [19:16]  bank, index into the table above          [V]
 *     bits [15:11]  pin within the bank, 0..31                [V]
 *     bits [10:7]   pad function: 0 = input, 1 = output,      [V]
 *                   2..15 = alternate functions (the P20's
 *                   LCD data pads use 2)
 *     bit  [6]      initial output level                      [V]
 *     bits [5:4]    drive strength, 0..3                      [V]
 *     bits [3:0]    pull selector; 4..15 index the pull table [V]
 *                   at ROM 0x0006501C, anything else means no
 *                   pull
 *
 * So 0x000138CB is bank 1, pin 7, function 1 (output), initial level 1,
 * drive 0, pull selector 0xB - which is exactly the P20's panel reset line,
 * configured as a high output. And 0x13800, the id the application passes to
 * its gpio_write, is the same bank and pin with the configuration fields
 * zeroed, because a write does not re-configure the pad.
 *
 * REGISTER MAP, offsets from a bank base
 *
 *     +0x000 + (pin>>3)*4   function, 4 bits per pin        [V] ROM 0x6F0
 *     +0x010                input data, 1 bit per pin       [V] ROM 0x97C
 *     +0x014 + (pin>>4)*4   drive, 2 bits per pin           [V] ROM 0x76A
 *     +0x024 + (pin>>4)*4   pull a, 2 bits per pin          [V] ROM 0x736
 *     +0x02C + (pin>>4)*4   pull b, 2 bits per pin          [V] ROM 0x736
 *     +0x034                set output bit, write 1<<pin    [V] ROM 0x6DE
 *     +0x038                clear output bit, write 1<<pin  [V] ROM 0x6DE
 *     +0x200 + (pin>>3)*4   interrupt mode, 4 bits per pin  [V] ROM 0x78C
 *     +0x210                interrupt enable                [V] ROM 0x7D0
 *     +0x214                interrupt pending, W1C          [V] ROM 0x7BC
 *
 * Direction is the function nibble, not a separate register: 0 is input and
 * 1 is output. ROM 0x718 sets function 1 and then drives the pin, which is
 * what pins that down.
 */
#ifndef SL6806_PADCTL_H
#define SL6806_PADCTL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------- the packed id */

#define SL6806_PAD_BANK(id)     (((uint32_t)(id) >> 16) & 0x0Fu)
#define SL6806_PAD_PIN(id)      (((uint32_t)(id) >> 11) & 0x1Fu)
#define SL6806_PAD_FUNC(id)     (((uint32_t)(id) >>  7) & 0x0Fu)
#define SL6806_PAD_LEVEL(id)    (((uint32_t)(id) >>  6) & 0x01u)
#define SL6806_PAD_DRIVE(id)    (((uint32_t)(id) >>  4) & 0x03u)
#define SL6806_PAD_PULL(id)     ( (uint32_t)(id)        & 0x0Fu)

#define SL6806_PAD_ID(bank, pin, func) \
    ((((uint32_t)(bank) & 0x0Fu) << 16) | \
     (((uint32_t)(pin)  & 0x1Fu) << 11) | \
     (((uint32_t)(func) & 0x0Fu) <<  7))

#define SL6806_PAD_FUNC_INPUT   0u
#define SL6806_PAD_FUNC_OUTPUT  1u

/* Number of banks in the ROM's table. A bank field larger than this is not a
 * pad id, and every call below rejects it rather than writing to whatever
 * address the arithmetic produces. */
#define SL6806_PAD_NBANKS       6

/* [V] The bank bases, ROM 0x00065004. Exposed because it is the one piece of
 * this that a different SL6806-family part would change. */
extern const uint32_t sl6806_pad_bank_base[SL6806_PAD_NBANKS];

/* ------------------------------------------------------------- the API */

/*
 * Apply a packed id in full: function, initial output level, drive strength
 * and pull. This is ROM 0x93C reimplemented, and it is what brings the LCD
 * data pads and the panel reset line up.
 *
 * Returns 0, or -1 if the id names a bank that does not exist.
 */
int sl6806_pad_configure(uint32_t id);

/* Change only the function nibble, leaving drive and pull alone. */
int sl6806_pad_set_func(uint32_t id, unsigned func);

/* Drive a pad that is already configured as an output. The configuration
 * fields of `id` are ignored, so the application's bare 0x13800 and this
 * file's fully specified 0x000138CB address the same pin.
 *
 * [V] register sequence, [I] that it moves a physical pin: nothing here has
 * been watched on a meter. If a pad does not move, the function nibble is
 * the first thing to check - a pad left in an alternate function will accept
 * these writes and ignore them.
 */
int sl6806_pad_write(uint32_t id, unsigned level);

/* Read a pad's input register. Returns 0 or 1, or -1 for a bad id. */
int sl6806_pad_read(uint32_t id);

/*
 * A ready-made vendor back end for hal_gpio.h, so that a variant which knows
 * its pin ids gets working digitalWrite()/digitalRead()/pinMode() by calling
 *
 *     sl6806_gpio_vendor_register(sl6806_padctl_vendor());
 *
 * and nothing else.
 */
struct sl6806_gpio_vendor;
const struct sl6806_gpio_vendor *sl6806_padctl_vendor(void);

/*
 * The pad ids this framework knows for a pin, as a reminder of what the
 * fields mean when you are staring at one in a disassembly:
 *
 *   0x00013800  bank 1 pin 7, no configuration - what the application's
 *               gpio_write is passed for the panel reset line
 *   0x000138CB  the same pad, configured: output, initially high, pull
 *               selector 0xB. What the application's pad setup passes.
 */

#ifdef __cplusplus
}
#endif

#endif /* SL6806_PADCTL_H */
