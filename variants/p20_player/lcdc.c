/*
 * lcdc.c - the P20 Player's LCD controller and pad setup.
 *
 * The driver in cores/sl6806/sl6806_lcdc.c is generic. This file is the
 * board's half of it: which pads the panel is wired to, how fast to clock
 * the controller, and how its lanes are mapped. Every value was read out of
 * the stock application's own bring-up path, not out of the bootloader's -
 * the two differ, and on this board the application is right:
 *
 *     0x00D3E944  clock gate, module clock 0x910, HAL_lcdc_module_init
 *     0x00D3E9DC  the eight pad ids, then the lane map
 *     0x00D3EAC0  DMA setup (not needed; this driver polls)
 *     0x00D3E1A4  the reset pulse on pad id 0x13800
 *
 * The bootloader's copies of the first two use bank 4 pads and a slower
 * module clock (0x310). If you are bringing up a different SL6806 board,
 * that is the shape of the thing to go and find in its own firmware.
 */

#include "sl6806_lcdc.h"
#include "sl6806_padctl.h"
#include "hal_gpio.h"
#include "variant.h"

/*
 * [V] The panel's pads, from the eight calls at 0x00D3E9DC, in the
 * application's order. Decoded with sl6806_padctl.h:
 *
 *   0x00010930  bank 1 pin 1  function 2   drive 3
 *   0x00011130  bank 1 pin 2  function 2   drive 3
 *   0x00011930  bank 1 pin 3  function 2   drive 3
 *   0x00012130  bank 1 pin 4  function 2   drive 3
 *   0x00012930  bank 1 pin 5  function 2   drive 3
 *   0x00013130  bank 1 pin 6  function 2   drive 3
 *   0x00014130  bank 1 pin 8  function 2   drive 3
 *   0x000138CB  bank 1 pin 7  function 1 (output), initially high, pull 0xB
 *
 * Seven pads on function 2 is what a QSPI display wants: clock, chip select,
 * four data lanes and one more - which on this panel is most likely the tear
 * signal, since the init sequence turns TEON on. The odd one out is pin 7,
 * configured as a plain output: the reset line, and the same pad the
 * application's reset routine drives as bare id 0x13800.
 *
 * The application also has a matching teardown at 0x00D3EA74 that puts the
 * seven data pads back to function 15. Nothing here calls it.
 */
static const uint32_t p20_lcd_pads[] = {
    0x00010930u, 0x00011130u, 0x00011930u, 0x00012130u,
    0x00012930u, 0x00013130u, 0x00014130u, 0x000138CBu,
};

/*
 * [V] The lane map, 0x00D3E9DC. The application fills a 16-byte table with
 * 0x0B and then overwrites seven entries; sl6806_lcdc packs them low nibble
 * first into +0x40 and +0x44, which the vendor does at 0x00829B30.
 *
 *   table = 0B 0D 02 03 04 0C 00 0B  0F 0B 0B 0B 0B 0B 0B 0B
 *
 * The bootloader's table is different again (00 02 04 0B 0F 03 0C 0D, then
 * 0x0B), which is consistent with these being per-pad lane assignments for a
 * different set of pads rather than anything panel-specific.
 */
#define P20_LCDC_MAP0 0xB0C432DBu
#define P20_LCDC_MAP1 0xBBBBBBBFu

static const sl6806_lcdc_config_t p20_lcdc = {
    /* [V] Panel descriptor +0x08 = 1: RGB565, two bytes per pixel. */
    .color_format = 1,

    /* [V] Panel descriptor +0x0D = 0x02 and +0x0C = 0x32 - the one-lane and
     * four-lane QSPI write opcodes. */
    .cmd_opcode   = 0x02,
    .pixel_opcode = 0x32,

    /* [V] Most significant byte first. The vendor's single-pixel write at
     * 0x0082837C byte-swaps the colour before pushing it, which settles what
     * the panel expects; the framebuffer is little-endian RGB565 and the FIFO
     * emits a word low byte first, so the swap has to happen here. */
    .swap_pixel_bytes = 1,

    /* [V] 0x00D3E944 writes 0x910 into CRU 0x4008010C field 0xF10. */
    .modclk = 0x910u,

    .map0 = P20_LCDC_MAP0,
    .map1 = P20_LCDC_MAP1,

    .pads  = p20_lcd_pads,
    .npads = sizeof(p20_lcd_pads) / sizeof(p20_lcd_pads[0]),

    /* [V] Same pad as the last entry above; named again because the panel's
     * reset pulse is driven through the bus rather than as part of pad
     * setup. */
    .reset_pad = 0x000138CBu,
};

/*
 * Called from sl6806_panel_get() the first time something asks for the
 * panel, so a sketch gets a working Screen from Screen.begin() alone.
 *
 * Installing the pad controller as the GPIO back end here is not a
 * side-effect for its own sake: the panel's reset line is a GPIO, and a
 * board that can drive that pad can drive its other named pads too, so
 * digitalWrite(PIN_LCD_RESET, ...) starts working at the same moment the
 * display does.
 */
int sl6806_lcd_bus_autoinit(void)
{
    if (sl6806_lcd_bus())
        return 0;

    sl6806_gpio_vendor_register(sl6806_padctl_vendor());
    return sl6806_lcdc_begin(&p20_lcdc);
}
