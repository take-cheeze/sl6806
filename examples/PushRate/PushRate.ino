/*
 * PushRate - find out what limits a full-screen push, before writing DMA.
 *
 * WHY THIS EXISTS. examples/BigBuffer measured one number on hardware:
 * a 240x296 RGB565 frame reaches the glass in 167348 us, which is 829 KiB/s
 * and 5 fps. That number says the polled bus is slow. It does not say what
 * makes it slow, and the two possible answers point at opposite fixes:
 *
 *   - The controller is still draining the FIFO when the CPU comes back for
 *     the next transfer. Then the panel's own clock is the ceiling, DMA would
 *     free the CPU but not raise the frame rate, and the thing to change is
 *     the module clock.
 *
 *   - The controller is finished before the CPU can issue the next transfer.
 *     Then the ceiling is the per-transfer software cost - clear the status,
 *     write the FIFO, set GO, poll - and handing the controller a command
 *     list (the DMA path in sl6806_lcdc.h) is worth a large multiple.
 *
 * The driver already counts what separates them. Every completion wait
 * records how many polls it took, and sl6806_lcdc_stats() reports the
 * minimum, maximum and last over a run. A transfer that completes on the
 * first poll spent no time waiting on the panel; one that takes hundreds
 * spent all of it there.
 *
 *     make SKETCH=examples/PushRate upload
 *     tools/sl6806-monitor build/PushRate.sym
 *
 * The second half then sweeps the module clock selector, because whichever
 * answer the polls give, "is 0x910 the fastest this controller runs" is
 * unanswered and cheap to ask. The header comment in sl6806_lcdc.h records
 * that the bootloader uses 0x310 and the application 0x910, and calls the
 * bootloader's the slower of the two - but that is an inference from which
 * code writes which value, not a measurement. This measures.
 *
 * THE SWEEP IS OFF BY DEFAULT, AND THAT IS A MEASURED CONSTRAINT.
 *
 * 2026-08-13, on hardware: with the sweep on, the device left the USB bus
 * mid-run and came back with its bulk endpoint out of sync - the next inquiry
 * answered with a stale CSW ahead of the data, and it took a USBDEVFS_RESET
 * to resynchronise. Recoverable, but it cost the run.
 *
 * The arithmetic that was wrong: a clock the controller does not answer costs
 * POLL_LIMIT polls, about 19 ms, PER TRANSFER - and a "single transfer" probe
 * is not single. Every push first sends the window commands, so a dead
 * candidate burns that 19 ms five or six times over, plus once more in the
 * wait_idle() inside begin(). That is ~120 ms per dead candidate, not 19, and
 * eighteen candidates is nearly two seconds with setup() holding the boot
 * ROM's USB callback. The bus does not wait that long.
 *
 * So the default build measures only the frame, which is 167 ms and is known
 * to be survivable. Turn the sweep on deliberately, knowing it may cost a
 * reset:
 *
 *     make SKETCH=examples/PushRate EXTRA_FLAGS=-DPUSHRATE_SWEEP=1 upload
 */

/* 1 to sweep the module clock selector. Read the paragraph above first. */
#ifndef PUSHRATE_SWEEP
#define PUSHRATE_SWEEP 0
#endif

#include <Arduino.h>
#include "sl6806_pwm.h"
#include "sl6806_lcdc.h"
#include "sl6806_cru.h"
#include "sl6806_mmio.h"

#define FB_W 240
#define FB_H 296
static sl6806_color_t framebuffer[FB_W * FB_H];

#if PUSHRATE_SWEEP
/* The sweep times a band rather than a frame. A frame is 167 ms and setup()
 * runs inside the boot ROM's USB callback, so eighteen frames back to back is
 * three seconds with the device off the bus - which is how a session ends.
 * 32 rows is 15360 bytes, about 18 ms at the measured rate, and eighteen of
 * those is a third of a second: the same order as the heap walk in BigBuffer,
 * which the device already survives.
 *
 * It also keeps every individual measurement far inside one SysTick wrap
 * (262 ms at 64 MHz), and micros() is called either side of each one, so the
 * wrap accumulator stays honest across the whole sweep.
 */
#define BAND_ROWS 32
#define BAND_BYTES ((uint32_t)FB_W * BAND_ROWS * 2u)
#endif /* PUSHRATE_SWEEP */

static void draw_gradient(void)
{
    int16_t x, y;

    for (y = 0; y < FB_H; y++)
        for (x = 0; x < FB_W; x++)
            framebuffer[y * FB_W + x] =
                SL6806_RGB((x * 255) / FB_W, (y * 255) / FB_H, 128);
}

/*
 * What one register access costs.
 *
 * The frame time divided by the transfer count says 18.8 us per 16 bytes,
 * which is about 1200 cycles at 64 MHz, and a transfer is only about a dozen
 * register accesses. Either those accesses are extremely slow or the time is
 * going somewhere else, and guessing which would be exactly the kind of
 * plausible-looking wrong number this branch keeps having to retract.
 *
 * So: time a lot of reads of the LCDC status register, and time the same loop
 * reading SRAM instead. The difference is what crossing to the peripheral
 * costs, with the loop's own overhead subtracted out rather than assumed
 * negligible.
 *
 * The status register is chosen because it is the one the completion wait
 * polls, and reading it has no side effects - the driver only ever clears
 * bits in it by writing.
 */
#define MMIO_REPS 4000u

static volatile uint32_t sink;
static volatile uint32_t sram_probe = 0x12345678u;

static uint32_t time_reads_mmio(void)
{
    uint32_t i, t0;

    t0 = micros();
    for (i = 0; i < MMIO_REPS; i++)
        sink = sl6806_mmio_read(SL6806_LCDC_BASE + SL6806_LCDC_STATUS);
    return micros() - t0;
}

static uint32_t time_reads_sram(void)
{
    uint32_t i, t0;

    t0 = micros();
    for (i = 0; i < MMIO_REPS; i++)
        sink = sram_probe;
    return micros() - t0;
}

/* Print `total` ns per access, as ns and as cycles, from a microsecond
 * elapsed over MMIO_REPS iterations. Integer only: no float formatting in a
 * payload this size. */
static void print_per_access(const char *label, uint32_t dt_us)
{
    uint32_t ns = (uint32_t)((uint64_t)dt_us * 1000u / MMIO_REPS);

    Serial.print("  ");
    Serial.print(label);
    Serial.print(dt_us);
    Serial.print(" us total, ");
    Serial.print(ns);
    Serial.print(" ns each (~");
    Serial.print((uint32_t)((uint64_t)dt_us * (F_CPU / 1000000u) / MMIO_REPS));
    Serial.println(" cycles)");
}

static void print_stats(void)
{
    const sl6806_lcdc_stats_t *s = sl6806_lcdc_stats();

    Serial.print("  transfers ");
    Serial.print(s->transfers);
    Serial.print("  timeouts ");
    Serial.print(s->timeouts);
    Serial.print("  polls min ");
    Serial.print(s->min_polls);
    Serial.print(" max ");
    Serial.print(s->max_polls);
    Serial.print(" last ");
    Serial.print(s->last_polls);
    Serial.print("  busy_waits ");
    Serial.println(s->busy_waits);
}

#if PUSHRATE_SWEEP
/*
 * Time one band. Returns microseconds, or 0 if any transfer in it gave up -
 * a clock setting that stops the controller answering must not be reported
 * as a speed.
 */
static uint32_t time_band(void)
{
    uint32_t t0, dt;

    sl6806_lcdc_stats_reset();
    sl6806_lcdc_timed_out = 0;

    t0 = micros();
    Screen.display(0, 0, FB_W, BAND_ROWS);
    dt = micros() - t0;

    if (sl6806_lcdc_timed_out)
        return 0;
    return dt ? dt : 1;
}

/*
 * One transfer, as a liveness probe before committing to a band.
 *
 * A dead clock costs POLL_LIMIT polls per transfer, which is about 19 ms.
 * That is tolerable once; it is not tolerable 480 times, which is what a
 * band of 32 rows would cost. So each candidate gets a single 16-byte
 * transfer first, and only a controller that answers it gets timed.
 */
static bool responds(void)
{
    sl6806_lcdc_timed_out = 0;
    Screen.display(0, 0, 8, 1);
    return !sl6806_lcdc_timed_out;
}

static void report_band(uint32_t dt)
{
    if (!dt) {
        Serial.println("no response");
        return;
    }
    Serial.print(dt);
    Serial.print(" us  ");
    Serial.print((uint32_t)(BAND_BYTES * 1000000ull / dt / 1024u));
    Serial.print(" KiB/s  (frame ");
    Serial.print((uint32_t)((uint64_t)dt * FB_H / BAND_ROWS / 1000u));
    Serial.println(" ms)");
}
#endif /* PUSHRATE_SWEEP */

void setup()
{
    sl6806_lcdc_config_t orig;
    uint32_t dt;
#if PUSHRATE_SWEEP
    sl6806_lcdc_config_t cfg;
    uint32_t best_dt = 0;
    uint32_t best_modclk = 0;
    uint8_t n;
#endif

    Serial.begin(115200);
    Serial.println();
    Serial.println("=== SL6806 push rate ===");

    if (sl6806_backlight_begin(100))
        Serial.println("backlight on");

    if (!Screen.begin(framebuffer, FB_W, FB_H)) {
        Serial.println("no panel; nothing to measure");
        return;
    }
    draw_gradient();

    /*
     * Copy by value. sl6806_lcdc_config() hands back a pointer into the
     * driver's own storage, and begin() overwrites that storage, so keeping
     * the pointer would leave nothing to restore from at the end.
     */
    {
        const sl6806_lcdc_config_t *p = sl6806_lcdc_config();
        if (!p) {
            Serial.println("no lcdc config; is this an LCDC board?");
            return;
        }
        orig = *p;
    }

    Serial.print("module clock 0x");
    Serial.print(orig.modclk, HEX);
    Serial.println(" (as configured by the variant)");
    Serial.println();

    /* ---- 0. what a register access costs ---- */

    Serial.println("register access cost:");
    print_per_access("mmio  ", time_reads_mmio());
    print_per_access("sram  ", time_reads_sram());
    Serial.println("  (sram is the same loop without the peripheral: subtract it)");
    Serial.println();

    /* ---- 1. what limits a full frame ---- */

    Serial.println("one full frame, with poll counts:");
    sl6806_lcdc_stats_reset();
    {
        uint32_t t0 = micros();
        Screen.display();
        dt = micros() - t0;
    }
    Serial.print("  ");
    Serial.print(dt);
    Serial.print(" us");
    if (dt >= 262000u) {
        Serial.println("  (>= one SysTick wrap - a lower bound)");
    } else {
        Serial.print("   ");
        Serial.print((uint32_t)((uint64_t)FB_W * FB_H * 2 * 1000000u / dt / 1024u));
        Serial.println(" KiB/s");
    }
    print_stats();
    Serial.println();
    Serial.println("  min_polls 0 means the controller was already done when");
    Serial.println("  the CPU looked: software is the ceiling, and DMA pays.");
    Serial.println("  A large minimum means the panel clock is the ceiling.");
    Serial.println();

    /* ---- 2. is 0x910 the fastest the controller runs ---- */

#if PUSHRATE_SWEEP
    Serial.print("module clock sweep, ");
    Serial.print(BAND_ROWS);
    Serial.println(" rows each:");

    for (n = 0; n < 16; n++) {
        cfg = orig;
        cfg.modclk = ((uint32_t)n << 8) | 0x10u;

        Serial.print("  0x");
        Serial.print(cfg.modclk, HEX);
        Serial.print("  ");

        if (sl6806_lcdc_begin(&cfg) != 0) {
            Serial.println("begin failed");
            continue;
        }
        if (!responds()) {
            Serial.println("no response");
            continue;
        }
        dt = time_band();
        report_band(dt);

        if (dt && (!best_dt || dt < best_dt)) {
            best_dt = dt;
            best_modclk = cfg.modclk;
        }
    }

    /*
     * Bit 4 is inside the field mask (0xF10) and both values anybody has seen
     * set it, so it has never been varied. Two more measurements settle
     * whether it does anything, without opening the sweep to 32 entries.
     */
    Serial.println("  and with bit 4 clear, for the two known values:");
    for (n = 0; n < 2; n++) {
        cfg = orig;
        cfg.modclk = n ? 0x900u : 0x300u;

        Serial.print("  0x");
        Serial.print(cfg.modclk, HEX);
        Serial.print("  ");

        if (sl6806_lcdc_begin(&cfg) != 0) {
            Serial.println("begin failed");
            continue;
        }
        if (!responds()) {
            Serial.println("no response");
            continue;
        }
        dt = time_band();
        report_band(dt);

        if (dt && (!best_dt || dt < best_dt)) {
            best_dt = dt;
            best_modclk = cfg.modclk;
        }
    }

    /* ---- 3. put it back, and confirm the restore works ---- */

    Serial.println();
    if (sl6806_lcdc_begin(&orig) != 0) {
        Serial.println("RESTORE FAILED - power cycle before trusting the panel");
        return;
    }

    Serial.print("fastest was 0x");
    Serial.print(best_modclk, HEX);
    Serial.print(" at ");
    Serial.print(best_dt);
    Serial.print(" us/band; variant uses 0x");
    Serial.println(orig.modclk, HEX);

    Serial.println("restored; one more full frame to prove the panel still works:");
    sl6806_lcdc_stats_reset();
    {
        uint32_t t0 = micros();
        Screen.display();
        dt = micros() - t0;
    }
    Serial.print("  ");
    Serial.print(dt);
    Serial.println(" us");
    print_stats();
#else
    Serial.println("module clock sweep: not built in (PUSHRATE_SWEEP=0)");
    Serial.println("  it can cost a USB resynchronise - see the top of this file");
#endif /* PUSHRATE_SWEEP */
}

void loop(void)
{
    /* Nothing: the boot ROM's cb2 does not fire on this part, so everything
     * above runs in setup() by design rather than by accident. */
}
