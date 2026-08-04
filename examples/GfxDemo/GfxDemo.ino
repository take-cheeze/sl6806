/*
 * GfxDemo - the display stack, drawing into RAM.
 *
 * WHAT YOU WILL SEE: nothing on the screen. There is no panel driver for any
 * SL6806 board yet - the controller, resolution and bus are all still
 * unknown - so this reports that over Serial and then draws anyway.
 *
 * Drawing anyway is the point. The framebuffer and every primitive are
 * finished and unit-tested (tests/host/test_gfx.c), so a UI can be written
 * and checked now, and it will appear the moment someone fills in
 * sl6806_panel_get(). This sketch verifies the drawing worked by reading
 * pixels back and printing a summary.
 *
 * MEMORY: this uses a small static buffer rather than a full screen. In
 * payload mode the entire window is 64 KB, so a 240x135 framebuffer
 * (~65 KB) cannot exist. Rendering a band at a time and pushing it
 * repeatedly is the normal way around that, and is what a real sketch on
 * this chip would do.
 */

#include <Arduino.h>

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
    Serial.println();
    Serial.println("=== SL6806 gfx demo ===");

    if (!Display::panelAvailable()) {
        Serial.println("No panel driver: drawing into RAM only.");
        Serial.println("See docs/LCD.md to add one.");
    }

    /* Returns false without a panel, but the framebuffer is still set up so
     * every draw call below works. */
    Screen.begin(band, BAND_W, BAND_H);

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
