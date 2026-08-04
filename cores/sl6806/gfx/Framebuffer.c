#include "Framebuffer.h"
#include "font5x7.h"

void sl6806_fb_init(sl6806_fb_t *fb, sl6806_color_t *pixels,
                    int16_t w, int16_t h)
{
    fb->pixels = pixels;
    fb->width = w;
    fb->height = h;
}

void sl6806_fb_pixel(sl6806_fb_t *fb, int16_t x, int16_t y, sl6806_color_t c)
{
    if (x < 0 || y < 0 || x >= fb->width || y >= fb->height)
        return;
    fb->pixels[(int32_t)y * fb->width + x] = c;
}

sl6806_color_t sl6806_fb_get(const sl6806_fb_t *fb, int16_t x, int16_t y)
{
    if (x < 0 || y < 0 || x >= fb->width || y >= fb->height)
        return 0;
    return fb->pixels[(int32_t)y * fb->width + x];
}

void sl6806_fb_fill(sl6806_fb_t *fb, sl6806_color_t c)
{
    int32_t n = (int32_t)fb->width * fb->height;
    int32_t i;

    for (i = 0; i < n; i++)
        fb->pixels[i] = c;
}

void sl6806_fb_hline(sl6806_fb_t *fb, int16_t x, int16_t y, int16_t w,
                     sl6806_color_t c)
{
    int32_t row;
    int16_t i;

    if (w < 0) {           /* normalise so callers can pass either direction */
        x += w + 1;
        w = (int16_t)-w;
    }
    if (y < 0 || y >= fb->height || w == 0)
        return;

    if (x < 0) {
        w += x;
        x = 0;
    }
    if (x >= fb->width)
        return;
    if (x + w > fb->width)
        w = fb->width - x;
    if (w <= 0)
        return;

    row = (int32_t)y * fb->width + x;
    for (i = 0; i < w; i++)
        fb->pixels[row + i] = c;
}

void sl6806_fb_vline(sl6806_fb_t *fb, int16_t x, int16_t y, int16_t h,
                     sl6806_color_t c)
{
    int16_t i;

    if (h < 0) {
        y += h + 1;
        h = (int16_t)-h;
    }
    if (x < 0 || x >= fb->width || h == 0)
        return;

    if (y < 0) {
        h += y;
        y = 0;
    }
    if (y >= fb->height)
        return;
    if (y + h > fb->height)
        h = fb->height - y;

    for (i = 0; i < h; i++)
        fb->pixels[(int32_t)(y + i) * fb->width + x] = c;
}

void sl6806_fb_line(sl6806_fb_t *fb, int16_t x0, int16_t y0,
                    int16_t x1, int16_t y1, sl6806_color_t c)
{
    int16_t dx, dy, sx, sy;
    int32_t err, e2;

    if (y0 == y1) {
        sl6806_fb_hline(fb, x0 < x1 ? x0 : x1, y0,
                        (int16_t)((x1 > x0 ? x1 - x0 : x0 - x1) + 1), c);
        return;
    }
    if (x0 == x1) {
        sl6806_fb_vline(fb, x0, y0 < y1 ? y0 : y1,
                        (int16_t)((y1 > y0 ? y1 - y0 : y0 - y1) + 1), c);
        return;
    }

    /* Bresenham. Clipping happens per-pixel; these are short lines on a small
     * panel, so the simplicity is worth more than the saved compares. */
    dx = (int16_t)(x1 > x0 ? x1 - x0 : x0 - x1);
    dy = (int16_t)(y1 > y0 ? y1 - y0 : y0 - y1);
    sx = (int16_t)(x0 < x1 ? 1 : -1);
    sy = (int16_t)(y0 < y1 ? 1 : -1);
    err = (int32_t)dx - dy;

    for (;;) {
        sl6806_fb_pixel(fb, x0, y0, c);
        if (x0 == x1 && y0 == y1)
            break;
        e2 = err * 2;
        if (e2 > -dy) {
            err -= dy;
            x0 = (int16_t)(x0 + sx);
        }
        if (e2 < dx) {
            err += dx;
            y0 = (int16_t)(y0 + sy);
        }
    }
}

void sl6806_fb_rect(sl6806_fb_t *fb, int16_t x, int16_t y,
                    int16_t w, int16_t h, sl6806_color_t c)
{
    if (w <= 0 || h <= 0)
        return;
    sl6806_fb_hline(fb, x, y, w, c);
    sl6806_fb_hline(fb, x, (int16_t)(y + h - 1), w, c);
    sl6806_fb_vline(fb, x, y, h, c);
    sl6806_fb_vline(fb, (int16_t)(x + w - 1), y, h, c);
}

void sl6806_fb_fill_rect(sl6806_fb_t *fb, int16_t x, int16_t y,
                         int16_t w, int16_t h, sl6806_color_t c)
{
    int16_t i;

    if (w <= 0 || h <= 0)
        return;
    for (i = 0; i < h; i++)
        sl6806_fb_hline(fb, x, (int16_t)(y + i), w, c);
}

void sl6806_fb_circle(sl6806_fb_t *fb, int16_t cx, int16_t cy, int16_t r,
                      sl6806_color_t c)
{
    int16_t x = 0, y = r;
    int32_t d = 1 - r;

    if (r < 0)
        return;

    while (x <= y) {
        sl6806_fb_pixel(fb, (int16_t)(cx + x), (int16_t)(cy + y), c);
        sl6806_fb_pixel(fb, (int16_t)(cx - x), (int16_t)(cy + y), c);
        sl6806_fb_pixel(fb, (int16_t)(cx + x), (int16_t)(cy - y), c);
        sl6806_fb_pixel(fb, (int16_t)(cx - x), (int16_t)(cy - y), c);
        sl6806_fb_pixel(fb, (int16_t)(cx + y), (int16_t)(cy + x), c);
        sl6806_fb_pixel(fb, (int16_t)(cx - y), (int16_t)(cy + x), c);
        sl6806_fb_pixel(fb, (int16_t)(cx + y), (int16_t)(cy - x), c);
        sl6806_fb_pixel(fb, (int16_t)(cx - y), (int16_t)(cy - x), c);

        if (d < 0) {
            d += 2 * x + 3;
        } else {
            d += 2 * (x - y) + 5;
            y--;
        }
        x++;
    }
}

void sl6806_fb_fill_circle(sl6806_fb_t *fb, int16_t cx, int16_t cy, int16_t r,
                           sl6806_color_t c)
{
    int16_t x = 0, y = r;
    int32_t d = 1 - r;

    if (r < 0)
        return;

    while (x <= y) {
        /* Span-fill: each octant step gives two horizontal runs. */
        sl6806_fb_hline(fb, (int16_t)(cx - x), (int16_t)(cy + y),
                        (int16_t)(2 * x + 1), c);
        sl6806_fb_hline(fb, (int16_t)(cx - x), (int16_t)(cy - y),
                        (int16_t)(2 * x + 1), c);
        sl6806_fb_hline(fb, (int16_t)(cx - y), (int16_t)(cy + x),
                        (int16_t)(2 * y + 1), c);
        sl6806_fb_hline(fb, (int16_t)(cx - y), (int16_t)(cy - x),
                        (int16_t)(2 * y + 1), c);

        if (d < 0) {
            d += 2 * x + 3;
        } else {
            d += 2 * (x - y) + 5;
            y--;
        }
        x++;
    }
}

void sl6806_fb_blit(sl6806_fb_t *fb, int16_t x, int16_t y,
                    int16_t w, int16_t h, const sl6806_color_t *src)
{
    int16_t row, col;

    if (w <= 0 || h <= 0)
        return;

    for (row = 0; row < h; row++) {
        int16_t dy = (int16_t)(y + row);

        if (dy < 0 || dy >= fb->height)
            continue;
        for (col = 0; col < w; col++) {
            int16_t dx = (int16_t)(x + col);

            if (dx < 0 || dx >= fb->width)
                continue;
            fb->pixels[(int32_t)dy * fb->width + dx] =
                src[(int32_t)row * w + col];
        }
    }
}

void sl6806_fb_char(sl6806_fb_t *fb, int16_t x, int16_t y, char ch,
                    sl6806_color_t fg, uint32_t bg, uint8_t scale)
{
    const uint8_t *glyph;
    uint8_t col, row;

    if (scale == 0)
        scale = 1;

    if ((unsigned char)ch < SL6806_FONT_FIRST ||
        (unsigned char)ch > SL6806_FONT_LAST)
        ch = '?';

    glyph = &sl6806_font5x7[((unsigned char)ch - SL6806_FONT_FIRST)
                            * SL6806_FONT_WIDTH];

    for (col = 0; col < SL6806_FONT_ADVANCE; col++) {
        /* The 6th column is blank: it is the inter-character gap. */
        uint8_t bits = (col < SL6806_FONT_WIDTH) ? glyph[col] : 0;

        for (row = 0; row < SL6806_FONT_HEIGHT; row++) {
            int on = (bits >> row) & 1;

            if (!on && bg == SL6806_NO_BG)
                continue;

            {
                sl6806_color_t c = on ? fg : (sl6806_color_t)bg;
                int16_t px = (int16_t)(x + col * scale);
                int16_t py = (int16_t)(y + row * scale);

                if (scale == 1)
                    sl6806_fb_pixel(fb, px, py, c);
                else
                    sl6806_fb_fill_rect(fb, px, py, scale, scale, c);
            }
        }
    }
}

void sl6806_fb_text(sl6806_fb_t *fb, int16_t x, int16_t y, const char *s,
                    sl6806_color_t fg, uint32_t bg, uint8_t scale)
{
    int16_t cx = x;

    if (scale == 0)
        scale = 1;

    for (; *s; s++) {
        if (*s == '\n') {
            cx = x;
            y = (int16_t)(y + (SL6806_FONT_HEIGHT + 1) * scale);
            continue;
        }
        sl6806_fb_char(fb, cx, y, *s, fg, bg, scale);
        cx = (int16_t)(cx + SL6806_FONT_ADVANCE * scale);
    }
}

int16_t sl6806_fb_text_width(const char *s, uint8_t scale)
{
    int16_t run = 0, best = 0;

    if (scale == 0)
        scale = 1;

    for (; *s; s++) {
        if (*s == '\n') {
            run = 0;
            continue;
        }
        run = (int16_t)(run + SL6806_FONT_ADVANCE * scale);
        if (run > best)
            best = run;
    }
    return best;
}
