/*
 * Hello - the smallest sketch that proves the whole chain works.
 *
 * No GPIO, so nothing here depends on the unknown register map. If you see
 * this output in the monitor, then: the payload loaded, the boot ROM called
 * it, .bss was cleared, C++ constructors ran, the heap works, the console
 * ring works, and loop() is being driven. That is the bring-up test.
 *
 *   make SKETCH=examples/Hello upload
 *   make SKETCH=examples/Hello monitor
 */

#include <Arduino.h>

static uint32_t counter;

void setup()
{
    Serial.begin(115200);

    Serial.println();
    Serial.println("=== SL6806 Arduino framework ===");
    Serial.print("variant : ");
    Serial.println(SL6806_VARIANT_NAME);
    Serial.print("F_CPU   : ");
    Serial.print(F_CPU / 1000000UL);
    Serial.println(" MHz (assumed - `make calibrate` measures the real one)");

    /* Exercises the heap and the C++ runtime, not just printing. */
    String s = "heap+String ok, ";
    s += 6806;
    s += " reporting in";
    Serial.println(s);

    Serial.print("GPIO    : ");
    Serial.println(sl6806_gpio_available() ? "configured" : "NOT configured");
    Serial.println();
}

void loop()
{
    Serial.print("tick ");
    Serial.print(counter++);
    Serial.print("  millis=");
    Serial.println(millis());

    delay(1000);
}
