/*
 * main.cpp - the setup()/loop() runner, shared by payload and firmware builds.
 */

#include "Arduino.h"
#include "HardwareSerial.h"

extern "C" void sl6806_run_setup(void)
{
    sl6806_time_init();
    Serial.begin();
    setup();
}

extern "C" void sl6806_run_loop(void)
{
    loop();

    /* Keeps the timekeeping wrap accumulator fed even if the sketch never
     * calls a timing function. See wiring_time.c. */
    (void)sl6806_cycles();
}

/* A sketch that only defines setup() is legal, as on Arduino. */
extern "C" __attribute__((weak)) void loop(void) {}

/* ------------------------------------------------------------------ */
/* misc wiring                                                         */
/* ------------------------------------------------------------------ */

extern "C" long map(long x, long in_min, long in_max, long out_min, long out_max)
{
    long divisor = in_max - in_min;

    if (divisor == 0)
        return out_min;   /* Arduino divides by zero here; we do not. */
    return (x - in_min) * (out_max - out_min) / divisor + out_min;
}

extern "C" void randomSeed(unsigned long seed)
{
    if (seed)
        srand(seed);
}

extern "C" long sl6806_random(long howbig)
{
    if (howbig <= 0)
        return 0;
    return rand() % howbig;
}

extern "C" long sl6806_random_range(long howsmall, long howbig)
{
    if (howsmall >= howbig)
        return howsmall;
    return sl6806_random(howbig - howsmall) + howsmall;
}

extern "C" void sl6806_panic(const char *msg)
{
    Serial.println();
    Serial.print("*** SL6806 PANIC: ");
    Serial.println(msg);
    Serial.flush();

    /* Stay alive so the monitor can still read the ring buffer out. */
    sl6806_disable_irq();
    for (;;)
        ;
}
