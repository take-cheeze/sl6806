/*
 * variant.h - board definition for the "P20 Player" (SL6806).
 *
 * This is the board the reverse engineering was done on: the UI product
 * string in its firmware is "P20 Player".
 *
 * ===================================================================
 *  PIN NUMBERS ARE NOT KNOWN YET
 * ===================================================================
 * There is no pinout for this board and no GPIO register map for this SoC.
 * The pin table in variant.c is therefore empty, and every digital call
 * reports that GPIO is unconfigured instead of pretending to work.
 *
 * To bring GPIO up:
 *   1. Find the GPIO registers - see the recipe in cores/sl6806/hal_gpio.h.
 *   2. Fill in sl6806_gpio_ports[] and sl6806_pin_map[] in variant.c.
 *   3. Define SL6806_GPIO_CONFIGURED (the build then stops warning).
 *   4. Give the pins names below, so sketches read like Arduino sketches.
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
 * These are real values - size your framebuffers with them - even though the
 * panel driver's flush path is not implemented yet.
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

/* Panel reset line. The vendor code calls its GPIO writer with this pin id
 * and the sequence high / 10ms / low / 20ms / high / 120ms. The encoding of
 * the id is not yet understood, which is why GPIO is still unconfigured. */
#define SL6806_PANEL_RESET_PIN_ID  0x13800   /* [V] as an opaque vendor id */

/*
 * Named pins. These are placeholders: they are indices into an empty pin
 * table, so using them reports "GPIO not configured" rather than driving a
 * pin. They exist so example sketches compile and so the names are in one
 * place when the real mapping is discovered.
 */
#define LED_BUILTIN   0   /* [?] which pin, or whether one exists at all */

#define BTN_PLAY      1   /* [?] */
#define BTN_PREV      2   /* [?] */
#define BTN_NEXT      3   /* [?] */
#define BTN_MENU      4   /* [?] */
#define BTN_VOL_UP    5   /* [?] */
#define BTN_VOL_DOWN  6   /* [?] */

/* Peripherals known to exist on this board from the firmware analysis, none
 * of which have a driver yet - listed so the hardware inventory lives with
 * the board definition:
 *   - colour LCD driven through an LVGL 8.x port (lv_lcd_init at 0x00D3E34C
 *     in the stock image is the vendor porting layer to read)
 *   - SD/MMC card slot (also exposed as the USB card reader)
 *   - SPI NOR flash, 4 MiB, XIP at 0x00C00000
 *   - audio DAC / headphone out
 *   - Bluetooth Classic + LE radio
 *   - FM tuner
 */

#endif /* SL6806_VARIANT_H */
