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
 *  THE BUS EXISTS NOW
 * =====================================================================
 * This header used to say the byte-level transport was the one missing
 * piece, because the two routines the stock application calls to put a byte
 * on the wire live in SRAM and are in no dump. They are still in no dump -
 * but the HLKJ bootloader has its own copies, in flash, and those
 * disassemble. cores/sl6806/sl6806_lcdc.c is a transcription of them: the
 * panel is a QSPI display and the controller at 0x400D9000 is the QSPI
 * master.
 *
 * On the P20 the bus comes up by itself the first time anything asks for the
 * panel, so Screen.begin() is all a sketch needs.
 *
 * The seam below is still the seam. Supply an sl6806_lcd_bus_t and the whole
 * stack above it - init sequence, window addressing, framebuffer, Display,
 * text - works unchanged, which is how a different board or a bit-banged bus
 * would plug in. A board with no bus still reports NULL from
 * sl6806_panel_get(), and Display::begin() says why rather than silently
 * drawing nowhere.
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
 * The byte-level transport to the panel controller.
 *
 * The one this framework ships drives the LCD controller at 0x400D9000; see
 * sl6806_lcdc.h. A board could equally bit-bang the panel on GPIO, whose
 * registers are also known now (sl6806_padctl.h), though there is no reason
 * to on a board that has the controller.
 *
 * One route that does NOT work, recorded so it is not tried again: calling
 * the application's SRAM routines at 0x0080E842 and 0x0080E8D8. A
 * bootloader-mode memory dump shows uninitialised noise at both addresses,
 * because the application that installs them has never run. See
 * docs/sl6806_re_notes.md 7f.
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

    /* Optional. Called once after the final pixels() of a flush.
     *
     * A rectangle whose rows are not contiguous in the source arrives as
     * several pixels() calls, and a bus that has to decide "this is the last
     * transfer" *before* starting it - which the LCDC does, because
     * releasing chip select is a bit set before the transfer, not after -
     * cannot tell from pixels() alone which call is the last. This is how it
     * finds out. A bus with no such state leaves it NULL. */
    void (*pixels_end)(void);

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

/*
 * Bring up whatever transport the board has, if none is registered yet.
 *
 * Weak, and a no-op by default, so that a variant with a real bus can make
 * Screen.begin() work with no setup call in the sketch, while a host test -
 * or a board with no display - links the default and never touches a
 * peripheral. variants/p20_player/lcdc.c is the reference implementation.
 *
 * Returns 0 if a bus is registered on return, -1 otherwise.
 */
int sl6806_lcd_bus_autoinit(void);

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
