/*
 * Blink - the traditional first sketch.
 *
 * HEADS UP: LED_BUILTIN is a placeholder. Nobody has identified the GPIO
 * registers on the SL6806 yet, so this sketch will report
 *
 *     *** SL6806: GPIO is not configured ***
 *
 * on the serial monitor instead of blinking anything. That is the sketch
 * working correctly - it is telling you the driver data is missing rather
 * than failing silently. The timing and the Serial output below are real.
 *
 * See cores/sl6806/hal_gpio.h for how to find the registers, and
 * variants/p20_player/variant.c for where to put them once you have.
 */

#include <Arduino.h>

void setup()
{
    Serial.begin(115200);
    Serial.println("SL6806 Blink");

    if (!sl6806_gpio_available())
        Serial.println("(GPIO unconfigured - timing still runs, see the note above)");

    pinMode(LED_BUILTIN, OUTPUT);
}

void loop()
{
    digitalWrite(LED_BUILTIN, HIGH);
    delay(500);

    digitalWrite(LED_BUILTIN, LOW);
    delay(500);

    Serial.print("uptime ");
    Serial.print(millis() / 1000);
    Serial.println("s");
}
