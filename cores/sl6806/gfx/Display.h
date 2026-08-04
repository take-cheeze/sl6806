/*
 * Display.h - Arduino-style display object for SL6806.
 *
 * Draws into a RAM framebuffer, then pushes it to the panel with display().
 * Inherits Print, so Screen.print(...) works exactly like Serial.print(...).
 *
 * MEMORY - READ THIS BEFORE CHOOSING A RESOLUTION
 * An RGB565 framebuffer costs width * height * 2 bytes. A 240x135 panel is
 * ~65 KB, which does not fit in payload mode at all: the whole payload window
 * is 64 KB, leaving roughly 38 KB of heap. Firmware mode has ~190 KB and can
 * hold one comfortably.
 *
 * So there are three ways to use this:
 *
 *   begin()                      - allocate a full-screen buffer from the
 *                                  heap. Fails cleanly if it does not fit.
 *   begin(buf, w, h)             - use a buffer you supply. Pass a smaller
 *                                  w/h than the panel to render a band and
 *                                  push it repeatedly - the usual trick for
 *                                  a device with less RAM than screen.
 *   panel()->flush(...)          - bypass the framebuffer entirely.
 *
 * None of this draws anything until a variant supplies a panel driver. See
 * Panel.h.
 */
#ifndef SL6806_DISPLAY_H
#define SL6806_DISPLAY_H

#include "Framebuffer.h"
#include "Panel.h"
#include "../Print.h"

class Display : public Print {
public:
    /* Allocate a full-screen framebuffer from the heap. Returns false if
     * there is no panel driver or the allocation does not fit. */
    bool begin();

    /* Use a caller-supplied framebuffer, which may be smaller than the
     * panel. Never allocates. */
    bool begin(sl6806_color_t *buffer, int16_t w, int16_t h);

    void end();

    /* True once a panel driver exists AND begin() succeeded. */
    bool available() const { return ready_; }

    /* True if the board has a panel driver at all - false on every SL6806
     * board known today. Check this to degrade gracefully. */
    static bool panelAvailable() { return sl6806_panel_get() != nullptr; }

    const sl6806_panel_t *panel() const { return panel_; }

    int16_t width()  const { return fb_.width; }
    int16_t height() const { return fb_.height; }

    sl6806_fb_t *framebuffer() { return &fb_; }

    /* ---- drawing ---- */
    void fill(sl6806_color_t c)   { sl6806_fb_fill(&fb_, c); }
    void clear()                  { fill(SL6806_BLACK); cursor_x_ = cursor_y_ = 0; }

    void drawPixel(int16_t x, int16_t y, sl6806_color_t c)
        { sl6806_fb_pixel(&fb_, x, y, c); }
    sl6806_color_t readPixel(int16_t x, int16_t y) const
        { return sl6806_fb_get(&fb_, x, y); }

    void drawFastHLine(int16_t x, int16_t y, int16_t w, sl6806_color_t c)
        { sl6806_fb_hline(&fb_, x, y, w, c); }
    void drawFastVLine(int16_t x, int16_t y, int16_t h, sl6806_color_t c)
        { sl6806_fb_vline(&fb_, x, y, h, c); }
    void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                  sl6806_color_t c)
        { sl6806_fb_line(&fb_, x0, y0, x1, y1, c); }

    void drawRect(int16_t x, int16_t y, int16_t w, int16_t h, sl6806_color_t c)
        { sl6806_fb_rect(&fb_, x, y, w, h, c); }
    void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, sl6806_color_t c)
        { sl6806_fb_fill_rect(&fb_, x, y, w, h, c); }

    void drawCircle(int16_t x, int16_t y, int16_t r, sl6806_color_t c)
        { sl6806_fb_circle(&fb_, x, y, r, c); }
    void fillCircle(int16_t x, int16_t y, int16_t r, sl6806_color_t c)
        { sl6806_fb_fill_circle(&fb_, x, y, r, c); }

    void drawBitmap(int16_t x, int16_t y, int16_t w, int16_t h,
                    const sl6806_color_t *src)
        { sl6806_fb_blit(&fb_, x, y, w, h, src); }

    void drawChar(int16_t x, int16_t y, char ch, sl6806_color_t fg,
                  uint32_t bg, uint8_t scale = 1)
        { sl6806_fb_char(&fb_, x, y, ch, fg, bg, scale); }
    void drawText(int16_t x, int16_t y, const char *s, sl6806_color_t fg,
                  uint32_t bg = SL6806_NO_BG, uint8_t scale = 1)
        { sl6806_fb_text(&fb_, x, y, s, fg, bg, scale); }

    /* ---- Print interface: Screen.print(...) like Serial ---- */
    void setCursor(int16_t x, int16_t y) { cursor_x_ = x; cursor_y_ = y; }
    int16_t getCursorX() const { return cursor_x_; }
    int16_t getCursorY() const { return cursor_y_; }
    void setTextColor(sl6806_color_t fg, uint32_t bg = SL6806_NO_BG)
        { text_fg_ = fg; text_bg_ = bg; }
    void setTextSize(uint8_t s) { text_scale_ = s ? s : 1; }

    virtual size_t write(uint8_t c);
    using Print::write;

    /* ---- output ---- */
    void display();   /* push the whole framebuffer */
    void display(int16_t x, int16_t y, int16_t w, int16_t h);

    /* Push the framebuffer to a different origin on the panel. Use with a
     * band-sized buffer to cover a screen larger than RAM allows. */
    void displayAt(int16_t panel_x, int16_t panel_y);

    void setBacklight(uint8_t level);

private:
    sl6806_fb_t fb_ = { nullptr, 0, 0 };
    const sl6806_panel_t *panel_ = nullptr;
    sl6806_color_t *owned_ = nullptr;   /* heap buffer to free in end() */
    bool ready_ = false;

    int16_t cursor_x_ = 0, cursor_y_ = 0;
    sl6806_color_t text_fg_ = SL6806_WHITE;
    uint32_t text_bg_ = SL6806_NO_BG;
    uint8_t text_scale_ = 1;
};

extern Display Screen;

#endif /* SL6806_DISPLAY_H */
