/*
 * sl6806_firm.h - start the stock application from a payload.
 *
 * =====================================================================
 *  WHAT THIS IS
 * =====================================================================
 * The device has two useful states. In bootloader mode the mask ROM is in
 * charge, a payload can be loaded over USB, and a great deal does not work -
 * the SD host is gated off, the serial port is gated off, and the whole
 * `0x400Exxxx` group needs its PLL started by hand. In normal operation the
 * HLKJ bootloader runs, brings the hardware up, and starts the application
 * out of flash, and everything works because that is the product.
 *
 * `MODE=firmware` would put this framework's own code in that second state,
 * and it is the one thing in this tree that can leave a device that does not
 * boot. **This is the half of it that cannot.** It starts the *vendor's*
 * application, from a payload, out of the flash image already on the device,
 * writing nothing:
 *
 *     read the FIRM header from XIP flash
 *     copy its segment to SRAM
 *     set the vector table base and the stack pointer
 *     jump
 *
 * A power cycle undoes all of it, because nothing was written anywhere
 * persistent. That is the whole safety argument, and it is why this exists
 * before MODE=firmware does.
 *
 * =====================================================================
 *  WHAT IT IS FOR
 * =====================================================================
 * Chiefly, deciding what "a payload cannot do it" means. The SD host takes a
 * command and reports nothing after seven hypotheses were closed by
 * measurement (docs/SD.md), and the one structural difference left is that a
 * payload runs in bootloader mode and the code that demonstrably reads cards
 * does not. If the application boots here and the card mounts on the host as
 * `301a:2801`, that difference is the answer and the SD investigation moves
 * from "is the block broken" to "what does bootloader mode withhold".
 *
 * It costs one run and a power cycle.
 *
 * =====================================================================
 *  WHAT IT COSTS WHILE IT RUNS
 * =====================================================================
 * **The USB link, and with it the console.** The application re-initialises
 * USB as a card reader; the ROM's download endpoint is gone the moment the
 * jump happens. So everything a run has to say must be said before it jumps,
 * and the observable afterwards is what the *device* does - the panel
 * lighting up, the host enumerating a mass-storage device - not what it
 * prints.
 *
 * =====================================================================
 *  THE ADDRESSES, AND THE ONE THAT IS INFERRED
 * =====================================================================
 * From the header at flash file `0x10000`, XIP `0x00C10000` (notes §6):
 *
 *     +0x00  0x00000030   header length
 *     +0x04  0x8F4E4434   build timestamp
 *     +0x10  0x00804C00   the entry, not the load address - see below
 *     +0x14  0x00804C01   the same, with the Thumb bit
 *     +0x1C  0x00005862   bytes to copy
 *
 * §6 read `+0x10` as "loadToRam" and §7m corrected it: the segment at file
 * `0x10030` opens with a **full 256-entry Cortex-M vector table**, `0x400`
 * bytes, so if the table is at the load address then the reset handler named
 * inside it - `0x00804C00` - sits immediately after it and the load address
 * is `0x00804800`. §7m confirmed that independently by counting which
 * candidate base made SRAM call targets land on function prologues: ten of
 * twenty-six at `0x00804800`, none at either alternative.
 *
 * **This code does not take that on faith.** It derives the load address the
 * same way - entry minus the vector table - and then checks the table against
 * itself: word 1 must be the entry it was derived from, and word 0 must be a
 * plausible SRAM stack pointer. Both hold for the shipped image (`0x0082D63C`
 * and `0x00804C01`). If a different build ever disagrees, this refuses rather
 * than jumping into the middle of something.
 */
#ifndef SL6806_FIRM_H
#define SL6806_FIRM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* [V] §5: FIRM at flash 0x010000, XIP at 0x00C10000. */
#define SL6806_FIRM_XIP     0x00C10000u
#define SL6806_FIRM_HDR_LEN 0x30u
/* [V] the segment follows the header. */
#define SL6806_FIRM_SEG_OFF 0x30u
/* [V] 256 Cortex-M vectors. The load address is the entry less this. */
#define SL6806_FIRM_VECTORS 0x400u

/*
 * SRAM, §3, used to sanity-check what the header claims. sl6806.h already
 * defines these on the device; this file is also compiled for host tests,
 * where that header is stubbed out, so it supplies them only if absent
 * rather than shadowing the real ones.
 */
#ifndef SL6806_SRAM_BASE
#define SL6806_SRAM_BASE    0x00800000u
#endif
#ifndef SL6806_SRAM_END
#define SL6806_SRAM_END     0x00900000u
#endif

enum {
    SL6806_FIRM_OK          = 0,
    SL6806_FIRM_ERR_HDR     = -1,  /* header length is not 0x30 */
    SL6806_FIRM_ERR_ENTRY   = -2,  /* entry is not Thumb, or not in SRAM */
    SL6806_FIRM_ERR_LENGTH  = -3,  /* length is zero or absurd */
    SL6806_FIRM_ERR_VECTORS = -4   /* the vector table disagrees with itself */
};

typedef struct {
    uint32_t timestamp;
    uint32_t entry;        /* Thumb address to branch to */
    uint32_t load;         /* where the segment goes: entry - 0x400 */
    uint32_t length;       /* bytes to copy */
    uint32_t stack;        /* vector[0], the application's initial SP */
} sl6806_firm_t;

/*
 * Read and check the header. `header` is 0x30 bytes and `vectors` the first
 * eight bytes of the segment that follows it - both are passed in rather than
 * read here so this can be tested without a device.
 *
 * Returns SL6806_FIRM_OK, or one of the errors above with `out` untouched.
 */
int sl6806_firm_parse(const uint8_t *header, const uint8_t *vectors,
                      sl6806_firm_t *out);

/* The same, reading XIP flash directly. Device only. */
int sl6806_firm_read(sl6806_firm_t *out);

/*
 * Copy the segment into SRAM and enter it. Does not return, and takes the USB
 * link with it.
 *
 * Returns only on failure - a `f` that did not come from a successful parse,
 * or a load address that would overwrite this code. Everything else is a
 * one-way trip ending in a power cycle.
 */
int sl6806_firm_boot(const sl6806_firm_t *f);

#ifdef __cplusplus
}
#endif

#endif /* SL6806_FIRM_H */
