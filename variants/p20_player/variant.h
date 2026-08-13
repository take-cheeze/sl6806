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
#define PIN_EXT_RESET 8   /* [V] vendor id 0x18000, touch controller reset */
#define PIN_TP_INT    9   /* [V] vendor id 0x16800, touch interrupt, active low */

#define PIN_CAM_RESET 10  /* [V] vendor id 0x47080, camera reset */
#define PIN_CAM_PWDN  11  /* [V] vendor id 0x47880, camera power-down */

/* PIN_EXT_RESET was named before its device was known. It is the reset line
 * of the touch controller - see docs/sl6806_re_notes.md §7h - so prefer this
 * name in new code. The old one stays because sketches use it. */
#define PIN_TP_RESET  PIN_EXT_RESET

/* Peripherals known to exist on this board from the firmware analysis.
 * Listed so the hardware inventory lives with the board definition; only the
 * display has a driver:
 *   - colour LCD on a QSPI bus - driven, see panel.c and lcdc.c
 *   - SD/MMC card slot (also exposed as the USB card reader)
 *   - SPI NOR flash, 4 MiB, XIP at 0x00C00000
 *   - audio DAC / headphone out
 *   - Bluetooth Classic + LE radio
 *   - FM tuner
 *   - capacitive touch panel - Hynitron CST816 family, TWI bus 1, addr 0x15
 *   - camera - 1 MP "sc101", TWI bus 0, addr 0x68, DVP on bank 4, with
 *     hardware JPEG and H.264 encoders behind it
 *
 * The last two are documented register-for-register in
 * docs/sl6806_re_notes.md §7h, and neither has to wait for the TWI
 * controller base that the stock firmware reaches them through: every line
 * either one needs is an ordinary pad. examples/TouchDemo bit-bangs bus 1
 * and reads coordinates; examples/CameraDemo runs the sensor's power-up on
 * PIN_CAM_RESET and PIN_CAM_PWDN and bit-bangs bus 0 for its chip id.
 *
 * The camera has one line the touch panel does not - MCLK, bank 4 pin 3,
 * which the vendor feeds from a clock channel whose registers are unknown.
 * A sensor of this family clocks its register block from MCLK, so that, and
 * not the TWI base, is what may still stand between this board and a
 * responding camera. Pixels are further off again: the DVP/CSI front end
 * and the codecs behind it are undecoded.
 */

#endif /* SL6806_VARIANT_H */
