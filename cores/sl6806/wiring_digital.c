/*
 * wiring_digital.c - pinMode / digitalWrite / digitalRead.
 *
 * These are thin wrappers over hal_gpio, which is fully implemented but needs
 * the variant to supply the register table. Until it does, each call reports
 * once over Serial rather than silently doing nothing.
 */

#include "Arduino.h"
#include "hal_gpio.h"

void pinMode(uint8_t pin, uint8_t mode)
{
    switch (mode) {
    case OUTPUT:
        sl6806_gpio_set_pull(pin, 0, 0);
        sl6806_gpio_set_dir(pin, 1);
        break;
    case INPUT:
        sl6806_gpio_set_dir(pin, 0);
        sl6806_gpio_set_pull(pin, 0, 0);
        break;
    case INPUT_PULLUP:
        sl6806_gpio_set_dir(pin, 0);
        sl6806_gpio_set_pull(pin, 1, 1);
        break;
    case INPUT_PULLDOWN:
        sl6806_gpio_set_dir(pin, 0);
        sl6806_gpio_set_pull(pin, 1, 0);
        break;
    default:
        break;
    }
}

void digitalWrite(uint8_t pin, uint8_t value)
{
    sl6806_gpio_write(pin, value != LOW);
}

int digitalRead(uint8_t pin)
{
    return sl6806_gpio_read(pin) ? HIGH : LOW;
}

/*
 * The ADC and the PWM/timer blocks have not been located either. Reporting
 * is deliberate: returning a plausible 0 from analogRead() would look like a
 * grounded input rather than a missing driver.
 */
int analogRead(uint8_t pin)
{
    (void)pin;
    sl6806_gpio_unconfigured("analogRead()");
    return 0;
}

void analogWrite(uint8_t pin, int value)
{
    (void)pin;
    (void)value;
    sl6806_gpio_unconfigured("analogWrite()");
}

void analogReadResolution(uint8_t bits)
{
    (void)bits;
}

void analogWriteResolution(uint8_t bits)
{
    (void)bits;
}
