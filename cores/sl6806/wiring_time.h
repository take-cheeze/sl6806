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

/* Modulus-1 of the hardware counter that ended up backing sl6806_cycles():
 * 0xFFFFFFFF for the DWT cycle counter, 0x00FFFFFF for the SysTick fallback,
 * and 0 if neither of them advances - in which case there is no time source
 * at all. Divide by the clock to get the wrap period the rule above refers
 * to. */
uint32_t sl6806_tick_mask(void);

/* The last raw hardware counter value read, whatever the source. Reported to
 * the host in the status query purely so a dead counter can be told apart
 * from an absent one: a register that reads a constant is a different fault
 * from one that reads zero. */
uint32_t sl6806_tick_raw(void);

/*
 * Called once if neither counter advances. Weak - override to change or
 * silence the reporting.
 *
 * When this fires, millis()/micros() are frozen and delay() returns without
 * waiting. That is deliberate. A delay implemented as "spin until the counter
 * reaches a target" cannot terminate when the counter never moves, and in
 * RUN_MODE=poll it spins inside the boot ROM's USB handler - which takes the
 * device off the bus until it is unplugged. Wrong timing is recoverable; a
 * wedged link in the middle of a bench session is not.
 */
void     sl6806_no_clock(void);

uint32_t millis(void);
uint32_t micros(void);
void     delay(uint32_t ms);
void     delayMicroseconds(uint32_t us);
void     yield(void);

/*
 * Cap on how long delay() and delayMicroseconds() may block, in
 * milliseconds. 0 (the default) means no cap.
 *
 * This exists because RUN_MODE=poll drives loop() from inside the boot ROM's
 * USB command handler. Blocking there past the host's SCSI timeout does not
 * merely slow the link down - it desynchronises the endpoint, and the device
 * stops answering anything at all until it is unplugged. A delay(1000) is
 * enough to do it.
 *
 * When a cap is set, a longer delay is shortened and reported once rather
 * than silently obeyed or silently ignored. Pace loop() with millis() instead
 * if the timing matters.
 */
void     sl6806_set_block_limit(uint32_t ms);
uint32_t sl6806_block_limit(void);

/* Called once when a delay is clamped. Weak - override to change or silence
 * the reporting. */
void     sl6806_block_clamped(uint32_t asked_ms, uint32_t capped_ms);

#ifdef __cplusplus
}
#endif

#endif /* SL6806_WIRING_TIME_H */
