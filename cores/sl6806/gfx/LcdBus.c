/*
 * LcdBus.c - command streams and MIPI DCS windowing for SL6806 panels.
 *
 * Everything here sits above the byte-level transport (Panel.h's
 * sl6806_lcd_bus_t) and below the framebuffer. It is the part that is the
 * same for every DCS panel, so a variant only has to supply geometry, the
 * recovered command tables, and the transport itself.
 *
 * It is deliberately free of hardware access: the only way it touches the
 * panel is through the registered bus. That makes it testable on the host
 * against a recording bus - see tests/host/test_panel.c, which checks the
 * P20's init sequence byte for byte and the window arithmetic against the
 * offsets the vendor firmware uses.
 */

#include "Panel.h"
#include "../wiring_time.h"

static const sl6806_lcd_bus_t *g_bus;

int sl6806_lcd_bus_register(const sl6806_lcd_bus_t *bus)
{
    if (bus && (!bus->command || !bus->pixels))
        return -1;
    g_bus = bus;
    return 0;
}

const sl6806_lcd_bus_t *sl6806_lcd_bus(void)
{
    return g_bus;
}

int sl6806_lcd_run(const uint8_t *seq)
{
    const sl6806_lcd_bus_t *bus = g_bus;

    if (!bus || !seq)
        return -1;

    for (;;) {
        uint8_t op = *seq++;

        if (op == SL6806_LCD_END)
            return 0;

        if (op == SL6806_LCD_CMD) {
            uint8_t cmd = seq[0];
            uint8_t n   = seq[1];
            seq += 2;
            bus->command(cmd, n ? seq : 0, n);
            seq += n;
            continue;
        }

        if (op == SL6806_LCD_DELAY) {
            uint32_t ms = (uint32_t)seq[0] | ((uint32_t)seq[1] << 8);
            seq += 2;
            delay(ms);
            continue;
        }

        /* An unknown opcode means the stream is misaligned, and carrying on
         * would push arbitrary bytes at the controller. Stop instead. */
        return -1;
    }
}

int sl6806_dcs_begin(const sl6806_dcs_panel_t *p)
{
    const sl6806_lcd_bus_t *bus = g_bus;

    if (!bus || !p || !p->init)
        return -1;

    if (bus->reset) {
        bus->reset(1);
        delay(p->reset_high_ms);
        bus->reset(0);
        delay(p->reset_low_ms);
        bus->reset(1);
        delay(p->reset_settle_ms);
    }

    return sl6806_lcd_run(p->init);
}

/* CASET/RASET take big-endian 16-bit start and end columns, inclusive. */
static void set_window(const sl6806_dcs_panel_t *p, const sl6806_lcd_bus_t *bus,
                       int16_t x, int16_t y, int16_t w, int16_t h)
{
    uint16_t x0 = (uint16_t)(x + p->x_offset);
    uint16_t y0 = (uint16_t)(y + p->y_offset);
    uint16_t x1 = (uint16_t)(x0 + w - 1);
    uint16_t y1 = (uint16_t)(y0 + h - 1);
    uint8_t a[4];

    a[0] = (uint8_t)(x0 >> 8); a[1] = (uint8_t)x0;
    a[2] = (uint8_t)(x1 >> 8); a[3] = (uint8_t)x1;
    bus->command(p->caset, a, 4);

    a[0] = (uint8_t)(y0 >> 8); a[1] = (uint8_t)y0;
    a[2] = (uint8_t)(y1 >> 8); a[3] = (uint8_t)y1;
    bus->command(p->raset, a, 4);
}

void sl6806_dcs_flush(const sl6806_dcs_panel_t *p,
                      int16_t x, int16_t y, int16_t w, int16_t h,
                      const sl6806_color_t *src)
{
    const sl6806_lcd_bus_t *bus = g_bus;
    int16_t skip_x = 0, skip_y = 0;
    int16_t src_stride;

    if (!bus || !p || !src || w <= 0 || h <= 0)
        return;

    src_stride = w;

    /* Clip to the visible area. Anything else would address controller RAM
     * outside the panel, which on some controllers wraps rather than being
     * ignored. */
    if (x < 0) { skip_x = -x; w += x; x = 0; }
    if (y < 0) { skip_y = -y; h += y; y = 0; }
    if (x + w > p->width)  w = (int16_t)(p->width  - x);
    if (y + h > p->height) h = (int16_t)(p->height - y);
    if (w <= 0 || h <= 0)
        return;

    set_window(p, bus, x, y, w, h);
    bus->command(p->ramwr, 0, 0);

    src += (int32_t)skip_y * src_stride + skip_x;

    if (w == src_stride) {
        /* Contiguous: one burst for the whole rectangle. */
        bus->pixels(src, (uint32_t)w * (uint32_t)h);
    } else {
        for (int16_t row = 0; row < h; row++) {
            bus->pixels(src, (uint32_t)w);
            src += src_stride;
        }
    }
}

int sl6806_dcs_sleep(const sl6806_dcs_panel_t *p)
{
    return (p && p->sleep) ? sl6806_lcd_run(p->sleep) : -1;
}

int sl6806_dcs_wake(const sl6806_dcs_panel_t *p)
{
    return (p && p->wake) ? sl6806_lcd_run(p->wake) : -1;
}

int sl6806_dcs_display(const sl6806_dcs_panel_t *p, int on)
{
    if (!p)
        return -1;
    return sl6806_lcd_run(on ? p->display_on : p->display_off);
}
