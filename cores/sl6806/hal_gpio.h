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
 * So GPIO here is a complete, working driver with exactly one hole in it:
 * the register addresses. Fill in the table below - once - and every
 * Arduino digital call starts working. Until then, calls into it report an
 * error over Serial instead of silently doing nothing, because a silent
 * no-op on a GPIO API is how you spend an afternoon debugging your wiring
 * for no reason.
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

/* Low-level accessors - these are what the wiring layer calls. */
void     sl6806_gpio_set_dir(uint8_t pin, int output);
void     sl6806_gpio_write(uint8_t pin, int value);
int      sl6806_gpio_read(uint8_t pin);
void     sl6806_gpio_set_pull(uint8_t pin, int enable, int up);

/*
 * Returns 1 if the variant has a real register table, 0 if GPIO is still
 * unconfigured. Sketches can check this to degrade gracefully.
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
