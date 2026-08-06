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

/*
 * CLOCK CALIBRATION SIDE-CHANNEL
 *
 * F_CPU is a build-time guess. Every millis(), micros() and delay() is wrong
 * by exactly the ratio between that guess and the truth, so one measurement
 * fixes all timing at once - but measuring it on the device means blocking for
 * tens of seconds, and in payload mode blocking is what wedges the USB link.
 *
 * So the measurement is done from the host instead. Every console poll stamps
 * a snapshot of sl6806_cycles() here, taken *inside* the USB transaction the
 * host is timing. tools/sl6806-clockcal collects those stamps against its own
 * wall clock and regresses one against the other; the slope is the real clock.
 * The device blocks for nothing and any sketch can be calibrated, because the
 * stamping lives in the core rather than in a sketch.
 *
 * There is no locking and none is needed: the device only ever writes this
 * from inside a USB command, and the host only ever reads it from a different
 * USB command, so the two can never overlap.
 *
 * Like the console ring this lives in .bss - the linker picks the address and
 * the build exports it to build/<sketch>.sym. Nothing is hardcoded.
 */
#define SL6806_CLOCKSTAMP_MAGIC 0x4B4C4353u   /* "SCLK" little-endian */

typedef struct {
    uint32_t magic;              /* SL6806_CLOCKSTAMP_MAGIC once time_init ran */
    uint32_t f_cpu;              /* the F_CPU this build was compiled with     */
    volatile uint32_t seq;       /* stamps taken so far; proves it is live     */
    volatile uint32_t cyc_lo;    /* sl6806_cycles() at the last stamp, low     */
    volatile uint32_t cyc_hi;    /* ... and high                               */
} sl6806_clockstamp_t;

extern sl6806_clockstamp_t _sl6806_clockstamp;

/* Take a stamp. Called from the console poll handler; a sketch does not need
 * to call it. Reading the counter here also keeps the wrap accumulator honest
 * for free whenever a host is polling. */
void     sl6806_time_stamp(void);

#ifdef __cplusplus
}
#endif

#endif /* SL6806_WIRING_TIME_H */
