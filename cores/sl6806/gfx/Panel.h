/*
 * Panel.h - the hardware half of the display stack.
 *
 * The panel itself is no longer a mystery. Everything a driver needs was
 * recovered from the stock firmware and lives in the variant:
 *
 *   240 x 296 visible, drawn at controller offset (0, 12)
 *   RGB565 (COLMOD 0x55), standard MIPI DCS command set
 *   a 33-command vendor init sequence, plus sleep/wake/display on/off
 *   reset on the pin the vendor code calls 0x13800
 *
 * See tools/sl6806-panelseq, which re-derives all of it from a dump.
 *
 * =====================================================================
 *  WHAT IS STILL MISSING: THE BUS, AND ONLY THE BUS
 * =====================================================================
 * Knowing the commands is not the same as being able to send them. The two
 * routines the stock firmware calls to put a byte on the wire -
 *
 *     0x0080E842  lcd_write_cmd(last, devid, cmd)
 *     0x0080E8D8  lcd_write_data(last, byte)
 *
 * - live in SRAM and are not present anywhere in the flash image, so they
 * cannot be disassembled from a dump. They belong to the mask ROM's driver
 * set (see docs/sl6806_re_notes.md 7d), which means a ROM dump, not a flash
 * dump, is what unlocks them.
 *
 * So this header defines the seam at exactly that line. Supply an
 * sl6806_lcd_bus_t - three short functions - and the whole stack above it
 * (init sequence, window addressing, framebuffer, Display, text) works
 * unchanged. Until something registers one, sl6806_panel_get() returns NULL
 * and Display::begin() says why rather than silently drawing nowhere.
 */
#ifndef SL6806_PANEL_H
#define SL6806_PANEL_H

#include <stdint.h>
#include "Framebuffer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ bus */

/*
 * The byte-level transport to the panel controller. This is the one thing
 * the framework cannot supply for you.
 *
 * A bus can be implemented three ways, in increasing order of difficulty:
 *
 *   1. Call the ROM routines above, if a memory dump confirms they are
 *      resident when your sketch runs. See docs/LCD.md.
 *   2. Program the LCD controller directly, once its registers are known.
 *   3. Bit-bang the panel on GPIO, once the GPIO registers are known.
 *
 * All three produce the same three functions.
 */
typedef struct {
    const char *name;      /* shown at startup, e.g. "ROM lcd_write" */

    /* Send one command byte followed by `nargs` parameter bytes. The panel
     * chip select (or its equivalent) must be released afterwards - the
     * vendor bus signals this with a "last" flag on the final byte. */
    void (*command)(uint8_t cmd, const uint8_t *args, uint8_t nargs);

    /* Send `count` RGB565 pixels as the parameter stream of a RAMWR that
     * command() has just issued. Called once per flush() row-block, so it is
     * worth making this a DMA burst rather than a byte loop. */
    void (*pixels)(const sl6806_color_t *src, uint32_t count);

    /* Drive the panel's reset pin, 1 = deasserted. NULL if the board wires
     * reset to something the bus handles itself. */
    void (*reset)(uint8_t level);

    /* Optional backlight control, 0..255. NULL if not controllable. */
    void (*backlight)(uint8_t level);
} sl6806_lcd_bus_t;

/*
 * Register the transport. Pass NULL to unregister. The pointer is kept, not
 * copied, so it must outlive the display - a static const is the usual
 * choice. Returns 0 on success, -1 if `bus` is missing command() or
 * pixels(), which are the two that cannot be defaulted.
 */
int sl6806_lcd_bus_register(const sl6806_lcd_bus_t *bus);

/* The registered transport, or NULL. */
const sl6806_lcd_bus_t *sl6806_lcd_bus(void);

/* ------------------------------------------------------- command streams */

/*
 * Init sequences are stored as a byte stream so a variant can carry several
 * hundred bytes of vendor magic without hundreds of function calls.
 *
 *   SL6806_LCD_CMD,   cmd, n, b0..bn-1     one DCS command with n parameters
 *   SL6806_LCD_DELAY, lo, hi               wait for a little-endian ms count
 *   SL6806_LCD_END                         stop
 *
 * tools/sl6806-panelseq --c emits exactly this format.
 */
#define SL6806_LCD_END   0x00
#define SL6806_LCD_CMD   0x01
#define SL6806_LCD_DELAY 0x02

/* Run a command stream on the registered bus. Returns 0, or -1 if there is
 * no bus or the stream contains an unknown opcode. */
int sl6806_lcd_run(const uint8_t *seq);

/* --------------------------------------------------- generic DCS panel */

/*
 * Everything about a MIPI DCS panel that is board-specific. A variant fills
 * one of these in and hands it to the two functions below; no variant needs
 * to write windowing or pixel-pushing code of its own.
 */
typedef struct {
    int16_t width, height;      /* visible area */
    int16_t x_offset, y_offset; /* where that area sits in controller RAM */

    uint8_t caset, raset, ramwr;

    const uint8_t *init;        /* required */
    const uint8_t *sleep;       /* may be NULL */
    const uint8_t *wake;
    const uint8_t *display_on;
    const uint8_t *display_off;

    /* Reset pulse shape, in milliseconds. Ignored when the bus has no
     * reset(). The vendor sequence is high 10 / low 20 / high 120. */
    uint16_t reset_high_ms, reset_low_ms, reset_settle_ms;
} sl6806_dcs_panel_t;

/* Reset the controller and run its init sequence. 0 on success. */
int sl6806_dcs_begin(const sl6806_dcs_panel_t *p);

/* Address a window and push `w * h` pixels into it. `src` is row-major and
 * `w` wide. Coordinates are in visible-area space; the offsets are added
 * here. Out-of-range rectangles are clipped, and a fully clipped one is a
 * no-op rather than a bad CASET. */
void sl6806_dcs_flush(const sl6806_dcs_panel_t *p,
                      int16_t x, int16_t y, int16_t w, int16_t h,
                      const sl6806_color_t *src);

int sl6806_dcs_sleep(const sl6806_dcs_panel_t *p);
int sl6806_dcs_wake(const sl6806_dcs_panel_t *p);
int sl6806_dcs_display(const sl6806_dcs_panel_t *p, int on);

/* ------------------------------------------------------------- panel API */

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
 * Supplied by the variant. Returns NULL when no panel can be driven - which
 * on the P20 Player means no LCD bus has been registered yet.
 */
const sl6806_panel_t *sl6806_panel_get(void);

#ifdef __cplusplus
}
#endif

#endif /* SL6806_PANEL_H */
