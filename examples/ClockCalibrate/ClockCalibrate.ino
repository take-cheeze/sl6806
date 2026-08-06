/*
 * ClockCalibrate - measure the real CPU clock, and check the result.
 *
 * WHY YOU NEED THIS
 * The SL6806's clock frequency is not documented and has not been recovered
 * from the firmware, so F_CPU defaults to a placeholder 120 MHz. Every
 * delay() and millis() is wrong by exactly the ratio between that guess and
 * reality - if the chip actually runs at 60 MHz, delay(1000) waits two
 * seconds. It is a single scale factor, so one measurement fixes everything.
 *
 * HOW TO USE IT
 *   1. make SKETCH=examples/ClockCalibrate RUN_MODE=poll upload
 *   2. tools/sl6806-clockcal build/ClockCalibrate.sym
 *   3. Rebuild with the number it prints:
 *        make SKETCH=examples/Blink F_CPU=<measured> upload
 *      and pass F_CPU from then on - it is a property of the chip, not of the
 *      sketch.
 *   4. Re-run this sketch to check: the seconds it counts should now match a
 *      wall clock.
 *
 * WHY THE MEASUREMENT IS NOT DONE HERE
 * The earlier version of this sketch delay()ed for thirty seconds in setup()
 * while you held a stopwatch. That cannot work in payload mode: the boot ROM
 * cannot service USB while the sketch is running, so a sketch that blocks for
 * thirty seconds takes the USB link down with it and the device stops
 * answering until it is unplugged. A one-second delay already did exactly
 * that to a real unit.
 *
 * So the timing lives on the host. The core stamps its cycle counter on every
 * console poll, inside the USB transaction the host is timing, and
 * tools/sl6806-clockcal fits a line through those stamps. The device never
 * blocks, and because the stamping is in the core rather than in this sketch,
 * *any* sketch can be calibrated - this one just gives you something quiet to
 * calibrate against.
 *
 * All this sketch does is tick once a second so you can eyeball the result.
 */

#include <Arduino.h>

static uint32_t next_tick;
static uint32_t seconds;

void setup()
{
    Serial.begin(115200);

    Serial.println();
    Serial.println("=== SL6806 clock calibration ===");
    Serial.print("this build assumes F_CPU = ");
    Serial.print((uint32_t)F_CPU);
    Serial.println(" Hz");
    Serial.println();
    Serial.println("Measure the real one from the host, with the device left");
    Serial.println("running:");
    Serial.println();
    Serial.println("    tools/sl6806-clockcal build/ClockCalibrate.sym");
    Serial.println();
    Serial.println("Then rebuild everything with F_CPU=<what it prints>.");
    Serial.println();
    Serial.println("The count below is this chip's own idea of seconds. After");
    Serial.println("calibration it should keep up with a wall clock; before,");
    Serial.println("it is off by exactly the ratio the tool reports.");
    Serial.println();
}

/*
 * Paced with millis() rather than delay(), which is the pattern every sketch
 * should use in RUN_MODE=poll: loop() runs inside the boot ROM's USB command
 * handler, so it must return promptly. Nothing here blocks for more than the
 * time it takes to format a line.
 */
void loop()
{
    uint32_t now = millis();

    if ((int32_t)(now - next_tick) < 0)
        return;

    next_tick = now + 1000;

    Serial.print("t = ");
    Serial.print(seconds++);
    Serial.print(" s   (millis=");
    Serial.print(now);
    Serial.println(")");
}
