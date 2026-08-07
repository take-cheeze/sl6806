/*
 * GfxDemo - the display stack, drawing into RAM.
 *
 * WHAT YOU SHOULD SEE: a 160x40 band in the top-left corner of the panel,
 * with a title bar, some shapes and a running millisecond count.
 *
 * "Should", because the driver behind it was written from a disassembly of
 * the stock bootloader and has never been checked against a screen.
 *
 * WHAT IS DIFFERENT NOW: the backlight works, and this sketch turns it on. So
 * for the first time a dark panel is a statement about the bus driver rather
 * than about the lamp. Every previous "no picture" result on this board was
 * taken with the backlight off and means nothing.
 *
 * If it stays dark, docs/LCD.md has the list of things to try in the order
 * worth trying them - and section 5 of that list, the byte order, is now
 * settled from two directions, so start further down.
 *
 * Either way the sketch also reads its own pixels back and prints them, so
 * it says something useful even with the display disconnected: the
 * framebuffer and every primitive are unit-tested (tests/host/test_gfx.c),
 * and a UI can be developed against the readback alone.
 *
 * MEMORY: this uses a small static buffer rather than a full screen. In
 * payload mode the entire window is 64 KB, so a 240x135 framebuffer
 * (~65 KB) cannot exist. Rendering a band at a time and pushing it
 * repeatedly is the normal way around that, and is what a real sketch on
 * this chip would do.
 */

#include <Arduino.h>
#include "sl6806_pwm.h"

/* 160 x 40 x 2 bytes = 12.8 KB - comfortable inside the payload heap. */
static const int16_t BAND_W = 160;
static const int16_t BAND_H = 40;
static sl6806_color_t band[BAND_W * BAND_H];

static void drawScene()
{
    Screen.fill(SL6806_RGB(16, 16, 32));

    /* Title bar. */
    Screen.fillRect(0, 0, BAND_W, 11, SL6806_RGB(0, 60, 120));
    Screen.drawText(2, 2, "SL6806 gfx", SL6806_WHITE);

    /* Shapes. */
    Screen.drawRect(4, 15, 30, 20, SL6806_WHITE);
    Screen.fillRect(38, 15, 30, 20, SL6806_RED);
    Screen.drawCircle(85, 25, 9, SL6806_YELLOW);
    Screen.fillCircle(110, 25, 9, SL6806_GREEN);
    Screen.drawLine(125, 15, 155, 35, SL6806_CYAN);

    /* Print interface: Screen.print works like Serial.print. */
    Screen.setCursor(4, 40 - 9);
    Screen.setTextColor(SL6806_WHITE);
    Screen.print("t=");
    Screen.print(millis());
}

void setup()
{
    Serial.begin(115200);

    /* The lamp first, so a dark screen means the driver and not the light. */
    if (sl6806_backlight_begin(100))
        Serial.println("backlight on");
    else
        Serial.println("backlight did NOT come up - a dark panel proves nothing");
    Serial.println();
    Serial.println("=== SL6806 gfx demo ===");

    /* Returns false without a panel, but the framebuffer is still set up so
     * every draw call below works. This is also what brings the LCD
     * controller up on a board that has one. */
    if (!Screen.begin(band, BAND_W, BAND_H)) {
        Serial.println("No panel: drawing into RAM only.");
        Serial.println("See docs/LCD.md.");
    } else {
        Serial.print("panel: ");
        Serial.print(sl6806_panel_get()->name);
        Serial.print(" over ");
        Serial.println(sl6806_lcd_bus()->name);
    }

    Serial.print("framebuffer: ");
    Serial.print(Screen.width());
    Serial.print("x");
    Serial.print(Screen.height());
    Serial.print(" (");
    Serial.print((uint32_t)sizeof(band));
    Serial.println(" bytes)");

    drawScene();

    /* Prove the drawing actually happened by sampling pixels back. The panel
     * cannot confirm it, so the framebuffer has to. */
    Serial.println();
    Serial.println("readback checks:");

    Serial.print("  title bar   : 0x");
    Serial.println(Screen.readPixel(1, 1), HEX);
    Serial.print("  red rect    : 0x");
    Serial.println(Screen.readPixel(50, 25), HEX);
    Serial.print("  green disc  : 0x");
    Serial.println(Screen.readPixel(110, 25), HEX);
    Serial.print("  empty area  : 0x");
    Serial.println(Screen.readPixel(78, 38), HEX);

    Serial.println();
    Serial.println("expected: F800 for the red rect, 07E0 for the green disc.");

    /* A no-op without a panel; the real call once one exists. */
    Screen.display();
}

void loop()
{
    static uint32_t frame;

    drawScene();
    Screen.display();

    Serial.print("frame ");
    Serial.println(frame++);

    delay(1000);
}
