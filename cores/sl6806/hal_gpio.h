/*
 * hal_gpio.h - GPIO abstraction for SL6806.
 *
 * =====================================================================
 *  READ THIS BEFORE EXPECTING digitalWrite() TO DO ANYTHING
 * =====================================================================
 * The SL6806 GPIO registers are NOT known. There is no public datasheet,
 * no register map in any dump analysed so far, and no published reverse
 * engineering of the pad controller. Nothing in this framework can invent
 * them.
 *
 * So GPIO here is a complete, working driver with exactly one hole in it,
 * and there are two ways to fill it:
 *
 *   - the register table below: fill it in once, in your variant, and every
 *     Arduino digital call starts working; or
 *   - the vendor back end further down. The stock firmware does not touch
 *     GPIO registers either - it calls a routine in the mask ROM's driver
 *     set with a packed pin id - so if you can reach that routine you get
 *     working pins without knowing a single register address.
 *
 * Until one of the two exists, calls report an error over Serial instead of
 * silently doing nothing, because a silent no-op on a GPIO API is how you
 * spend an afternoon debugging your wiring for no reason.
 *
 * HOW TO FIND THE REGISTERS
 * -------------------------
 * 1. Get a good 4 MiB dump (see docs/DUMPING.md).
 * 2. Run  tools/sl6806-find-mmio.py dump.bin  - it scans the FIRM image for
 *    literal-pool constants that look like peripheral bases and ranks them
 *    by how they are used (read-modify-write on a bit index = GPIO-shaped).
 * 3. Load FIRM in Ghidra at 0x00C10000 and look at the code around the
 *    top-ranked bases. A GPIO port is recognisable: three or four registers
 *    at a fixed stride, one written with (1 << pin), one read back.
 * 4. Confirm on hardware with examples/MmioProbe, which toggles a candidate
 *    register and lets you watch a pin with a meter or LED.
 * 5. Write what you found into the port table in your variant and define
 *    SL6806_GPIO_CONFIGURED.
 *
 * The struct below covers both common layouts (set/clear registers, or a
 * single read-modify-write output register), so you should not need to
 * change any code - only data.
 */
#ifndef SL6806_HAL_GPIO_H
#define SL6806_HAL_GPIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SL6806_REG_NONE 0xFFFFu   /* this port has no such register */

/*
 * One GPIO port (a bank of up to 32 pins). All fields except `base` are
 * byte offsets from `base`.
 */
typedef struct {
    uint32_t base;      /* MMIO base address of the port                   */
    uint16_t dir;       /* direction register                              */
    uint16_t out;       /* output data register (read-modify-write)        */
    uint16_t in;        /* input data register                             */
    uint16_t set;       /* atomic set-bits register, or SL6806_REG_NONE    */
    uint16_t clr;       /* atomic clear-bits register, or SL6806_REG_NONE  */
    uint16_t pull;      /* pull enable register, or SL6806_REG_NONE        */
    uint16_t pull_dir;  /* pull up/down select, or SL6806_REG_NONE         */
    uint8_t  dir_output_is_1; /* 1 if a set bit in `dir` means output      */
    uint8_t  npins;     /* pins implemented in this port (1..32)           */
} sl6806_gpio_port_t;

/*
 * One Arduino pin number -> (port, bit). Supplied by the variant.
 */
typedef struct {
    uint8_t port;       /* index into the variant's port table */
    uint8_t bit;        /* bit position within the port        */
} sl6806_pin_t;

/* Supplied by the variant (variants/<board>/variant.c). */
extern const sl6806_gpio_port_t sl6806_gpio_ports[];
extern const uint8_t            sl6806_gpio_nports;
extern const sl6806_pin_t       sl6806_pin_map[];
extern const uint8_t            sl6806_npins;

/* =====================================================================
 *  THE VENDOR BACK END
 * =====================================================================
 * The stock firmware never writes a GPIO register. It calls
 *
 *     0x00811C7C  gpio_write(id, value)      13 call sites
 *     0x00811C90  gpio_config(id, value)     23 call sites
 *     0x00811CB4  gpio_?(id, value)          18 call sites
 *
 * with a packed pin id, from code that lives in SRAM and is not present in
 * the flash image (docs/sl6806_re_notes.md 7d).
 *
 * BE WARNED: those routines are NOT resident in bootloader mode. A dump of
 * SRAM taken there shows uninitialised noise at 0x00811C7C, because the
 * application that installs them has never run (7f). So a payload cannot
 * simply call them, and this back end has no working implementation yet. It
 * is still the right shape for one - a variant that finds another way to
 * reach the vendor driver, or reimplements it once the registers are known,
 * plugs in here and every named pin below starts working.
 *
 * The id encoding, read off the 54 call sites:
 *
 *     bits [10:7]   configuration nibble; 0x0 or 0xF in everything observed
 *     bits [16:11]  pin selector, values 3, 14, 15, 39, 48, 54..59, 62, 63
 *     bits [18:17]  a second selector; 0 for the display pins, 2 for one
 *                   other group. Whether this is a bank number or more pin
 *                   bits cannot be told apart from call sites alone.
 *
 * SL6806_GPIO_ID() builds the common case. Known ids are named in the
 * variant.
 *
 * The value argument is 0/1 in the reset paths but 0/0x40 in one other, so
 * it is more likely a pad-register value than a plain logic level. Check
 * against hardware before trusting a level other than 0 or 1.
 */
#define SL6806_GPIO_ID(pin)          (((uint32_t)(pin) & 0x3Fu) << 11)
#define SL6806_GPIO_ID_CFG(pin, cfg) (SL6806_GPIO_ID(pin) | \
                                      (((uint32_t)(cfg) & 0xFu) << 7))
#define SL6806_GPIO_ID_PIN(id)       (((uint32_t)(id) >> 11) & 0x3Fu)

/* Not a real pin id: marks an entry in the variant's vendor map as unknown,
 * so that pin reports instead of driving whatever id 0 turns out to be. */
#define SL6806_GPIO_ID_NONE          0xFFFFFFFFu

typedef struct {
    /* Required. Mirrors the vendor gpio_write(id, value). */
    void     (*write)(uint32_t id, uint32_t value);
    /* Optional; without it digitalRead() reports instead of guessing. */
    uint32_t (*read)(uint32_t id);
    /* Optional; mirrors the vendor gpio_config(id, value). Not called by
     * pinMode(): which field of the vendor configuration word selects the
     * direction, and which the pull, is not known, so pinMode() leaves a
     * vendor-mapped pad exactly as whatever ran before us configured it.
     * Guessing a field would silently reconfigure the pad into something
     * arbitrary. Call this yourself if you know the value you want. */
    void     (*config)(uint32_t id, uint32_t value);
} sl6806_gpio_vendor_t;

/*
 * Install a vendor back end. Pass NULL to remove it. The pointer is kept,
 * not copied. Returns 0, or -1 if `v` has no write().
 *
 * Once installed it takes priority over the register table for any pin that
 * has an id in sl6806_vendor_pin_map[], so a board can use both: the ROM
 * routine for pads whose id is known, registers for the rest.
 */
int sl6806_gpio_vendor_register(const sl6806_gpio_vendor_t *v);

/* Supplied by the variant: Arduino pin number -> vendor pin id, with
 * SL6806_GPIO_ID_NONE for pins whose id is unknown. May be empty. */
extern const uint32_t sl6806_vendor_pin_map[];
extern const uint8_t  sl6806_nvendor_pins;

/* Low-level accessors - these are what the wiring layer calls. */
void     sl6806_gpio_set_dir(uint8_t pin, int output);
void     sl6806_gpio_write(uint8_t pin, int value);
int      sl6806_gpio_read(uint8_t pin);
void     sl6806_gpio_set_pull(uint8_t pin, int enable, int up);

/*
 * Returns 1 if this build can actually drive a pin - either the variant has
 * a real register table, or a vendor back end is installed and the variant
 * knows at least one pin id. Sketches can check this to degrade gracefully.
 */
int      sl6806_gpio_available(void);

/*
 * Called once when an unconfigured GPIO call is made. Weak - override it to
 * change the reporting behaviour (for example to halt instead of warn).
 */
void     sl6806_gpio_unconfigured(const char *what);

#ifdef __cplusplus
}
#endif

#endif /* SL6806_HAL_GPIO_H */
