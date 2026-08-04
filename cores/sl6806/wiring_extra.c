/*
 * wiring_extra.c - the rest of the Arduino digital API.
 *
 * shiftOut(), shiftIn(), pulseIn() and pulseInLong() are written entirely in
 * terms of digitalWrite/digitalRead/micros, so they are as real as GPIO is:
 * the moment a variant can drive a pin - through a register table or through
 * the vendor back end - these work too, with no further reverse engineering.
 * They are also the reason a bit-banged SPI or one-wire sensor is possible on
 * this chip before any peripheral driver exists.
 *
 * tone(), noTone(), attachInterrupt() and detachInterrupt() are the opposite
 * case: they need hardware nobody has located. They report once and return,
 * for the same reason digitalWrite() does - a tone() that silently does
 * nothing looks like a wiring fault.
 */

#include "Arduino.h"
#include "hal_gpio.h"
#include "sl6806_console.h"

void shiftOut(uint8_t dataPin, uint8_t clockPin, uint8_t bitOrder, uint8_t val)
{
    uint8_t i;

    for (i = 0; i < 8; i++) {
        uint8_t b = (bitOrder == LSBFIRST) ? (uint8_t)(val >> i)
                                           : (uint8_t)(val >> (7 - i));
        digitalWrite(dataPin, b & 1);
        digitalWrite(clockPin, HIGH);
        digitalWrite(clockPin, LOW);
    }
}

uint8_t shiftIn(uint8_t dataPin, uint8_t clockPin, uint8_t bitOrder)
{
    uint8_t value = 0;
    uint8_t i;

    for (i = 0; i < 8; i++) {
        digitalWrite(clockPin, HIGH);
        if (digitalRead(dataPin) == HIGH)
            value |= (bitOrder == LSBFIRST) ? (uint8_t)(1u << i)
                                            : (uint8_t)(1u << (7 - i));
        digitalWrite(clockPin, LOW);
    }
    return value;
}

/*
 * Both pulse functions time with micros(). The Arduino AVR core has two
 * versions because its cycle-counting one is more precise than its
 * interrupt-safe one; here there is only micros(), so pulseInLong() is the
 * same function and exists for source compatibility.
 *
 * Accuracy is bounded by micros(), which is bounded by F_CPU being right -
 * see the calibration note in README.md. A wrong F_CPU scales every result
 * by one constant factor.
 */
static uint32_t pulse_in(uint8_t pin, uint8_t state, uint32_t timeout)
{
    uint32_t start = micros();
    uint32_t begin;

    /* Wait out any pulse already in progress. */
    while (digitalRead(pin) == state)
        if (micros() - start > timeout)
            return 0;

    /* Wait for the pulse to start. */
    while (digitalRead(pin) != state)
        if (micros() - start > timeout)
            return 0;

    begin = micros();

    /* Wait for it to end. */
    while (digitalRead(pin) == state)
        if (micros() - start > timeout)
            return 0;

    return micros() - begin;
}

uint32_t pulseIn(uint8_t pin, uint8_t state, uint32_t timeout)
{
    return pulse_in(pin, state, timeout);
}

uint32_t pulseInLong(uint8_t pin, uint8_t state, uint32_t timeout)
{
    return pulse_in(pin, state, timeout);
}

void tone(uint8_t pin, unsigned int frequency, unsigned long duration)
{
    (void)pin;
    (void)frequency;
    (void)duration;
    /*
     * tone() needs a timer with a compare output. The SL6806 has a
     * timer-shaped block at 0x40009000 - three registers per channel at a
     * 0x20 stride, per-channel interrupt flags with callbacks - but its
     * registers are not decoded and its input clock is unknown, so a tone
     * generated from it would be at an unknown frequency, which is worse
     * than none.
     */
    sl6806_gpio_unconfigured("tone()");
}

void noTone(uint8_t pin)
{
    (void)pin;
}

void attachInterrupt(uint8_t pin, void (*handler)(void), int mode)
{
    (void)pin;
    (void)handler;
    (void)mode;
    /*
     * Pin change interrupts need the pad controller's interrupt registers,
     * which are part of the same unfound GPIO block. Note that the vendor
     * back end cannot help here either: the ROM routines the stock firmware
     * calls only write and configure pads, and nothing in the application
     * registers a pin-change callback.
     */
    sl6806_gpio_unconfigured("attachInterrupt()");
}

void detachInterrupt(uint8_t pin)
{
    (void)pin;
}
