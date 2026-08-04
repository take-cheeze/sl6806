/*
 * panel.c - display driver for the P20 Player.
 *
 * There is not one yet. The board definitely has a colour LCD - the stock
 * firmware drives it through LVGL 8.x - but the controller, resolution, bus
 * and pins are all unknown, so there is nothing honest to put here.
 *
 * Returning NULL is what makes Display::begin() explain the situation instead
 * of drawing into a void. Everything above this file - framebuffer,
 * primitives, text, the Display class - is finished and tested; this single
 * function is the whole remaining gap.
 *
 * WHEN YOU HAVE THE FACTS (see docs/LCD.md), it looks like this:
 *
 *   static int panel_begin(void)
 *   {
 *       // reset pulse, then the controller's init sequence
 *       return 0;
 *   }
 *
 *   static void panel_flush(int16_t x, int16_t y, int16_t w, int16_t h,
 *                           const sl6806_color_t *src)
 *   {
 *       // set the column/row address window, then stream w*h pixels
 *   }
 *
 *   static const sl6806_panel_t p20_panel = {
 *       .name   = "ST7789 240x135",
 *       .width  = 240,
 *       .height = 135,
 *       .begin  = panel_begin,
 *       .flush  = panel_flush,
 *       .backlight = NULL,
 *   };
 *
 *   const sl6806_panel_t *sl6806_panel_get(void) { return &p20_panel; }
 *
 * Note that panel_flush needs a working SPI or parallel bus, which in turn
 * needs the GPIO/peripheral registers - so hal_gpio.h's discovery step comes
 * first. If the panel turns out to be RGB-interfaced with a framebuffer the
 * hardware scans out of RAM, flush() may reduce to a memcpy, and the
 * framebuffer address is then the only thing you need.
 */

#include "gfx/Panel.h"

const sl6806_panel_t *sl6806_panel_get(void)
{
    return 0;   /* no panel driver - see above */
}
