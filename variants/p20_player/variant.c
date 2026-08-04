/*
 * variant.c - GPIO tables for the P20 Player.
 *
 * Both tables are empty because the SL6806 GPIO registers have not been
 * found. An empty table is what makes sl6806_gpio_available() return 0, which
 * is what makes digital calls report instead of silently doing nothing.
 *
 * WHEN YOU HAVE THE REGISTERS, it looks like this - a port with atomic
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

/* One zeroed entry each: a zero-length array is not standard C, and the
 * counts below are what actually mark GPIO as unconfigured. Replace the
 * contents and set the counts to the real sizes. */
const sl6806_gpio_port_t sl6806_gpio_ports[1] = { { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 } };
const sl6806_pin_t       sl6806_pin_map[1]    = { { 0, 0 } };

#ifdef SL6806_GPIO_CONFIGURED
const uint8_t sl6806_gpio_nports = sizeof(sl6806_gpio_ports) / sizeof(sl6806_gpio_ports[0]);
const uint8_t sl6806_npins       = sizeof(sl6806_pin_map)    / sizeof(sl6806_pin_map[0]);
#else
const uint8_t sl6806_gpio_nports = 0;
const uint8_t sl6806_npins       = 0;
#endif
