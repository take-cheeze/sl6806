#include "Display.h"
#include "font5x7.h"          /* cursor advance uses the glyph metrics */
#include "../sl6806_console.h"

#include <stdlib.h>
#include <stdio.h>

Display Screen;

/* Weak so a sketch can replace the message or halt instead. */
__attribute__((weak)) void sl6806_panel_unconfigured(void)
{
    sl6806_debug_print(
        "\r\n"
        "*** SL6806: no display driver ***\r\n"
        "    Drawing works - the framebuffer and all primitives are\r\n"
        "    implemented and tested - but there is no panel driver, so\r\n"
        "    nothing reaches the screen.\r\n"
        "    The panel controller, resolution and bus are all still unknown;\r\n"
        "    they live in lv_lcd_init (0x00D3E34C) in the stock firmware.\r\n"
        "    See docs/LCD.md, then fill in sl6806_panel_get() in your\r\n"
        "    variant.\r\n\r\n");
}

bool Display::begin()
{
    panel_ = sl6806_panel_get();
    if (!panel_) {
        sl6806_panel_unconfigured();
        return false;
    }

    size_t px = (size_t)panel_->width * (size_t)panel_->height;
    sl6806_color_t *buf = (sl6806_color_t *)malloc(px * sizeof(sl6806_color_t));

    if (!buf) {
        /* Say what was needed. On a chip where the whole payload window is
         * 64 KB, "out of memory" on its own is not actionable. */
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "SL6806: cannot allocate a %dx%d framebuffer (%u bytes).\r\n"
                 "        Use begin(buffer, w, h) with a smaller band.\r\n",
                 panel_->width, panel_->height,
                 (unsigned)(px * sizeof(sl6806_color_t)));
        sl6806_debug_print(msg);
        return false;
    }

    owned_ = buf;
    if (!begin(buf, panel_->width, panel_->height)) {
        free(owned_);
        owned_ = nullptr;
        return false;
    }
    return true;
}

bool Display::begin(sl6806_color_t *buffer, int16_t w, int16_t h)
{
    if (!buffer || w <= 0 || h <= 0)
        return false;

    panel_ = sl6806_panel_get();
    if (!panel_) {
        sl6806_panel_unconfigured();
        /* Still set the framebuffer up. Drawing into RAM is harmless and
         * lets a sketch be developed - and unit tested - before the panel
         * driver exists. It simply never becomes visible. */
        sl6806_fb_init(&fb_, buffer, w, h);
        return false;
    }

    if (panel_->begin && panel_->begin() != 0) {
        sl6806_debug_print("SL6806: panel initialisation failed\r\n");
        return false;
    }

    sl6806_fb_init(&fb_, buffer, w, h);
    cursor_x_ = cursor_y_ = 0;
    ready_ = true;
    return true;
}

void Display::end()
{
    if (owned_) {
        free(owned_);
        owned_ = nullptr;
    }
    ready_ = false;
    fb_.pixels = nullptr;
    fb_.width = fb_.height = 0;
}

void Display::display()
{
    display(0, 0, fb_.width, fb_.height);
}

void Display::display(int16_t x, int16_t y, int16_t w, int16_t h)
{
    if (!ready_ || !panel_ || !panel_->flush)
        return;

    /* Clip to the framebuffer so a caller cannot make us read past it. */
    if (x < 0) { w = (int16_t)(w + x); x = 0; }
    if (y < 0) { h = (int16_t)(h + y); y = 0; }
    if (x + w > fb_.width)  w = (int16_t)(fb_.width - x);
    if (y + h > fb_.height) h = (int16_t)(fb_.height - y);
    if (w <= 0 || h <= 0)
        return;

    if (w == fb_.width) {
        /* Contiguous run - hand the panel one block. */
        panel_->flush(x, y, w, h, &fb_.pixels[(int32_t)y * fb_.width + x]);
        return;
    }

    /* Sub-width region: rows are not contiguous, so push them one at a
     * time rather than copying into a temporary. */
    for (int16_t row = 0; row < h; row++) {
        panel_->flush(x, (int16_t)(y + row), w, 1,
                      &fb_.pixels[(int32_t)(y + row) * fb_.width + x]);
    }
}

void Display::displayAt(int16_t panel_x, int16_t panel_y)
{
    if (!ready_ || !panel_ || !panel_->flush)
        return;
    panel_->flush(panel_x, panel_y, fb_.width, fb_.height, fb_.pixels);
}

void Display::setBacklight(uint8_t level)
{
    if (panel_ && panel_->backlight)
        panel_->backlight(level);
}

size_t Display::write(uint8_t c)
{
    if (c == '\r')
        return 1;

    if (c == '\n') {
        cursor_x_ = 0;
        cursor_y_ = (int16_t)(cursor_y_ + (SL6806_FONT_HEIGHT + 1) * text_scale_);
        return 1;
    }

    /* Wrap at the right edge, like a terminal. */
    if (cursor_x_ + SL6806_FONT_ADVANCE * text_scale_ > fb_.width) {
        cursor_x_ = 0;
        cursor_y_ = (int16_t)(cursor_y_ + (SL6806_FONT_HEIGHT + 1) * text_scale_);
    }

    sl6806_fb_char(&fb_, cursor_x_, cursor_y_, (char)c, text_fg_, text_bg_,
                   text_scale_);
    cursor_x_ = (int16_t)(cursor_x_ + SL6806_FONT_ADVANCE * text_scale_);
    return 1;
}
