/*
 * variant.h - board definition for the "P20 Player" (SL6806).
 *
 * This is the board the reverse engineering was done on: the UI product
 * string in its firmware is "P20 Player".
 *
 * ===================================================================
 *  MOST PIN NUMBERS ARE STILL UNKNOWN
 * ===================================================================
 * The GPIO driver is done - cores/sl6806/sl6806_padctl.c, registers read out
 * of the mask ROM - but there is no pinout for this board, so most of the
 * names below are placeholders and using them reports that the pin is
 * unconfigured instead of pretending to work.
 *
 * Two pins are an exception: PIN_LCD_RESET and PIN_EXT_RESET have the pad
 * ids the stock firmware uses, read off its own call sites. They work as
 * soon as the pad controller is installed as the GPIO back end, which the
 * display bring-up does (variants/p20_player/lcdc.c).
 *
 * To bring the rest up, work out which pad id each button and LED uses -
 * the recipe is in cores/sl6806/hal_gpio.h - add it to
 * sl6806_vendor_pin_map[] in variant.c, and give it a name below so sketches
 * read like Arduino sketches.
 *
 * If you are bringing up a different SL6806 board, copy this directory
 * rather than editing it, and build with BOARD=<your-board>.
 */
#ifndef SL6806_VARIANT_H
#define SL6806_VARIANT_H

#define SL6806_VARIANT_NAME "p20_player"

/*
 * Display geometry, read out of the stock firmware's panel descriptor
 * (flash 0x00C519FC). See variants/p20_player/panel.c for the full decode.
 * A full framebuffer at this size is 139 KB, which only fits in firmware
 * mode - render a band and push it, as examples/GfxDemo does.
 */
#define SL6806_PANEL_WIDTH     240   /* [V] */
#define SL6806_PANEL_HEIGHT    296   /* [V] */
#define SL6806_PANEL_X_OFFSET  0     /* [V] */
#define SL6806_PANEL_Y_OFFSET  12    /* [V] */

/* MIPI DCS commands the panel answers. [V] from the same descriptor. */
#define SL6806_DCS_SLPIN       0x10
#define SL6806_DCS_SLPOUT      0x11
#define SL6806_DCS_DISPOFF     0x28
#define SL6806_DCS_DISPON      0x29
#define SL6806_DCS_CASET       0x2A
#define SL6806_DCS_RASET       0x2B
#define SL6806_DCS_RAMWR       0x2C
#define SL6806_DCS_RAMRD       0x2E
#define SL6806_DCS_MADCTL      0x36

/* Panel reset line, as a packed pad id: bank 1, pin 7. The vendor code
 * drives it high / 10ms / low / 20ms / high / 120ms. The display bring-up
 * uses 0x000138CB, the same pad with its configuration fields filled in -
 * output, initially high. See cores/sl6806/sl6806_padctl.h. */
#define SL6806_PANEL_RESET_PIN_ID  0x13800   /* [V] */

/*
 * Named pins - Arduino pin numbers, indices into the tables in variant.c.
 *
 * PIN_LCD_RESET and PIN_EXT_RESET have real vendor pin ids and work through
 * a vendor back end. The rest are placeholders that report "GPIO not
 * configured"; they exist so example sketches compile and so the names are
 * in one place when the real mapping is discovered.
 */
#define LED_BUILTIN   0   /* [?] which pin, or whether one exists at all */

#define BTN_PLAY      1   /* [?] */
#define BTN_PREV      2   /* [?] */
#define BTN_NEXT      3   /* [?] */
#define BTN_MENU      4   /* [?] */
#define BTN_VOL_UP    5   /* [?] */
#define BTN_VOL_DOWN  6   /* [?] */

#define PIN_LCD_RESET 7   /* [V] vendor id 0x13800, panel reset */
#define PIN_EXT_RESET 8   /* [V] vendor id 0x18000, [I] an I2C device's reset */

/* Peripherals known to exist on this board from the firmware analysis.
 * Listed so the hardware inventory lives with the board definition; only the
 * display has a driver:
 *   - colour LCD on a QSPI bus - driven, see panel.c and lcdc.c
 *   - SD/MMC card slot (also exposed as the USB card reader)
 *   - SPI NOR flash, 4 MiB, XIP at 0x00C00000
 *   - audio DAC / headphone out
 *   - Bluetooth Classic + LE radio
 *   - FM tuner
 */

/* ------------------------------------------------------------------ */
/* Keys                                                                */
/* ------------------------------------------------------------------ */

/*
 * [M] The two volume buttons, measured on hardware 2026-08-07.
 *
 * They are not GPIO. Both sit on a resistor ladder into ADC channel 0, on
 * bank 1 pin 9 with the pad in function 15 (analog). The vendor's key map at
 * 0x0081C02C is a list of ascending thresholds, and the plateaus this board
 * actually produces land where it predicts:
 *
 *     nothing pressed   ~0xFEA
 *     volume up         ~0xD58   (key id 0x40)
 *     volume down       ~0x23    (key id 0x42)
 *
 * Which id is which was settled by pressing them; the ids themselves come
 * from the firmware. See docs/sl6806_re_notes.md 15 for the bring-up order,
 * which is five steps and unforgiving about all of them.
 */
#define SL6806_KEY_ADC_CHANNEL   0
#define SL6806_KEY_ADC_PAD       0x00014F80u  /* [V] bank 1 pin 9, function 15 */

#define SL6806_KEY_NONE          (-1)
#define SL6806_KEY_VOL_UP        0x40         /* [M] */
#define SL6806_KEY_VOL_DOWN      0x42         /* [M] */

/* [V] Thresholds from the vendor's map, ascending. */
#define SL6806_KEY_LEVEL_DOWN    0x0200u      /* below this: VOL_DOWN */
#define SL6806_KEY_LEVEL_UP      0x0E60u      /* below this: VOL_UP   */

/* Decode a conversion into a key id, or SL6806_KEY_NONE. */
static inline int sl6806_key_decode(unsigned long adc)
{
    if (adc < SL6806_KEY_LEVEL_DOWN)
        return SL6806_KEY_VOL_DOWN;
    if (adc < SL6806_KEY_LEVEL_UP)
        return SL6806_KEY_VOL_UP;
    return SL6806_KEY_NONE;
}

#endif /* SL6806_VARIANT_H */
