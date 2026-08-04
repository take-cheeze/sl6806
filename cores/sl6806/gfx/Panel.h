/*
 * Panel.h - the hardware half of the display stack.
 *
 * =====================================================================
 *  THE PANEL DRIVER IS THE MISSING PIECE
 * =====================================================================
 * Framebuffer.c is complete and tested. What is not known is how to get
 * those pixels onto the glass, because that needs facts nobody has
 * recovered yet:
 *
 *   - the panel controller (ST7789? ILI9341? something vendor-specific)
 *   - the resolution and colour depth
 *   - the bus: SPI, parallel, or an RGB interface with a framebuffer in RAM
 *   - which pins carry reset / chip-select / data-command / backlight
 *   - the controller's initialisation sequence
 *
 * All of it lives in the stock firmware's `lv_lcd_init` at 0x00D3E34C - the
 * vendor's LVGL porting layer - and reading it needs a real 4 MiB dump. See
 * docs/LCD.md for how to extract it.
 *
 * This header is the seam. Fill in one of these structs and the entire
 * drawing stack, the Display class and the example sketches start working
 * unchanged. Until a variant supplies one, sl6806_panel_get() returns NULL
 * and Display::begin() reports why instead of silently drawing nowhere.
 */
#ifndef SL6806_PANEL_H
#define SL6806_PANEL_H

#include <stdint.h>
#include "Framebuffer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;       /* shown at startup, e.g. "ST7789 240x240" */
    int16_t width;
    int16_t height;

    /* Power up and initialise the controller. Return 0 on success.
     * Called once from Display::begin(). */
    int (*begin)(void);

    /* Push a rectangle of RGB565 pixels. `src` is row-major and `w` wide -
     * it is a window into the caller's framebuffer, not the whole thing.
     * This is the only function that must talk to hardware. */
    void (*flush)(int16_t x, int16_t y, int16_t w, int16_t h,
                  const sl6806_color_t *src);

    /* Optional; NULL if the board has no controllable backlight. */
    void (*backlight)(uint8_t level);
} sl6806_panel_t;

/*
 * Supplied by the variant. Returns NULL when the board has no panel driver,
 * which is the current state for every known SL6806 board.
 */
const sl6806_panel_t *sl6806_panel_get(void);

#ifdef __cplusplus
}
#endif

#endif /* SL6806_PANEL_H */
