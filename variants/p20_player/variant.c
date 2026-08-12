/*
 * variant.c - pin tables for the P20 Player.
 *
 * There are two tables here because hal_gpio.h offers two ways to reach a
 * pad. On this chip only one of them is the right shape.
 *
 * 1. sl6806_gpio_ports[] - an Arduino pin number mapped to a port and a bit,
 *    with a direction register. Deliberately left empty. The SL6806's pad
 *    controller does not work that way: direction is a four-bit function
 *    field shared with the alternate-function selector, so a "direction
 *    register" entry would either be wrong or would have to lie. The
 *    registers themselves are known - sl6806_padctl.h - this table is simply
 *    not how to use them.
 *
 * 2. sl6806_vendor_pin_map[] - the packed pad ids. This is how the stock
 *    firmware addresses pins and how this board does too. The ids for the
 *    pins the stock firmware touches are known, because they appear as
 *    immediates at its call sites.
 *
 * The back end behind table 2 is sl6806_padctl_vendor(), and on this board
 * the display bring-up installs it (see lcdc.c) - so once anything has
 * touched Screen, every named pin below with a real id works. A sketch that
 * wants pins without a display can install it itself:
 *
 *     sl6806_gpio_vendor_register(sl6806_padctl_vendor());
 *
 * WHAT IS STILL MISSING is the board's pinout, not the driver: which pad
 * each button is wired to. Every SL6806_GPIO_ID_NONE below is a pin nobody
 * has identified yet.
 */

#include "hal_gpio.h"
#include "variant.h"

/* One zeroed entry each: a zero-length array is not standard C, and the
 * counts below are what actually mark the register path as unconfigured.
 * Replace the contents and set the counts to the real sizes. */
const sl6806_gpio_port_t sl6806_gpio_ports[1] = { { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 } };
const sl6806_pin_t       sl6806_pin_map[1]    = { { 0, 0 } };

#ifdef SL6806_GPIO_CONFIGURED
const uint8_t sl6806_gpio_nports = sizeof(sl6806_gpio_ports) / sizeof(sl6806_gpio_ports[0]);
const uint8_t sl6806_npins       = sizeof(sl6806_pin_map)    / sizeof(sl6806_pin_map[0]);
#else
const uint8_t sl6806_gpio_nports = 0;
const uint8_t sl6806_npins       = 0;
#endif

/*
 * Arduino pin number -> vendor pin id.
 *
 * Every entry that is not SL6806_GPIO_ID_NONE was read out of the stock
 * firmware as an immediate at a call site of the vendor GPIO routine at
 * 0x00811C7C, so the id itself is verified even where the pin's job is only
 * inferred from the code around it. Provenance is per line.
 *
 * The order matches the names in variant.h. Pins with no known id stay
 * SL6806_GPIO_ID_NONE so that using them reports rather than driving some
 * unrelated pad.
 */
const uint32_t sl6806_vendor_pin_map[] = {
    SL6806_GPIO_ID_NONE,        /* 0  LED_BUILTIN   [?] no LED found      */
    SL6806_GPIO_ID_NONE,        /* 1  BTN_PLAY      [?]                   */
    SL6806_GPIO_ID_NONE,        /* 2  BTN_PREV      [?]                   */
    SL6806_GPIO_ID_NONE,        /* 3  BTN_NEXT      [?]                   */
    SL6806_GPIO_ID_NONE,        /* 4  BTN_MENU      [?]                   */
    SL6806_GPIO_ID_NONE,        /* 5  BTN_VOL_UP    [?]                   */
    SL6806_GPIO_ID_NONE,        /* 6  BTN_VOL_DOWN  [?]                   */

    /* 7  PIN_LCD_RESET. [V] id, [V] role: the panel reset routine at
     * 0x00D3E1A4 drives exactly this id high / 10 ms / low / 20 ms / high.
     * Bank 1 pin 7; the display bring-up configures it as an output, so this
     * is the one entry here that is known to be usable end to end. */
    0x13800u,

    /* 8  PIN_EXT_RESET, also PIN_TP_RESET. [V] id, [V] role: the reset line
     * of the touch controller. Driven low / 10 ms / high / 50 ms at
     * 0x00D401E6, immediately before reading register 0xA7 over I2C from
     * address 0x15 - the CST816 chip-id register. The camera is a separate
     * device on a separate bus (bank 4, address 0x68), which is what settles
     * the ambiguity this comment used to carry. See notes §7h.
     * The pad is configured as 0x000180F0 - output, drive 3. */
    0x18000u,

    /* 9  PIN_TP_INT. [V] id, [V] role: the touch controller's interrupt.
     * The vendor arms it at 0x00D3FEF0 and its handler at 0x00D3FF24 reads
     * the pad and only queues an I2C read when it reads 0, so the line is
     * active low. The vendor puts the pad on alt function 14 with a pull-up
     * (0x00016F08); function 14 is one of only two functions §7g measured
     * that leave the input buffer alive, so as a plain input (function 0)
     * this pad is readable too - which is what digitalRead() will do. */
    0x16800u,
};

const uint8_t sl6806_nvendor_pins =
    sizeof(sl6806_vendor_pin_map) / sizeof(sl6806_vendor_pin_map[0]);

/*
 * Other vendor pin ids seen in the stock firmware, recorded but not mapped to
 * an Arduino pin number because their function is not established. Add them
 * above once you know what they do.
 *
 *   0x1B000 0x1B800 0x1C000 0x1C800 0x1D000 0x1D800 0x1F000 0x1F800
 *       pins 54..59, 62, 63. Used with the config routine at 0x00811C90 and
 *       a value of 0x780, from code around 0x00D93E00-0x00D94800.
 *   0x47080 0x47880
 *       the camera's RESET and PWDN, bank 4 pins 14 and 15. Driven with 0
 *       and 0x40 - the level lives in bit 6 of the packed id - by the sensor
 *       power-up at 0x00D44B1C: both low, 5 ms, RESET high, 5 ms, PWDN high,
 *       20 ms, then the chip-id read. Not mapped to an Arduino pin because
 *       driving them without a sensor driver does nothing useful.
 *   0x41930
 *       the camera's MCLK, bank 4 pin 3, alt function 2. Not a GPIO.
 *   0x46138 0x46938   bank 4 pins 12, 13 - TWI0, the camera's I2C.
 *   0x17618 0x17E18   bank 1 pins 14, 15 - TWI1, the touch panel's I2C.
 *       All four are alternate functions, so they are pads to leave alone
 *       rather than pins to drive.
 *   0x41F80 0x47780 0x47F80
 *       the rest of that selector group, role still unknown.
 *
 * See docs/sl6806_re_notes.md §7h for the touch and camera devices these
 * belong to, register by register.
 */
