/*
 * hal_gpio.c - GPIO driver. Complete except for the register table, which
 * the variant supplies. See hal_gpio.h for how to fill that in.
 */

#include "sl6806.h"
#include "hal_gpio.h"
#include "sl6806_console.h"

#define REG(base, off) (*(volatile uint32_t *)((base) + (off)))

int sl6806_gpio_available(void)
{
    return sl6806_gpio_nports != 0 && sl6806_npins != 0;
}

__attribute__((weak)) void sl6806_gpio_unconfigured(const char *what)
{
    static uint8_t reported;

    if (reported)
        return;
    reported = 1;

    sl6806_debug_print(
        "\r\n"
        "*** SL6806: GPIO is not configured ***\r\n"
        "    "); sl6806_debug_print(what); sl6806_debug_print(" was called, but this build has no GPIO\r\n"
        "    register map. The SL6806 pad controller has not been reverse\r\n"
        "    engineered, so the framework cannot guess it.\r\n"
        "    Fill in sl6806_gpio_ports[] in your variant and define\r\n"
        "    SL6806_GPIO_CONFIGURED. See cores/sl6806/hal_gpio.h.\r\n"
        "    (reported once; further GPIO calls are ignored)\r\n\r\n");
}

/* Resolve a pin to its port descriptor, or NULL. */
static const sl6806_gpio_port_t *port_of(uint8_t pin, uint8_t *bit)
{
    const sl6806_pin_t *p;

    if (!sl6806_gpio_available()) {
        sl6806_gpio_unconfigured("A digital pin function");
        return 0;
    }
    if (pin >= sl6806_npins) {
        sl6806_debug_print("SL6806: pin number out of range\r\n");
        return 0;
    }

    p = &sl6806_pin_map[pin];
    if (p->port >= sl6806_gpio_nports)
        return 0;

    *bit = p->bit;
    return &sl6806_gpio_ports[p->port];
}

void sl6806_gpio_set_dir(uint8_t pin, int output)
{
    const sl6806_gpio_port_t *pt;
    uint8_t bit;
    uint32_t pm, v;

    pt = port_of(pin, &bit);
    if (!pt || pt->dir == SL6806_REG_NONE)
        return;

    pm = sl6806_irq_save();
    v = REG(pt->base, pt->dir);
    if (!!output == !!pt->dir_output_is_1)
        v |= (1u << bit);
    else
        v &= ~(1u << bit);
    REG(pt->base, pt->dir) = v;
    sl6806_irq_restore(pm);
}

void sl6806_gpio_write(uint8_t pin, int value)
{
    const sl6806_gpio_port_t *pt;
    uint8_t bit;
    uint32_t pm, v;

    pt = port_of(pin, &bit);
    if (!pt)
        return;

    /* Prefer the atomic set/clear registers when the port has them. */
    if (value && pt->set != SL6806_REG_NONE) {
        REG(pt->base, pt->set) = (1u << bit);
        return;
    }
    if (!value && pt->clr != SL6806_REG_NONE) {
        REG(pt->base, pt->clr) = (1u << bit);
        return;
    }
    if (pt->out == SL6806_REG_NONE)
        return;

    pm = sl6806_irq_save();
    v = REG(pt->base, pt->out);
    if (value)
        v |= (1u << bit);
    else
        v &= ~(1u << bit);
    REG(pt->base, pt->out) = v;
    sl6806_irq_restore(pm);
}

int sl6806_gpio_read(uint8_t pin)
{
    const sl6806_gpio_port_t *pt;
    uint8_t bit;
    uint16_t off;

    pt = port_of(pin, &bit);
    if (!pt)
        return 0;

    /* Fall back to the output register if the port has no separate input
     * register - reading back a driven pin is still useful. */
    off = (pt->in != SL6806_REG_NONE) ? pt->in : pt->out;
    if (off == SL6806_REG_NONE)
        return 0;

    return (REG(pt->base, off) >> bit) & 1u;
}

void sl6806_gpio_set_pull(uint8_t pin, int enable, int up)
{
    const sl6806_gpio_port_t *pt;
    uint8_t bit;
    uint32_t pm, v;

    pt = port_of(pin, &bit);
    if (!pt || pt->pull == SL6806_REG_NONE)
        return;

    pm = sl6806_irq_save();

    v = REG(pt->base, pt->pull);
    if (enable)
        v |= (1u << bit);
    else
        v &= ~(1u << bit);
    REG(pt->base, pt->pull) = v;

    if (enable && pt->pull_dir != SL6806_REG_NONE) {
        v = REG(pt->base, pt->pull_dir);
        if (up)
            v |= (1u << bit);
        else
            v &= ~(1u << bit);
        REG(pt->base, pt->pull_dir) = v;
    }

    sl6806_irq_restore(pm);
}
