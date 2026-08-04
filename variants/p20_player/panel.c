/*
 * panel.c - display definition for the P20 Player.
 *
 * The panel is now IDENTIFIED. Everything below was read out of the stock
 * firmware; provenance is given per item so you can re-check any of it.
 *
 *   240 x 296 pixels, drawn at offset (0, 12)
 *   standard MIPI DCS command set (CASET/RASET/RAMWR/MADCTL)
 *   driven by a hardware LCD controller with DMA at 0x40080000
 *   reset over a GPIO pin the vendor code identifies as 0x13800
 *
 * WHAT IS STILL MISSING - and it is one specific thing:
 * the sequence that hands a DMA descriptor to the LCDC and starts it. The
 * vendor's lcdc_set_descriptor builds the descriptor in SRAM, but the code
 * that writes it to the controller lives in a RAM-resident region which is
 * NOT stored linearly in the flash image, so it cannot be disassembled from a
 * dump alone. docs/LCD.md records exactly where the trail stops.
 *
 * Because flush() is what makes pixels appear and flush() is exactly what is
 * missing, sl6806_panel_get() still returns NULL. Registering a panel whose
 * flush did nothing would make Display::available() claim success and then
 * silently drop every frame. The geometry below is still exported, so
 * sketches can size framebuffers correctly today.
 *
 * ---------------------------------------------------------------------
 * HOW THIS WAS FOUND (so it can be extended)
 *
 * The panel descriptor lives in flash at 0x00C519FC and is copied to SRAM at
 * 0x0081C1CC. It is identifiable because bytes +17/+18 are 0x2A/0x2B - CASET
 * and RASET - and because its function-pointer fields point at the panel
 * routines that lv_lcd_init installs. Layout, verified:
 *
 *   +0x00 u16  240      width
 *   +0x02 u16  296      height
 *   +0x04 u16  0        x offset
 *   +0x06 u16  12       y offset
 *   +0x08 u8   1        colour/bus mode
 *   +0x0A u8   2
 *   +0x0C u8   50
 *   +0x0D u8   2        device/channel id, passed to the command writer
 *   +0x0E u8   0x0B
 *   +0x0F u8   0x2C     RAMWR
 *   +0x10 u8   0x2E     RAMRD
 *   +0x11 u8   0x2A     CASET
 *   +0x12 u8   0x2B     RASET
 *   +0x13 u8   0x36     MADCTL
 *   +0x14 ptr  0x00D3F46D   init sequence
 *   +0x18 ptr  0x00D3F8CD   sleep  (DISPOFF 0x28, 10ms, SLPIN 0x10, 120ms)
 *   +0x1C ptr  0x00D3F8F9   wake   (SLPOUT 0x11, 120ms, DISPON 0x29, 10ms)
 *   +0x20 ptr  0x00D3F941
 *   +0x24 ptr  0x00D3F925
 *
 * The init sequence at 0x00D3F46C sends vendor commands 0xFD, 0x61, 0x62 with
 * data bytes, so the controller is a vendor variant rather than a stock
 * ST7789 - but it answers the standard DCS commands above.
 */

#include "gfx/Panel.h"

/*
 * No driver yet: flush() is unimplemented, and a panel that cannot flush is
 * worse than no panel because it reports success. See the note above.
 */
const sl6806_panel_t *sl6806_panel_get(void)
{
    return 0;
}
