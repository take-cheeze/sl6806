/*
 * hal_gpio.h - GPIO abstraction for SL6806.
 *
 * =====================================================================
 *  READ THIS BEFORE EXPECTING digitalWrite() TO DO ANYTHING
 * =====================================================================
 * The registers ARE known now - see cores/sl6806/sl6806_padctl.h, which is
 * the pad controller read out of the mask ROM. What is still missing on this
 * board is not the registers but the *pinout*: which pad each button and LED
 * is wired to. A pin whose pad id nobody knows is still a pin you cannot
 * drive, so most of the names in the variant still report rather than
 * pretend.
 *
 * There are two ways to reach a pad, and a board can use both at once:
 *
 *   - the vendor back end: a packed pad id and a driver behind it. This is
 *     how the stock firmware works, it is how the panel reset line is
 *     driven, and sl6806_padctl_vendor() is a ready-made implementation:
 *
 *         sl6806_gpio_vendor_register(sl6806_padctl_vendor());
 *
 *     On the P20 that already happens when the display comes up.
 *
 *   - the register table below: an Arduino pin number mapped to a port and
 *     a bit. Note that this model does not fit the SL6806 well - direction
 *     here is a four-bit function field, not a bit in a direction register -
 *     so the vendor path is the one to prefer on this chip. The table stays
 *     for boards where it does fit.
 *
 * When neither can reach a pin, calls report an error over Serial instead of
 * silently doing nothing, because a silent no-op on a GPIO API is how you
 * spend an afternoon debugging your wiring for no reason.
 *
 * HOW TO FIND A PIN'S PAD ID
 * --------------------------
 * The ids are immediates at the stock firmware's call sites, so:
 *
 * 1. Get a good 4 MiB dump (see docs/DUMPING.md).
 * 2. Find the calls to the vendor GPIO routines - 0x00811C7C and its
 *    siblings - and read the first argument at each. There are 54 of them.
 * 3. Decode the id with the macros in sl6806_padctl.h to see which bank and
 *    pin it names, and read the code around the call to guess its job.
 * 4. Confirm on hardware: drive it and watch with a meter, or configure it
 *    as an input and press the button you think it is.
 * 5. Add it to sl6806_vendor_pin_map[] in your variant.
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
 * The stock firmware never writes a GPIO register directly. It calls
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
 * application that installs them has never run (7f). So calling *them* is
 * out - but they are thunks into the mask ROM, and the ROM is resident, so
 * reimplementing what they do is not. cores/sl6806/sl6806_padctl.c does
 * exactly that, and sl6806_padctl_vendor() returns it in this shape.
 *
 * The id encoding, decoded from ROM 0x902 and ROM 0x93C rather than guessed
 * from call sites. See sl6806_padctl.h for the full version:
 *
 *     bits [19:16]  bank
 *     bits [15:11]  pin within the bank
 *     bits [10:7]   pad function: 0 input, 1 output, 2+ alternate
 *     bit  [6]      initial output level
 *     bits [5:4]    drive strength
 *     bits [3:0]    pull selector
 *
 * SL6806_GPIO_ID() builds the common case - bank 0 and no configuration -
 * which is enough to address a pad for a read or a write, because neither
 * looks at the configuration fields. Known ids are named in the variant.
 */
/* Address a pad for a read or a write: bank and pin, no configuration. The
 * same as SL6806_PAD_ID(bank, pin, 0); duplicated so this header stays
 * usable on its own. */
#define SL6806_GPIO_ID(bank, pin)    ((((uint32_t)(bank) & 0x0Fu) << 16) | \
                                      (((uint32_t)(pin)  & 0x1Fu) << 11))
#define SL6806_GPIO_ID_BANK(id)      (((uint32_t)(id) >> 16) & 0x0Fu)
#define SL6806_GPIO_ID_PIN(id)       (((uint32_t)(id) >> 11) & 0x1Fu)

/* Not a real pin id: marks an entry in the variant's vendor map as unknown,
 * so that pin reports instead of driving whatever id 0 turns out to be. */
#define SL6806_GPIO_ID_NONE          0xFFFFFFFFu

typedef struct sl6806_gpio_vendor {
    /* Required. Mirrors the vendor gpio_write(id, value). */
    void     (*write)(uint32_t id, uint32_t value);
    /* Optional; without it digitalRead() reports instead of guessing. */
    uint32_t (*read)(uint32_t id);
    /* Optional; mirrors the vendor gpio_config(id, value). Not called by
     * pinMode() - it is the whole configuration word, and a back end that
     * does not know which field is which would be guessing. Call it yourself
     * if you know the value you want. */
    void     (*config)(uint32_t id, uint32_t value);
    /*
     * Optional. Set the pad's direction and nothing else.
     *
     * This is what pinMode() calls. It exists separately from config()
     * because on this chip direction turned out to be one field of the pad's
     * function nibble (sl6806_padctl.h), so it can be changed on its own
     * without disturbing drive strength or pull. A back end that cannot
     * separate the two leaves this NULL, and pinMode() then leaves the pad
     * as whatever configured it - which is still better than writing a
     * guessed word into it.
     */
    void     (*set_dir)(uint32_t id, int output);
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
