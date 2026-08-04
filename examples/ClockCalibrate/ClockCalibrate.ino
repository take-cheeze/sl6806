/*
 * ClockCalibrate - measure the real CPU clock.
 *
 * WHY YOU NEED THIS
 * The SL6806's clock frequency is not documented and has not been recovered
 * from the firmware, so F_CPU defaults to a placeholder 120 MHz. Every
 * delay() and millis() is wrong by exactly the ratio between that guess and
 * reality - if the chip actually runs at 60 MHz, delay(1000) waits two
 * seconds. It is a single scale factor, so one measurement fixes everything.
 *
 * HOW TO USE IT
 *   1. make SKETCH=examples/ClockCalibrate run
 *   2. Start a stopwatch when you see "GO", stop it at "DONE".
 *   3. Feed the real elapsed time into the formula it prints.
 *   4. Rebuild everything with that value:
 *        make SKETCH=examples/Blink F_CPU=<measured> upload
 *      and put it in your Makefile invocation from then on.
 *
 * A phone stopwatch over 30 seconds is good to about 1%, which is plenty.
 * If you later find the PLL registers, you can compute this exactly instead.
 */

#include <Arduino.h>

static const uint32_t TEST_SECONDS = 30;

void setup()
{
    Serial.begin(115200);

    Serial.println();
    Serial.println("=== SL6806 clock calibration ===");
    Serial.print("assuming F_CPU = ");
    Serial.print((uint32_t)F_CPU);
    Serial.println(" Hz");
    Serial.println();
    Serial.println("Start a stopwatch on GO and stop it on DONE.");
    Serial.println();

    delay(2000);

    Serial.println(">>> GO");
    uint64_t start = sl6806_cycles();

    /* Deliberately delay() rather than spin: this measures the same path the
     * sketches use, so a mistake anywhere in it shows up here too. */
    for (uint32_t i = 0; i < TEST_SECONDS; i++) {
        delay(1000);
        Serial.print(".");
    }

    uint64_t elapsed = sl6806_cycles() - start;
    Serial.println();
    Serial.println(">>> DONE");
    Serial.println();

    Serial.print("believed elapsed : ");
    Serial.print(TEST_SECONDS);
    Serial.println(" s");
    Serial.print("cycles counted   : ");
    Serial.println((unsigned long)(elapsed / 1000ULL));
    Serial.println("  (thousands of cycles)");
    Serial.println();

    Serial.println("Now compute the real clock:");
    Serial.println();
    Serial.print("  F_CPU_real = ");
    Serial.print((uint32_t)F_CPU);
    Serial.print(" * ");
    Serial.print(TEST_SECONDS);
    Serial.println(" / <seconds you measured>");
    Serial.println();
    Serial.println("Then rebuild with:  make F_CPU=<that number> ...");
    Serial.println();
    Serial.println("If your stopwatch said close to 30 s, the default guess");
    Serial.println("was right and nothing needs changing.");
}

void loop()
{
}
