/*
 * test_panel.c - native tests for the LCD command stream and DCS windowing.
 *
 * The point of these is that the two things most likely to be silently wrong
 * about a display bring-up are testable without a display:
 *
 *   1. Does the recovered vendor init sequence survive the round trip from
 *      firmware bytes to command stream to bus calls, unchanged? A dropped
 *      or reordered parameter in a panel init shows up as a blank screen
 *      with no other symptom, and is very hard to spot by reading.
 *   2. Does the window arithmetic put the pixels where the vendor firmware
 *      puts them? This panel draws at controller offset (0, 12), so an
 *      off-by-the-offset bug produces a picture that looks right except that
 *      it is shifted - which is easy to blame on the panel instead.
 *
 * Both are checked against a recording bus, so no hardware is involved.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "gfx/Panel.h"

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

/* ------------------------------------------------------------ recording bus */

#define LOG_MAX 4096

typedef struct {
    uint8_t cmd;
    uint8_t nargs;
    uint8_t args[16];
} rec_cmd_t;

static rec_cmd_t log_cmd[LOG_MAX];
static int       log_ncmd;
static uint32_t  log_pixels;      /* total pixels pushed */
static int       log_npixel_calls;
static uint16_t  log_first_pixel;
static int       log_reset[8];
static int       log_nreset;
static uint32_t  log_delay_total;

static void rec_command(uint8_t cmd, const uint8_t *args, uint8_t nargs)
{
    if (log_ncmd >= LOG_MAX)
        return;
    log_cmd[log_ncmd].cmd = cmd;
    log_cmd[log_ncmd].nargs = nargs;
    if (nargs > sizeof(log_cmd[0].args))
        nargs = sizeof(log_cmd[0].args);
    if (nargs)
        memcpy(log_cmd[log_ncmd].args, args, nargs);
    log_ncmd++;
}

static void rec_pixels(const sl6806_color_t *src, uint32_t count)
{
    if (log_npixel_calls == 0 && count)
        log_first_pixel = src[0];
    log_npixel_calls++;
    log_pixels += count;
}

static void rec_reset(uint8_t level)
{
    if (log_nreset < (int)(sizeof(log_reset) / sizeof(log_reset[0])))
        log_reset[log_nreset++] = level;
}

static const sl6806_lcd_bus_t rec_bus = {
    .name    = "recording",
    .command = rec_command,
    .pixels  = rec_pixels,
    .reset   = rec_reset,
};

static void reset_log(void)
{
    log_ncmd = 0;
    log_pixels = 0;
    log_npixel_calls = 0;
    log_first_pixel = 0;
    log_nreset = 0;
    log_delay_total = 0;
}

/* The core's delay() is part of wiring_time.c, which needs the SysTick
 * hardware. Supply a counting stand-in instead of linking that in. */
void delay(uint32_t ms);
void delay(uint32_t ms) { log_delay_total += ms; }

/* The real ones live in wiring_time.c. LcdBus.c lifts the cap around panel
 * waits, so the stubs have to record it faithfully or the test below would
 * pass for the wrong reason. */
static uint32_t stub_block_limit;
uint32_t sl6806_block_limit(void);
uint32_t sl6806_block_limit(void) { return stub_block_limit; }
void sl6806_set_block_limit(uint32_t ms);
void sl6806_set_block_limit(uint32_t ms) { stub_block_limit = ms; }

/* ------------------------------------------------------------------ tests */

static void test_bus_registration(void)
{
    static const sl6806_lcd_bus_t no_command = { .name = "bad",
                                                 .pixels = rec_pixels };
    static const sl6806_lcd_bus_t no_pixels  = { .name = "bad",
                                                 .command = rec_command };

    CHECK(sl6806_lcd_bus_register(&no_command) == -1, "command() is required");
    CHECK(sl6806_lcd_bus_register(&no_pixels) == -1, "pixels() is required");
    CHECK(sl6806_lcd_bus() == NULL, "a rejected bus must not be installed");

    CHECK(sl6806_lcd_bus_register(&rec_bus) == 0, "a complete bus registers");
    CHECK(sl6806_lcd_bus() == &rec_bus, "and is what bus() returns");
}

static void test_stream_decoding(void)
{
    static const uint8_t seq[] = {
        SL6806_LCD_CMD, 0x36, 1, 0x00,
        SL6806_LCD_CMD, 0x11, 0,
        SL6806_LCD_DELAY, 120, 0,
        SL6806_LCD_CMD, 0xE0, 3, 0xAA, 0xBB, 0xCC,
        SL6806_LCD_DELAY, 0x2C, 0x01,          /* 300 ms, tests the high byte */
        SL6806_LCD_END,
    };

    reset_log();
    CHECK(sl6806_lcd_run(seq) == 0, "a well-formed stream runs");
    CHECK(log_ncmd == 3, "3 commands, got %d", log_ncmd);
    CHECK(log_cmd[0].cmd == 0x36 && log_cmd[0].nargs == 1 &&
          log_cmd[0].args[0] == 0x00, "MADCTL 0x00");
    CHECK(log_cmd[1].cmd == 0x11 && log_cmd[1].nargs == 0, "SLPOUT, no args");
    CHECK(log_cmd[2].cmd == 0xE0 && log_cmd[2].nargs == 3 &&
          log_cmd[2].args[0] == 0xAA && log_cmd[2].args[2] == 0xCC,
          "3-parameter vendor command");
    CHECK(log_delay_total == 120 + 300, "delays sum to 420, got %u",
          log_delay_total);
}

static void test_stream_rejects_garbage(void)
{
    static const uint8_t bad[] = { 0x7F, 0, 0, SL6806_LCD_END };

    reset_log();
    CHECK(sl6806_lcd_run(bad) == -1, "an unknown opcode stops the stream");
    CHECK(log_ncmd == 0, "and sends nothing");
}

/*
 * The real P20 tables, so a regeneration that changes them fails here.
 * Declared rather than included: they are static inside the variant, so this
 * runs the panel through its public entry points instead.
 */
extern const sl6806_dcs_panel_t sl6806_p20_panel;

static void test_p20_init_sequence(void)
{
    int i;
    int saw_colmod = 0, saw_madctl = 0, saw_slpout = 0, saw_dispon = 0;

    reset_log();
    CHECK(sl6806_dcs_begin(&sl6806_p20_panel) == 0, "init runs");

    /* Reset pulse: high, low, high - the vendor shape. */
    CHECK(log_nreset == 3, "3 reset edges, got %d", log_nreset);
    CHECK(log_reset[0] == 1 && log_reset[1] == 0 && log_reset[2] == 1,
          "reset goes high, low, high");
    /* 10 + 20 + 120 for the pulse, then 120 + 50 inside the sequence. */
    CHECK(log_delay_total == 10 + 20 + 120 + 120 + 50,
          "total delay is 320 ms, got %u", log_delay_total);

    CHECK(log_ncmd == 33, "33 commands in the vendor init, got %d", log_ncmd);

    /* Spot-check the values that decide whether anything appears at all. */
    for (i = 0; i < log_ncmd; i++) {
        switch (log_cmd[i].cmd) {
        case 0x3A:
            saw_colmod = 1;
            CHECK(log_cmd[i].args[0] == 0x55, "COLMOD is RGB565 (0x55), got 0x%02X",
                  log_cmd[i].args[0]);
            break;
        case 0x36:
            saw_madctl = 1;
            CHECK(log_cmd[i].args[0] == 0x00, "MADCTL 0x00");
            break;
        case 0x11: saw_slpout = 1; break;
        case 0x29: saw_dispon = 1; break;
        default: break;
        }
    }
    CHECK(saw_colmod && saw_madctl && saw_slpout && saw_dispon,
          "COLMOD, MADCTL, SLPOUT and DISPON are all present");

    /* Order matters: the panel must leave sleep before it is switched on. */
    {
        int slpout = -1, dispon = -1;
        for (i = 0; i < log_ncmd; i++) {
            if (log_cmd[i].cmd == 0x11 && slpout < 0) slpout = i;
            if (log_cmd[i].cmd == 0x29 && dispon < 0) dispon = i;
        }
        CHECK(slpout >= 0 && dispon > slpout, "SLPOUT precedes DISPON");
    }

    /* The first command the vendor sends unlocks the vendor register set. */
    CHECK(log_cmd[0].cmd == 0xFD && log_cmd[0].nargs == 2 &&
          log_cmd[0].args[0] == 0x06 && log_cmd[0].args[1] == 0x08,
          "the sequence opens with FD 06 08");
}

static void test_p20_power_sequences(void)
{
    reset_log();
    CHECK(sl6806_dcs_sleep(&sl6806_p20_panel) == 0, "sleep runs");
    CHECK(log_ncmd == 2 && log_cmd[0].cmd == 0x28 && log_cmd[1].cmd == 0x10,
          "sleep is DISPOFF then SLPIN");

    reset_log();
    CHECK(sl6806_dcs_wake(&sl6806_p20_panel) == 0, "wake runs");
    CHECK(log_ncmd == 2 && log_cmd[0].cmd == 0x11 && log_cmd[1].cmd == 0x29,
          "wake is SLPOUT then DISPON");

    reset_log();
    CHECK(sl6806_dcs_display(&sl6806_p20_panel, 0) == 0, "display off runs");
    CHECK(log_ncmd == 1 && log_cmd[0].cmd == 0x28, "display off is DISPOFF");
}

/* Pull a big-endian 16-bit pair out of a CASET/RASET payload. */
static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

static void test_window_offsets(void)
{
    static sl6806_color_t buf[16 * 8];
    int i;

    for (i = 0; i < 16 * 8; i++)
        buf[i] = (sl6806_color_t)(0x1000 + i);

    reset_log();
    sl6806_dcs_flush(&sl6806_p20_panel, 3, 5, 16, 8, buf);

    CHECK(log_ncmd == 3, "CASET, RASET, RAMWR, got %d commands", log_ncmd);
    CHECK(log_cmd[0].cmd == 0x2A && log_cmd[0].nargs == 4, "CASET with 4 bytes");
    CHECK(be16(log_cmd[0].args) == 3 && be16(log_cmd[0].args + 2) == 3 + 16 - 1,
          "columns 3..18, got %u..%u", be16(log_cmd[0].args),
          be16(log_cmd[0].args + 2));

    CHECK(log_cmd[1].cmd == 0x2B && log_cmd[1].nargs == 4, "RASET with 4 bytes");
    CHECK(be16(log_cmd[1].args) == 5 + 12 &&
          be16(log_cmd[1].args + 2) == 5 + 12 + 8 - 1,
          "rows carry the +12 panel offset: got %u..%u",
          be16(log_cmd[1].args), be16(log_cmd[1].args + 2));

    CHECK(log_cmd[2].cmd == 0x2C && log_cmd[2].nargs == 0, "then RAMWR");
    CHECK(log_pixels == 16u * 8u, "128 pixels, got %u", log_pixels);
    CHECK(log_npixel_calls == 1,
          "a full-width rectangle goes in one burst, got %d calls",
          log_npixel_calls);
    CHECK(log_first_pixel == 0x1000, "starting at the first source pixel");
}

static void test_window_subrect(void)
{
    static sl6806_color_t buf[8 * 8];
    int i;

    for (i = 0; i < 8 * 8; i++)
        buf[i] = (sl6806_color_t)i;

    /*
     * Clipping is what makes the source non-contiguous: an 8-wide rectangle
     * hanging two columns over the right edge still has 8-pixel rows in the
     * caller's buffer, but only 6 of each go to the panel. Getting that
     * stride wrong is how a clipped blit turns into a diagonal smear.
     */
    reset_log();
    sl6806_dcs_flush(&sl6806_p20_panel, (int16_t)(sl6806_p20_panel.width - 6),
                     0, 8, 8, buf);
    CHECK(log_pixels == 6u * 8u, "48 pixels, got %u", log_pixels);
    CHECK(log_npixel_calls == 8, "one burst per row, got %d", log_npixel_calls);
    CHECK(log_first_pixel == buf[0], "starting at the first source pixel");
}

static void test_window_clipping(void)
{
    static sl6806_color_t buf[8 * 8];

    memset(buf, 0, sizeof buf);

    reset_log();
    sl6806_dcs_flush(&sl6806_p20_panel, -100, -100, 8, 8, buf);
    CHECK(log_ncmd == 0 && log_pixels == 0,
          "a rectangle entirely off-screen sends nothing");

    reset_log();
    sl6806_dcs_flush(&sl6806_p20_panel, sl6806_p20_panel.width + 4, 0, 8, 8, buf);
    CHECK(log_ncmd == 0 && log_pixels == 0, "off the right edge sends nothing");

    /* Straddling the left edge: the window starts at 0 and narrows. */
    reset_log();
    sl6806_dcs_flush(&sl6806_p20_panel, -3, 0, 8, 8, buf);
    CHECK(log_ncmd == 3, "a partly visible rectangle is still drawn");
    CHECK(be16(log_cmd[0].args) == 0 && be16(log_cmd[0].args + 2) == 4,
          "clipped to columns 0..4, got %u..%u", be16(log_cmd[0].args),
          be16(log_cmd[0].args + 2));
    CHECK(log_pixels == 5u * 8u, "5 columns survive, got %u pixels", log_pixels);

    /* Straddling the bottom edge. */
    reset_log();
    sl6806_dcs_flush(&sl6806_p20_panel, 0, (int16_t)(sl6806_p20_panel.height - 3),
                     8, 8, buf);
    CHECK(log_pixels == 8u * 3u, "3 rows survive, got %u pixels", log_pixels);
    CHECK(be16(log_cmd[1].args + 2) ==
          (uint16_t)(sl6806_p20_panel.height - 1 + sl6806_p20_panel.y_offset),
          "the last row is the last visible one");
}

static void test_no_bus_is_safe(void)
{
    static sl6806_color_t buf[4];

    sl6806_lcd_bus_register(NULL);
    CHECK(sl6806_lcd_bus() == NULL, "unregistering works");
    CHECK(sl6806_panel_get() == NULL,
          "and with no bus the variant reports no panel");
    CHECK(sl6806_dcs_begin(&sl6806_p20_panel) == -1, "begin() fails cleanly");
    sl6806_dcs_flush(&sl6806_p20_panel, 0, 0, 2, 2, buf);   /* must not crash */

    sl6806_lcd_bus_register(&rec_bus);
    CHECK(sl6806_panel_get() != NULL, "with a bus the panel appears");
    CHECK(sl6806_panel_get()->width == sl6806_p20_panel.width,
          "and reports the panel width");
}

/*
 * The bug this exists to stop coming back.
 *
 * RUN_MODE=poll caps delay() at 50 ms so that a blocking sketch cannot wedge
 * the USB link. Applied to the panel, that cap silently shortens the 120 ms
 * after SLPOUT and the 120 ms after reset - and the symptom is a display that
 * initialises cleanly, reports no error, and stays black. It cost a hardware
 * session. The panel path must ignore the cap.
 */
static void test_panel_delays_ignore_the_block_cap(void)
{
    extern const sl6806_dcs_panel_t sl6806_p20_panel;

    sl6806_lcd_bus_register(&rec_bus);
    reset_log();
    stub_block_limit = 50;          /* what poll mode sets */

    sl6806_dcs_begin(&sl6806_p20_panel);

    /* reset 10 + 20 + 120, then the sequence's 120 + 50. */
    CHECK(log_delay_total == 320,
          "panel bring-up waited %u ms, want 320 - a cap got applied",
          (unsigned)log_delay_total);
    CHECK(stub_block_limit == 50, "the cap was not put back");

    stub_block_limit = 0;
}

int main(void)
{
    test_bus_registration();
    test_stream_decoding();
    test_stream_rejects_garbage();
    test_p20_init_sequence();
    test_p20_power_sequences();
    test_window_offsets();
    test_window_subrect();
    test_window_clipping();
    test_no_bus_is_safe();

    test_panel_delays_ignore_the_block_cap();

    printf("test_panel: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
