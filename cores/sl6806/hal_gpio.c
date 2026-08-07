/*
 * hal_gpio.c - GPIO driver.
 *
 * Complete except for one of two things the variant supplies: a register
 * table, or a vendor back end plus the pin ids to use with it. See
 * hal_gpio.h.
 *
 * When both exist, the vendor back end wins for any pin that has an id, and
 * the register table serves the rest - so a board can bring pins up in two
 * stages rather than all at once.
 */

#include "sl6806.h"
#include "hal_gpio.h"
#include "sl6806_console.h"

#define REG(base, off) (*(volatile uint32_t *)((base) + (off)))

static const sl6806_gpio_vendor_t *g_vendor;

int sl6806_gpio_vendor_register(const sl6806_gpio_vendor_t *v)
{
    if (v && !v->write)
        return -1;
    g_vendor = v;
    return 0;
}

/* The vendor id for `pin`, or SL6806_GPIO_ID_NONE if this build cannot
 * reach that pin through the vendor back end. */
static uint32_t vendor_id(uint8_t pin)
{
    if (!g_vendor || pin >= sl6806_nvendor_pins)
        return SL6806_GPIO_ID_NONE;
    return sl6806_vendor_pin_map[pin];
}

static int vendor_usable(void)
{
    uint8_t i;

    if (!g_vendor)
        return 0;
    for (i = 0; i < sl6806_nvendor_pins; i++)
        if (sl6806_vendor_pin_map[i] != SL6806_GPIO_ID_NONE)
            return 1;
    return 0;
}

static int registers_usable(void)
{
    return sl6806_gpio_nports != 0 && sl6806_npins != 0;
}

int sl6806_gpio_available(void)
{
    return registers_usable() || vendor_usable();
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
        "    "); sl6806_debug_print(what); sl6806_debug_print(" was called, but this build cannot reach a\r\n"
        "    pin. The SL6806 pad controller has not been reverse engineered,\r\n"
        "    so the framework cannot guess it. Two ways out:\r\n"
        "      - fill in sl6806_gpio_ports[] in your variant and define\r\n"
        "        SL6806_GPIO_CONFIGURED; or\r\n"
        "      - install a vendor back end with\r\n"
        "        sl6806_gpio_vendor_register(), which needs no registers.\r\n"
        "    See cores/sl6806/hal_gpio.h.\r\n"
        "    (reported once; further GPIO calls are ignored)\r\n\r\n");
}

/* Resolve a pin to its port descriptor, or NULL. */
static const sl6806_gpio_port_t *port_of(uint8_t pin, uint8_t *bit)
{
    const sl6806_pin_t *p;

    if (!registers_usable()) {
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
    uint32_t id = vendor_id(pin);

    if (id != SL6806_GPIO_ID_NONE) {
        /* A back end that can change the direction on its own does so. One
         * that cannot leaves the pad as whatever configured it, which is the
         * honest choice - guessing a field of the vendor configuration word
         * would reconfigure the pad into something arbitrary. */
        if (g_vendor->set_dir)
            g_vendor->set_dir(id, output);
        return;
    }

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
    uint32_t id = vendor_id(pin);

    if (id != SL6806_GPIO_ID_NONE) {
        g_vendor->write(id, value ? 1u : 0u);
        return;
    }

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
    uint32_t id = vendor_id(pin);

    if (id != SL6806_GPIO_ID_NONE) {
        if (!g_vendor->read) {
            sl6806_gpio_unconfigured("digitalRead()");
            return 0;
        }
        return (int)(g_vendor->read(id) & 1u);
    }

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
    uint32_t id = vendor_id(pin);

    if (id != SL6806_GPIO_ID_NONE) {
        /* Same reasoning as set_dir(): which bits of the vendor
         * configuration word are the pull is not known. */
        (void)enable;
        (void)up;
        return;
    }

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
