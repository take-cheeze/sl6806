#ifndef SL6806_WIRING_TIME_H
#define SL6806_WIRING_TIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void     sl6806_time_init(void);

/* Monotonic cycle count since sl6806_time_init(), 64-bit so it does not wrap
 * in any practical runtime. Must be read at least once per hardware-counter
 * wrap period - see the note at the top of wiring_time.c. */
uint64_t sl6806_cycles(void);

uint32_t millis(void);
uint32_t micros(void);
void     delay(uint32_t ms);
void     delayMicroseconds(uint32_t us);
void     yield(void);

#ifdef __cplusplus
}
#endif

#endif /* SL6806_WIRING_TIME_H */
