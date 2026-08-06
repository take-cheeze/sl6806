/*
 * sl6806_stat.h - the device's answer to "how fast are you actually running?"
 *
 * WHY THIS EXISTS
 * The PLL registers on this SoC have not been found, so the only way to learn
 * the real clock is to measure it against a clock that is known - and the only
 * such clock in the system is the host's. That is what this is for, and it
 * worked: 64.000 MHz on one P20 Player, now the F_CPU default.
 *
 * The obvious sketch-side approach does not work here. A sketch that delays a
 * known number of seconds and asks you to time it with a stopwatch blocks for
 * that whole time, and in payload mode the boot ROM cannot service USB while
 * _start has not returned. Thirty seconds of that is the same hazard that
 * wedges the link, only longer. (That was examples/ClockCalibrate, now
 * deleted; see git history.)
 *
 * So the measurement is inverted. The device never times anything: it only
 * reports its own free-running cycle counter, on demand, from inside the USB
 * command handler. The host stamps each answer with real wall-clock time and
 * regresses device cycles against host seconds. The device's contribution is
 * one counter read per round trip, which cannot block anything, and it works
 * in every run mode that keeps USB alive.
 *
 * HOW IT IS ADDRESSED
 * Same mechanism as the console: a sentinel "address" passed to the vendor
 * read command that means "answer this query" rather than "read this memory".
 * See sl6806_console.h for the full description of that trick. Nothing custom
 * is needed on the ROM side and no address is hardcoded on the host side
 * except the sentinel itself.
 *
 * WHAT THE READ DOES NOT DO
 * It is deliberately free of side effects beyond sampling the counter: it does
 * *not* drive loop(), even in RUN_MODE=poll. Calibration polls must all cost
 * the same, or the varying work would land in the regression as jitter. Use
 * the console poll when you want the sketch to advance.
 *
 * Sampling the counter is itself worth having: sl6806_cycles() must be called
 * at least once per hardware-counter wrap or elapsed time is lost (see
 * wiring_time.c). A host polling this keeps the accumulator honest even in
 * hook mode on a ROM whose idle callback never fires.
 */
#ifndef SL6806_STAT_H
#define SL6806_STAT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Sentinel "address" meaning "report status". Spelled "STAT" in hex, the same
 * convention as the console's "CONS", and far outside any valid SL6806
 * address - ROM ends at 0x0007D000, SRAM at 0x00840000, peripherals at
 * 0x41000000 - so a genuine read can never collide with it.
 */
#define SL6806_STAT_ADDR  0x53544154u

/* "STAT" in memory order, so it is legible in a USB trace. Distinct from the
 * console magic: a stale console packet must not read as a valid status. */
#define SL6806_STAT_MAGIC   0x54415453u
#define SL6806_STAT_VERSION 2u

/* Run modes, as reported in run_mode below. These are the values the Makefile
 * passes as -DSL6806_RUN_MODE; they live here because the host needs the
 * mapping too. TAKEOVER never appears in a reply - it registers no handler. */
#define SL6806_RUN_HOOK     0
#define SL6806_RUN_TAKEOVER 1
#define SL6806_RUN_POLL     2

/*
 * The reply. Fixed 40-byte layout, decoded on the host with a struct format,
 * so the field order and the absence of padding are part of the contract -
 * startup_payload.c asserts both at compile time.
 *
 * cycles is split into two 32-bit halves rather than declared uint64_t so
 * that no alignment rule anywhere can insert padding ahead of it.
 */
typedef struct {
    uint32_t magic;       /* SL6806_STAT_MAGIC                              */
    uint32_t version;     /* SL6806_STAT_VERSION                            */
    uint32_t cycles_lo;   /* sl6806_cycles(), sampled inside the handler    */
    uint32_t cycles_hi;
    uint32_t polls;       /* vendor reads served since _start               */
    uint32_t loops;       /* loop() calls made since _start                 */
    uint32_t f_cpu;       /* the F_CPU this image was *built* with          */
    uint32_t run_mode;    /* SL6806_RUN_*                                   */
    /*
     * Modulus-1 of the hardware counter behind sl6806_cycles(): 0xFFFFFFFF
     * for the DWT cycle counter, 0x00FFFFFF for the SysTick fallback, and
     * **0 if neither of them advances**, which is a real outcome on this part
     * and not a theoretical one. The host needs it to know how long it may go
     * between polls before a wrap passes unobserved and elapsed time is
     * silently lost - at 24 bits that is a fraction of a second.
     */
    uint32_t tick_mask;

    /*
     * The raw counter, or - when tick_mask is 0 - what the register read
     * during the probe. That distinction is the whole reason this field
     * exists: a counter stuck at a constant nonzero value means CYCCNT is not
     * implemented and the register reads junk, while a constant zero means it
     * is implemented but never clocked. Both present identically as "time
     * does not advance", and they call for different fixes.
     */
    uint32_t raw;
} sl6806_stat_t;

#ifdef __cplusplus
}
#endif

#endif /* SL6806_STAT_H */
