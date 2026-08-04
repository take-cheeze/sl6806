/*
 * variant.c - pin tables for the P20 Player.
 *
 * There are two tables here because there are two ways to drive a pin on
 * this chip, and this board knows something about one of them.
 *
 * 1. sl6806_gpio_ports[] - direct register access. Still empty: the SL6806
 *    pad controller registers have not been found. An empty table is what
 *    makes the register path report instead of silently doing nothing.
 *
 * 2. sl6806_vendor_pin_map[] - the packed pin ids the stock firmware passes
 *    to the mask ROM's GPIO routine. These ARE known, for the pins the stock
 *    firmware touches, because they appear as immediates at its call sites.
 *    Install a vendor back end with sl6806_gpio_vendor_register() and every
 *    named pin below starts working without any register map at all.
 *
 * WHEN YOU HAVE THE REGISTERS, table 1 looks like this - a port with atomic
 * set/clear registers at a 4-byte stride, and pins 0..31 mapped straight
 * through:
 *
 *   #define SL6806_GPIO_CONFIGURED 1
 *
 *   const sl6806_gpio_port_t sl6806_gpio_ports[] = {
 *       {
 *           .base = 0x40010000,      // <- the base you found
 *           .dir  = 0x00,            // <- register offsets
 *           .out  = 0x04,
 *           .in   = 0x08,
 *           .set  = 0x0C,            // or SL6806_REG_NONE if absent
 *           .clr  = 0x10,            // or SL6806_REG_NONE if absent
 *           .pull = SL6806_REG_NONE,
 *           .pull_dir = SL6806_REG_NONE,
 *           .dir_output_is_1 = 1,    // 0 if a set bit means *input*
 *           .npins = 32,
 *       },
 *   };
 *
 *   const sl6806_pin_t sl6806_pin_map[] = {
 *       {0, 0}, {0, 1}, {0, 2}, ...  // Arduino pin -> {port, bit}
 *   };
 *
 * Get dir_output_is_1 wrong and outputs read back correctly but never drive
 * the pin, so check it against a meter early.
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
     * 0x00D3E1A4 drives exactly this id high / 10 ms / low / 20 ms / high. */
    0x13800u,

    /* 8  PIN_EXT_RESET. [V] id, [I] role: driven low / 10 ms / high / 50 ms
     * at 0x00D401E6, immediately before a register read over what looks like
     * an I2C transfer to address 0x15 - so, the reset line of an I2C device
     * (touch controller or camera; the scene table has both). */
    0x18000u,
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
 *   0x41F80 0x47080 0x47780 0x47880 0x47F80
 *       the second selector group. Driven with values 0 and 0x40 in a
 *       power-sequencing routine at 0x00D44AB8.
 */
