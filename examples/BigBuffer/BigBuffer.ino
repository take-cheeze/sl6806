/*
 * BigBuffer - prove the rest of the megabyte is usable, then fill the screen.
 *
 * WHY THIS EXISTS. Until now every sketch here lived inside 64 KiB at
 * 0x00820000, because that is the window the stock smartlink_flash payload
 * uses and nobody had established anything else was safe. That ceiling is
 * why examples/GfxDemo draws a 160x40 band: a full 240x296 RGB565 framebuffer
 * is 138 KiB and could not exist.
 *
 * It can now. SRAM is 1 MiB, measured (docs/sl6806_re_notes.md 3), and the
 * loaded part of a payload - the bytes actually in the .bin - is the only
 * thing that has to fit in 64 KiB. bss and heap are NOLOAD and cost nothing
 * in the file, so ld/sl6806_payload.ld hands them everything up to
 * 0x008F0000.
 *
 * BUT NOBODY HAS PROVED THAT MEMORY IS FREE. It is almost certainly not the
 * ROM's - the ROM's own data and stack are below 0x00806000 - but "almost
 * certainly" is how you lose a USB link mid-session. So this walks the whole
 * heap with a pattern first and reports what it finds, and only then draws.
 *
 * The pattern is address-derived, so a region that aliases another shows up
 * as a mismatch rather than passing by accident - which matters here, because
 * an incompletely decoded address space is exactly what a chip with no
 * datasheet might do.
 *
 *     make SKETCH=examples/BigBuffer RUN_MODE=poll upload
 *     tools/sl6806-monitor build/BigBuffer.sym
 *
 * If the test passes, a full-screen framebuffer is a normal thing to declare
 * and the 64 KiB note in the README is obsolete.
 */

#include <Arduino.h>
#include "sl6806_pwm.h"

extern char __heap_start__, __heap_end__;

/* The whole panel: 240 x 296 RGB565 = 138.75 KiB, in bss, above the window. */
#define FB_W 240
#define FB_H 296
static sl6806_color_t framebuffer[FB_W * FB_H];

static uint32_t pattern(uintptr_t a)
{
    /* Address-derived, so an alias cannot pass. */
    return (uint32_t)a ^ 0xA5A5A5A5u;
}

/*
 * Walk [start, end) writing the pattern, then walk it again checking.
 *
 * One pass, not paced - which is a deliberate exception to the rule about
 * blocking inside the boot ROM's USB handler. 656 KiB of writes and reads is
 * a few hundred thousand instructions, tens of milliseconds, well inside the
 * host's patience. The thing that takes the device off the bus is a spin
 * waiting on hardware that never answers, not a bounded memcpy-shaped loop.
 */
static bool check_region(uintptr_t start, uintptr_t end)
{
    volatile uint32_t *p;
    uintptr_t a;
    uint32_t bad = 0;
    uintptr_t first_bad = 0;

    for (a = start; a + 4 <= end; a += 4) {
        p = (volatile uint32_t *)a;
        *p = pattern(a);
    }
    for (a = start; a + 4 <= end; a += 4) {
        p = (volatile uint32_t *)a;
        if (*p != pattern(a)) {
            if (!bad++)
                first_bad = a;
        }
    }

    Serial.print("  0x");
    Serial.print((uint32_t)start, HEX);
    Serial.print("..0x");
    Serial.print((uint32_t)end, HEX);
    Serial.print("  ");
    Serial.print((uint32_t)((end - start) / 1024));
    Serial.print(" KiB: ");
    if (!bad) {
        Serial.println("ok");
        return true;
    }
    Serial.print(bad);
    Serial.print(" words wrong, first at 0x");
    Serial.println((uint32_t)first_bad, HEX);
    return false;
}

static void draw(void)
{
    int16_t x, y;

    /* A gradient, so a torn or aliased buffer is obvious rather than subtle. */
    for (y = 0; y < FB_H; y++)
        for (x = 0; x < FB_W; x++)
            framebuffer[y * FB_W + x] =
                SL6806_RGB((x * 255) / FB_W, (y * 255) / FB_H, 128);

    Screen.fillRect(20, 20, FB_W - 40, 40, SL6806_BLACK);
    Screen.drawRect(20, 20, FB_W - 40, 40, SL6806_WHITE);
    Screen.drawText(28, 36, "full screen", SL6806_WHITE);
    Screen.drawText(28, 48, "240x296", SL6806_YELLOW);
}

void setup()
{
    uintptr_t hs = (uintptr_t)&__heap_start__;
    uintptr_t he = (uintptr_t)&__heap_end__;

    Serial.begin(115200);
    Serial.println();
    Serial.println("=== SL6806 big buffer ===");

    if (sl6806_backlight_begin(100))
        Serial.println("backlight on");

    Serial.print("framebuffer 0x");
    Serial.print((uint32_t)(uintptr_t)framebuffer, HEX);
    Serial.print(" .. 0x");
    Serial.print((uint32_t)((uintptr_t)framebuffer + sizeof framebuffer), HEX);
    Serial.print("  (");
    Serial.print((uint32_t)(sizeof framebuffer / 1024));
    Serial.println(" KiB, in bss above the 64 KiB window)");

    Serial.print("heap 0x");
    Serial.print((uint32_t)hs, HEX);
    Serial.print(" .. 0x");
    Serial.print((uint32_t)he, HEX);
    Serial.print("  (");
    Serial.print((uint32_t)((he - hs) / 1024));
    Serial.println(" KiB)");
    Serial.println();

    Serial.println("pattern-testing the heap before trusting it:");
    if (!check_region(hs, he))
        Serial.println("  ^ do NOT widen the linker script further");
    Serial.println();

    if (!Screen.begin(framebuffer, FB_W, FB_H)) {
        Serial.println("no panel; the buffer test above still stands");
        return;
    }
    draw();
    Screen.display();
    Serial.println("full-screen frame pushed.");
}

void loop()
{
    static uint32_t next;

    if (millis() < next)
        return;
    next = millis() + 2000;

    draw();
    Screen.display();
    Serial.println("frame");
}
