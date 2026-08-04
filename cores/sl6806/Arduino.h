/*
 * Arduino.h - main header for the SL6806 Arduino-like framework.
 *
 * Include this and write setup() / loop() as usual.
 *
 * What actually works on this chip today is summarised in README.md; the
 * short version is that timing, Serial, the C/C++ runtime and the flash
 * image format are real, while GPIO/ADC/PWM need a register map that has not
 * been reverse engineered yet.
 */
#ifndef ARDUINO_H
#define ARDUINO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#include "sl6806.h"
#include "wiring_time.h"
#include "hal_gpio.h"
#include "sl6806_console.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- pin/level constants ---- */
#define LOW             0
#define HIGH            1

#define INPUT           0
#define OUTPUT          1
#define INPUT_PULLUP    2
#define INPUT_PULLDOWN  3

#define LSBFIRST        0
#define MSBFIRST        1

#define CHANGE          1
#define FALLING         2
#define RISING          3

#ifndef PI
#define PI              3.1415926535897932384626433832795
#define HALF_PI         1.5707963267948966192313216916398
#define TWO_PI          6.283185307179586476925286766559
#define DEG_TO_RAD      0.017453292519943295769236907684886
#define RAD_TO_DEG      57.295779513082320876798154814105
#define EULER           2.718281828459045235360287471352
#endif

/* ---- digital / analog I/O ---- */
void pinMode(uint8_t pin, uint8_t mode);
void digitalWrite(uint8_t pin, uint8_t value);
int  digitalRead(uint8_t pin);
int  analogRead(uint8_t pin);
void analogWrite(uint8_t pin, int value);
void analogReadResolution(uint8_t bits);
void analogWriteResolution(uint8_t bits);

/* ---- bit helpers ---- */
#define bitRead(value, bit)         (((value) >> (bit)) & 0x01)
#define bitSet(value, bit)          ((value) |= (1UL << (bit)))
#define bitClear(value, bit)        ((value) &= ~(1UL << (bit)))
#define bitToggle(value, bit)       ((value) ^= (1UL << (bit)))
#define bitWrite(value, bit, b)     ((b) ? bitSet(value, bit) : bitClear(value, bit))
#define bit(b)                      (1UL << (b))

#define lowByte(w)                  ((uint8_t)((w) & 0xff))
#define highByte(w)                 ((uint8_t)((w) >> 8))

/* ---- misc ---- */
long map(long x, long in_min, long in_max, long out_min, long out_max);
void randomSeed(unsigned long seed);

/* newlib already declares `long random(void)`, so the Arduino forms cannot be
 * extern "C" without clashing with it. These are the C-callable versions;
 * C++ gets random()/random(a,b) as overloads below. */
long sl6806_random(long max);
long sl6806_random_range(long min, long max);

/* Halt with a message on the console. Used by the fault handlers. */
void sl6806_panic(const char *msg) __attribute__((noreturn));

/* Sketch entry points. */
void setup(void);
void loop(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#ifdef __cplusplus

/* Overloads, not extern "C" declarations - see the note above. These sit
 * alongside newlib's random(void) rather than replacing it. */
inline long random(long max)           { return sl6806_random(max); }
inline long random(long min, long max) { return sl6806_random_range(min, max); }
inline long random(int max)            { return sl6806_random(max); }
inline long random(int min, int max)   { return sl6806_random_range(min, max); }

template <typename T> inline const T &sl_min(const T &a, const T &b) { return a < b ? a : b; }
template <typename T> inline const T &sl_max(const T &a, const T &b) { return a > b ? a : b; }

#ifndef min
#define min(a, b) sl_min(a, b)
#endif
#ifndef max
#define max(a, b) sl_max(a, b)
#endif

#include "Print.h"
#include "Stream.h"
#include "WString.h"
#include "HardwareSerial.h"
#include "gfx/Framebuffer.h"
#include "gfx/Panel.h"
#include "gfx/Display.h"

#else /* C */

#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef max
#define max(a, b) ((a) > (b) ? (a) : (b))
#endif

#endif /* __cplusplus */

#ifndef abs
#define abs(x) ((x) > 0 ? (x) : -(x))
#endif
#ifndef constrain
#define constrain(x, l, h) ((x) < (l) ? (l) : ((x) > (h) ? (h) : (x)))
#endif
#ifndef sq
#define sq(x) ((x) * (x))
#endif

#include "variant.h"

#endif /* ARDUINO_H */
