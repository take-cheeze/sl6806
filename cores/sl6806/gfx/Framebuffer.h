/*
 * Framebuffer.h - RGB565 software framebuffer and drawing primitives.
 *
 * This half of the display stack is hardware-independent: it renders into a
 * plain array of 16-bit pixels and knows nothing about the panel. That makes
 * it fully testable on the host (see tests/host/test_gfx.c), and it is why it
 * is written and verified even though the SL6806's panel is still unknown.
 *
 * Getting those pixels onto glass is the other half - see Panel.h.
 *
 * Every primitive clips against the framebuffer bounds. Passing coordinates
 * outside it is legal and draws nothing, so callers never need bounds checks.
 */
#ifndef SL6806_FRAMEBUFFER_H
#define SL6806_FRAMEBUFFER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* RGB565, the format essentially every small TFT expects. */
typedef uint16_t sl6806_color_t;

#define SL6806_RGB(r, g, b) ((sl6806_color_t)                    \
    ((((uint16_t)(r) & 0xF8) << 8) |                             \
     (((uint16_t)(g) & 0xFC) << 3) |                             \
     (((uint16_t)(b) & 0xF8) >> 3)))

#define SL6806_BLACK   SL6806_RGB(0, 0, 0)
#define SL6806_WHITE   SL6806_RGB(255, 255, 255)
#define SL6806_RED     SL6806_RGB(255, 0, 0)
#define SL6806_GREEN   SL6806_RGB(0, 255, 0)
#define SL6806_BLUE    SL6806_RGB(0, 0, 255)
#define SL6806_YELLOW  SL6806_RGB(255, 255, 0)
#define SL6806_CYAN    SL6806_RGB(0, 255, 255)
#define SL6806_MAGENTA SL6806_RGB(255, 0, 255)

/* Transparent background for text. Not a representable RGB565 value, so it
 * cannot collide with a real colour. */
#define SL6806_NO_BG   0xFFFFFFFFu

typedef struct {
    sl6806_color_t *pixels;
    int16_t width;
    int16_t height;
} sl6806_fb_t;

void sl6806_fb_init(sl6806_fb_t *fb, sl6806_color_t *pixels,
                    int16_t w, int16_t h);

void sl6806_fb_fill(sl6806_fb_t *fb, sl6806_color_t c);
void sl6806_fb_pixel(sl6806_fb_t *fb, int16_t x, int16_t y, sl6806_color_t c);

/* Returns 0 for out-of-bounds reads. */
sl6806_color_t sl6806_fb_get(const sl6806_fb_t *fb, int16_t x, int16_t y);

void sl6806_fb_hline(sl6806_fb_t *fb, int16_t x, int16_t y, int16_t w,
                     sl6806_color_t c);
void sl6806_fb_vline(sl6806_fb_t *fb, int16_t x, int16_t y, int16_t h,
                     sl6806_color_t c);
void sl6806_fb_line(sl6806_fb_t *fb, int16_t x0, int16_t y0,
                    int16_t x1, int16_t y1, sl6806_color_t c);
void sl6806_fb_rect(sl6806_fb_t *fb, int16_t x, int16_t y,
                    int16_t w, int16_t h, sl6806_color_t c);
void sl6806_fb_fill_rect(sl6806_fb_t *fb, int16_t x, int16_t y,
                         int16_t w, int16_t h, sl6806_color_t c);
void sl6806_fb_circle(sl6806_fb_t *fb, int16_t cx, int16_t cy, int16_t r,
                      sl6806_color_t c);
void sl6806_fb_fill_circle(sl6806_fb_t *fb, int16_t cx, int16_t cy, int16_t r,
                           sl6806_color_t c);

/* Copy a rectangular RGB565 image in. `src` is row-major, `w` wide. */
void sl6806_fb_blit(sl6806_fb_t *fb, int16_t x, int16_t y,
                    int16_t w, int16_t h, const sl6806_color_t *src);

/* Text. `bg` may be SL6806_NO_BG to leave the background untouched. `scale`
 * multiplies each font pixel into a scale x scale block. */
void sl6806_fb_char(sl6806_fb_t *fb, int16_t x, int16_t y, char ch,
                    sl6806_color_t fg, uint32_t bg, uint8_t scale);
void sl6806_fb_text(sl6806_fb_t *fb, int16_t x, int16_t y, const char *s,
                    sl6806_color_t fg, uint32_t bg, uint8_t scale);

/* Width in pixels that sl6806_fb_text would occupy. */
int16_t sl6806_fb_text_width(const char *s, uint8_t scale);

#ifdef __cplusplus
}
#endif

#endif /* SL6806_FRAMEBUFFER_H */
