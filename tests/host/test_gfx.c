/*
 * test_gfx.c - native tests for the framebuffer primitives.
 *
 * The drawing code is hardware-independent, so all of it can be verified here
 * rather than guessed at on a panel nobody has identified yet. The clipping
 * cases matter most: on a device, a primitive that writes one pixel outside
 * the buffer corrupts whatever follows it in RAM and shows up as an unrelated
 * bug much later.
 *
 * Also writes gfx_demo.ppm so the output can be looked at, not just asserted
 * on - the font in particular is easier to check by eye.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "gfx/Framebuffer.h"

static int failures;
static int checks;

#define CHECK(cond, ...) do {                                   \
        checks++;                                               \
        if (!(cond)) {                                          \
            failures++;                                         \
            printf("  FAIL %s:%d: ", __func__, __LINE__);       \
            printf(__VA_ARGS__);                                \
            printf("\n");                                       \
        }                                                       \
    } while (0)

#define W 64
#define H 48

static sl6806_color_t pixels[W * H];
static sl6806_fb_t fb;

/* Canary pattern around the buffer would be ideal, but ASan already catches
 * out-of-bounds writes, so a plain buffer plus ASan is the stronger check. */
static void reset(void)
{
    sl6806_fb_init(&fb, pixels, W, H);
    sl6806_fb_fill(&fb, SL6806_BLACK);
}

static int count_non_black(void)
{
    int n = 0;
    for (int i = 0; i < W * H; i++)
        if (pixels[i] != SL6806_BLACK)
            n++;
    return n;
}

static void test_color_macro(void)
{
    CHECK(SL6806_RGB(0, 0, 0) == 0x0000, "black");
    CHECK(SL6806_RGB(255, 255, 255) == 0xFFFF, "white got 0x%04X",
          SL6806_RGB(255, 255, 255));
    CHECK(SL6806_RGB(255, 0, 0) == 0xF800, "red got 0x%04X", SL6806_RGB(255, 0, 0));
    CHECK(SL6806_RGB(0, 255, 0) == 0x07E0, "green got 0x%04X", SL6806_RGB(0, 255, 0));
    CHECK(SL6806_RGB(0, 0, 255) == 0x001F, "blue got 0x%04X", SL6806_RGB(0, 0, 255));
}

static void test_pixel_and_fill(void)
{
    reset();
    CHECK(count_non_black() == 0, "fill(black) should clear");

    sl6806_fb_fill(&fb, SL6806_WHITE);
    CHECK(count_non_black() == W * H, "fill(white) should set every pixel");

    reset();
    sl6806_fb_pixel(&fb, 10, 20, SL6806_RED);
    CHECK(sl6806_fb_get(&fb, 10, 20) == SL6806_RED, "pixel not set");
    CHECK(count_non_black() == 1, "exactly one pixel should change");
}

/* Out-of-bounds work must be silently dropped, never written. */
static void test_pixel_clipping(void)
{
    reset();
    sl6806_fb_pixel(&fb, -1, 0, SL6806_RED);
    sl6806_fb_pixel(&fb, 0, -1, SL6806_RED);
    sl6806_fb_pixel(&fb, W, 0, SL6806_RED);
    sl6806_fb_pixel(&fb, 0, H, SL6806_RED);
    sl6806_fb_pixel(&fb, -100, -100, SL6806_RED);
    sl6806_fb_pixel(&fb, 30000, 30000, SL6806_RED);
    CHECK(count_non_black() == 0, "out-of-bounds pixels must not be written");

    CHECK(sl6806_fb_get(&fb, -1, -1) == 0, "out-of-bounds read should be 0");
    CHECK(sl6806_fb_get(&fb, W, H) == 0, "out-of-bounds read should be 0");
}

static void test_hline_clipping(void)
{
    reset();
    sl6806_fb_hline(&fb, -10, 5, 20, SL6806_RED);   /* straddles left edge */
    CHECK(count_non_black() == 10, "expected 10 visible px, got %d",
          count_non_black());
    CHECK(sl6806_fb_get(&fb, 0, 5) == SL6806_RED, "x=0 should be drawn");
    CHECK(sl6806_fb_get(&fb, 9, 5) == SL6806_RED, "x=9 should be drawn");
    CHECK(sl6806_fb_get(&fb, 10, 5) == SL6806_BLACK, "x=10 should not be");

    reset();
    sl6806_fb_hline(&fb, W - 5, 5, 20, SL6806_RED); /* straddles right edge */
    CHECK(count_non_black() == 5, "expected 5 visible px, got %d",
          count_non_black());

    reset();
    sl6806_fb_hline(&fb, 0, -1, 10, SL6806_RED);
    sl6806_fb_hline(&fb, 0, H, 10, SL6806_RED);
    CHECK(count_non_black() == 0, "off-buffer rows must draw nothing");
}

static void test_vline_clipping(void)
{
    reset();
    sl6806_fb_vline(&fb, 5, -10, 20, SL6806_GREEN);
    CHECK(count_non_black() == 10, "expected 10 visible px, got %d",
          count_non_black());

    reset();
    sl6806_fb_vline(&fb, 5, H - 3, 20, SL6806_GREEN);
    CHECK(count_non_black() == 3, "expected 3 visible px, got %d",
          count_non_black());

    reset();
    sl6806_fb_vline(&fb, -1, 0, 10, SL6806_GREEN);
    sl6806_fb_vline(&fb, W, 0, 10, SL6806_GREEN);
    CHECK(count_non_black() == 0, "off-buffer columns must draw nothing");
}

static void test_rect(void)
{
    reset();
    sl6806_fb_rect(&fb, 2, 3, 10, 8, SL6806_BLUE);
    /* Outline only: 2*(w+h) - 4 corners counted twice. */
    CHECK(count_non_black() == 2 * 10 + 2 * 8 - 4,
          "outline pixel count wrong: %d", count_non_black());
    CHECK(sl6806_fb_get(&fb, 2, 3) == SL6806_BLUE, "top-left corner");
    CHECK(sl6806_fb_get(&fb, 11, 10) == SL6806_BLUE, "bottom-right corner");
    CHECK(sl6806_fb_get(&fb, 5, 5) == SL6806_BLACK, "interior must be empty");

    reset();
    sl6806_fb_fill_rect(&fb, 2, 3, 10, 8, SL6806_BLUE);
    CHECK(count_non_black() == 80, "filled rect should be 80 px, got %d",
          count_non_black());
    CHECK(sl6806_fb_get(&fb, 5, 5) == SL6806_BLUE, "interior must be filled");

    reset();
    sl6806_fb_fill_rect(&fb, -5, -5, 10, 10, SL6806_BLUE);
    CHECK(count_non_black() == 25, "clipped fill should be 25 px, got %d",
          count_non_black());

    reset();
    sl6806_fb_fill_rect(&fb, 0, 0, 0, 10, SL6806_BLUE);
    sl6806_fb_fill_rect(&fb, 0, 0, 10, 0, SL6806_BLUE);
    sl6806_fb_fill_rect(&fb, 0, 0, -5, -5, SL6806_BLUE);
    CHECK(count_non_black() == 0, "degenerate rects must draw nothing");
}

static void test_line(void)
{
    reset();
    sl6806_fb_line(&fb, 0, 0, 9, 0, SL6806_WHITE);
    CHECK(count_non_black() == 10, "horizontal line: %d", count_non_black());

    reset();
    sl6806_fb_line(&fb, 0, 0, 0, 9, SL6806_WHITE);
    CHECK(count_non_black() == 10, "vertical line: %d", count_non_black());

    reset();
    sl6806_fb_line(&fb, 0, 0, 9, 9, SL6806_WHITE);
    CHECK(count_non_black() == 10, "diagonal: %d", count_non_black());
    for (int i = 0; i < 10; i++)
        CHECK(sl6806_fb_get(&fb, i, i) == SL6806_WHITE, "diagonal px %d", i);

    /* A line entirely off-buffer must not write anything. */
    reset();
    sl6806_fb_line(&fb, -50, -50, -10, -10, SL6806_WHITE);
    CHECK(count_non_black() == 0, "fully clipped line drew something");

    /* Reversed endpoints must give the same result. */
    reset();
    sl6806_fb_line(&fb, 3, 2, 20, 15, SL6806_WHITE);
    int forward = count_non_black();
    reset();
    sl6806_fb_line(&fb, 20, 15, 3, 2, SL6806_WHITE);
    CHECK(count_non_black() == forward, "line should be direction-agnostic");
}

static void test_circle(void)
{
    reset();
    sl6806_fb_circle(&fb, 20, 20, 10, SL6806_YELLOW);
    CHECK(count_non_black() > 0, "circle drew nothing");
    /* Cardinal points must be on the outline. */
    CHECK(sl6806_fb_get(&fb, 30, 20) == SL6806_YELLOW, "right of circle");
    CHECK(sl6806_fb_get(&fb, 10, 20) == SL6806_YELLOW, "left of circle");
    CHECK(sl6806_fb_get(&fb, 20, 30) == SL6806_YELLOW, "below circle");
    CHECK(sl6806_fb_get(&fb, 20, 10) == SL6806_YELLOW, "above circle");
    CHECK(sl6806_fb_get(&fb, 20, 20) == SL6806_BLACK, "centre must be hollow");

    reset();
    sl6806_fb_fill_circle(&fb, 20, 20, 10, SL6806_YELLOW);
    CHECK(sl6806_fb_get(&fb, 20, 20) == SL6806_YELLOW, "centre must be filled");
    /* Area of a radius-10 disc is ~314; allow for rasterisation slop. */
    int n = count_non_black();
    CHECK(n > 280 && n < 360, "filled circle area %d outside expected range", n);

    /* Clipping at a corner must not fault (ASan is the real assertion). */
    reset();
    sl6806_fb_fill_circle(&fb, 0, 0, 15, SL6806_YELLOW);
    sl6806_fb_fill_circle(&fb, W, H, 15, SL6806_YELLOW);
    sl6806_fb_circle(&fb, -20, -20, 5, SL6806_YELLOW);
}

static void test_blit(void)
{
    sl6806_color_t img[4 * 3];

    for (int i = 0; i < 12; i++)
        img[i] = (sl6806_color_t)(i + 1);

    reset();
    sl6806_fb_blit(&fb, 5, 5, 4, 3, img);
    CHECK(sl6806_fb_get(&fb, 5, 5) == 1, "blit origin");
    CHECK(sl6806_fb_get(&fb, 8, 5) == 4, "blit row 0 end");
    CHECK(sl6806_fb_get(&fb, 5, 7) == 9, "blit row 2 start");
    CHECK(count_non_black() == 12, "blit should touch 12 px, got %d",
          count_non_black());

    /* Partially off the left/top edge. */
    reset();
    sl6806_fb_blit(&fb, -2, -1, 4, 3, img);
    CHECK(count_non_black() == 4, "clipped blit should touch 4 px, got %d",
          count_non_black());

    /* Entirely off-buffer. */
    reset();
    sl6806_fb_blit(&fb, -10, -10, 4, 3, img);
    sl6806_fb_blit(&fb, W + 5, H + 5, 4, 3, img);
    CHECK(count_non_black() == 0, "off-buffer blit drew something");
}

static void test_text(void)
{
    reset();
    sl6806_fb_text(&fb, 0, 0, "A", SL6806_WHITE, SL6806_NO_BG, 1);
    CHECK(count_non_black() > 0, "text drew nothing");

    /* Transparent background must leave surrounding pixels alone. */
    sl6806_fb_fill(&fb, SL6806_BLUE);
    sl6806_fb_text(&fb, 0, 0, " ", SL6806_WHITE, SL6806_NO_BG, 1);
    int blue = 0;
    for (int i = 0; i < W * H; i++)
        if (pixels[i] == SL6806_BLUE)
            blue++;
    CHECK(blue == W * H, "space with NO_BG must not touch the buffer");

    /* Opaque background must paint the full cell. */
    sl6806_fb_fill(&fb, SL6806_BLUE);
    sl6806_fb_text(&fb, 0, 0, " ", SL6806_WHITE, SL6806_BLACK, 1);
    CHECK(sl6806_fb_get(&fb, 0, 0) == SL6806_BLACK,
          "opaque bg should paint the cell");

    CHECK(sl6806_fb_text_width("ABC", 1) == 18, "width got %d",
          sl6806_fb_text_width("ABC", 1));
    CHECK(sl6806_fb_text_width("ABC", 2) == 36, "scaled width got %d",
          sl6806_fb_text_width("ABC", 2));
    CHECK(sl6806_fb_text_width("AB\nCDE", 1) == 18,
          "multiline width should be the longest line, got %d",
          sl6806_fb_text_width("AB\nCDE", 1));

    /* Text running off the edge must clip, not corrupt. */
    reset();
    sl6806_fb_text(&fb, W - 3, H - 3, "long string here",
                   SL6806_WHITE, SL6806_BLACK, 2);
    sl6806_fb_text(&fb, -20, -5, "off the top left", SL6806_WHITE,
                   SL6806_NO_BG, 1);
}

/* Render something worth looking at, so the font can be checked by eye. */
static void write_demo_ppm(void)
{
    enum { DW = 240, DH = 135 };
    static sl6806_color_t big[DW * DH];
    sl6806_fb_t d;
    FILE *f;

    sl6806_fb_init(&d, big, DW, DH);
    sl6806_fb_fill(&d, SL6806_RGB(16, 16, 32));

    sl6806_fb_fill_rect(&d, 0, 0, DW, 18, SL6806_RGB(0, 60, 120));
    sl6806_fb_text(&d, 4, 5, "SL6806 framebuffer", SL6806_WHITE,
                   SL6806_NO_BG, 1);

    sl6806_fb_text(&d, 4, 26, "ABCDEFGHIJKLMNOPQRSTUVWXYZ", SL6806_YELLOW,
                   SL6806_NO_BG, 1);
    sl6806_fb_text(&d, 4, 36, "abcdefghijklmnopqrstuvwxyz", SL6806_CYAN,
                   SL6806_NO_BG, 1);
    sl6806_fb_text(&d, 4, 46, "0123456789 !\"#$%&'()*+,-./", SL6806_GREEN,
                   SL6806_NO_BG, 1);
    sl6806_fb_text(&d, 4, 56, ":;<=>?@[\\]^_`{|}~", SL6806_MAGENTA,
                   SL6806_NO_BG, 1);

    sl6806_fb_text(&d, 4, 70, "Scaled x2", SL6806_WHITE, SL6806_NO_BG, 2);

    sl6806_fb_rect(&d, 150, 68, 40, 30, SL6806_WHITE);
    sl6806_fb_fill_rect(&d, 195, 68, 40, 30, SL6806_RED);
    sl6806_fb_circle(&d, 30, 112, 15, SL6806_YELLOW);
    sl6806_fb_fill_circle(&d, 70, 112, 15, SL6806_GREEN);
    sl6806_fb_line(&d, 95, 128, 140, 98, SL6806_WHITE);
    sl6806_fb_line(&d, 95, 98, 140, 128, SL6806_WHITE);

    /* Clipping demo: shapes deliberately hanging off the right edge. */
    sl6806_fb_fill_circle(&d, DW - 5, 112, 20, SL6806_BLUE);

    f = fopen("gfx_demo.ppm", "wb");
    if (!f)
        return;
    fprintf(f, "P6\n%d %d\n255\n", DW, DH);
    for (int i = 0; i < DW * DH; i++) {
        sl6806_color_t c = big[i];
        unsigned char rgb[3];
        /* Expand RGB565 back to 8 bits per channel, replicating the high
         * bits so full-scale stays full-scale. */
        rgb[0] = (unsigned char)(((c >> 11) & 0x1F) * 255 / 31);
        rgb[1] = (unsigned char)(((c >> 5) & 0x3F) * 255 / 63);
        rgb[2] = (unsigned char)((c & 0x1F) * 255 / 31);
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("  wrote gfx_demo.ppm\n");
}

int main(void)
{
    printf("SL6806 framebuffer tests\n");

    test_color_macro();
    test_pixel_and_fill();
    test_pixel_clipping();
    test_hline_clipping();
    test_vline_clipping();
    test_rect();
    test_line();
    test_circle();
    test_blit();
    test_text();
    write_demo_ppm();

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
