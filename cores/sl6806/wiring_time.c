/*
 * wiring_time.c - millis() / micros() / delay() for SL6806.
 *
 * WHY NOT A SYSTICK INTERRUPT:
 * In payload mode the sketch runs underneath the boot ROM, and the ROM owns
 * the vector table (VTOR points into mask ROM). Installing a SysTick handler
 * would mean relocating VTOR, which also moves the ROM's USB interrupt and
 * kills the link the sketch is loaded over. So timekeeping here is built on a
 * free-running counter that needs no interrupt at all.
 *
 * CONSEQUENCE - THE ONE RULE, AND IT IS TIGHT ON THIS PART:
 * The counter is 32-bit (DWT) or 24-bit (SysTick fallback) and wraps. Wraps
 * are accumulated in software, but only when the counter is read. You must
 * call millis(), micros(), delay() or yield() at least once per wrap period
 * or time is silently lost.
 *
 * On the one SL6806 measured, the DWT does not run and the fallback is in
 * use, so the wrap period is 2^24 / 64 MHz = **262 ms**. That is short enough
 * to matter: an ordinary loop() satisfies it, but any computation that runs
 * for a quarter of a second without touching the clock loses time. Call
 * yield() inside long work. (A working 32-bit DWT would give ~67 s at the
 * same clock and this rule would be a formality.)
 *
 * In firmware mode the sketch owns the vector table, so this restriction can
 * be lifted; see SL6806_TIME_USE_SYSTICK_IRQ in the header.
 */

#include "sl6806.h"
#include "wiring_time.h"
#include "sl6806_console.h"

/* 0 = no cap. Set by RUN_MODE=poll; see the header. */
static uint32_t block_limit_ms;

void sl6806_set_block_limit(uint32_t ms) { block_limit_ms = ms; }
uint32_t sl6806_block_limit(void) { return block_limit_ms; }

__attribute__((weak)) void sl6806_block_clamped(uint32_t asked_ms,
                                                uint32_t capped_ms)
{
    static uint8_t reported;
    char n[12];
    int i;

    if (reported)
        return;
    reported = 1;

    sl6806_debug_print(
        "\r\n*** SL6806: delay() clamped ***\r\n"
        "    A delay of ");
    /* No printf here: this can run from inside the USB handler, where the
     * whole point is to be quick. */
    for (i = 0; i < 11 && asked_ms; i++) {
        n[i] = (char)('0' + asked_ms % 10);
        asked_ms /= 10;
    }
    n[i] = 0;
    while (i--) {
        char c[2] = { n[i], 0 };
        sl6806_debug_print(c);
    }
    (void)capped_ms;
    sl6806_debug_print(
        " ms was shortened.\r\n"
        "    In RUN_MODE=poll, loop() runs inside the boot ROM's USB command\r\n"
        "    handler. Blocking there past the host's timeout wedges the\r\n"
        "    device until you unplug it, so long delays are capped.\r\n"
        "    Pace loop() with millis() instead of delay().\r\n"
        "    (reported once; further clamping is silent)\r\n\r\n");
}

/* Which counter we ended up with. tick_mask == 0 means none of them works;
 * see the note on sl6806_no_clock() below. */
static uint32_t tick_mask;      /* counter modulus - 1, or 0 for "none" */
static uint32_t last_raw;       /* previous raw up-count */
static uint64_t cycle_acc;      /* accumulated cycles */
static uint8_t  time_ready;
static uint32_t probe_raw;      /* what the counter read during the probe */

__attribute__((weak)) void sl6806_no_clock(void)
{
    static uint8_t reported;

    if (reported)
        return;
    reported = 1;

    sl6806_debug_print(
        "\r\n*** SL6806: no working cycle counter ***\r\n"
        "    Neither the DWT cycle counter nor SysTick advances on this\r\n"
        "    part, so there is no time source. millis() and micros() are\r\n"
        "    frozen at 0 and delay() returns immediately instead of\r\n"
        "    waiting.\r\n"
        "    Returning immediately is deliberate: a delay that spun on a\r\n"
        "    dead counter would never terminate, and in RUN_MODE=poll that\r\n"
        "    runs inside the boot ROM's USB handler - which wedges the\r\n"
        "    device until it is unplugged.\r\n"
        "    (reported once)\r\n\r\n");
}

/* Read the hardware counter as an up-counter. */
static inline uint32_t read_raw(void)
{
    if (tick_mask == 0xFFFFFFFFu)
        return DWT->CYCCNT;
    if (tick_mask == 0u)
        return 0;
    /* SysTick counts down; invert to get an up-count. */
    return (0x00FFFFFFu - (SysTick->VAL & 0x00FFFFFFu)) & 0x00FFFFFFu;
}

/*
 * Does this register actually count?
 *
 * WHY THIS IS NOT "IS IT NONZERO". The DWT is optional on Cortex-M4, and on a
 * part that does not implement CYCCNT - or that leaves the trace power domain
 * off - the register reads back a constant. Writing 0 and checking for
 * nonzero, which is what this used to do, calls that constant a working
 * counter. Measured on a real SL6806: the probe passed, tick_mask was set to
 * 32-bit, and sl6806_cycles() then returned 0 forever. Every delay() became
 * an infinite loop, and in poll mode that hung the device inside the ROM's
 * USB handler.
 *
 * Reading a volatile MMIO register in a loop cannot be optimised away, and
 * each read costs several cycles, so a counter that is running will differ
 * from its first value long before this gives up.
 */
static int counter_moves(volatile uint32_t *reg)
{
    uint32_t first = *reg;
    int i;

    probe_raw = first;          /* kept for the host-side status query */
    for (i = 0; i < 1000; i++)
        if (*reg != first)
            return 1;
    return 0;
}

void sl6806_time_init(void)
{
    if (time_ready)
        return;

    time_ready = 1;
    tick_mask = 0;

    /* Prefer the DWT cycle counter: 32-bit, 1-cycle resolution, free.
     *
     * TRCENA is written but deliberately not read back as a precondition.
     * Whether that bit sticks is a statement about the debug block, and the
     * only thing that actually matters is whether the counter then advances -
     * which counter_moves() answers directly. Gating on the read-back can
     * only reject a working counter. */
    *(volatile uint32_t *)SL6806_COREDEBUG_BASE |= COREDEBUG_DEMCR_TRCENA;
    sl6806_dsb();
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA;
    sl6806_dsb();
    if (counter_moves(&DWT->CYCCNT))
        tick_mask = 0xFFFFFFFFu;

    if (!tick_mask) {
        /* Fall back to SysTick as a free-running 24-bit counter, interrupt
         * disabled so we never touch the ROM's vector table. CLKSOURCE picks
         * the processor clock, which is always implemented; the external
         * reference is optional. */
        SysTick->CTRL = 0;
        SysTick->LOAD = 0x00FFFFFFu;
        SysTick->VAL  = 0;
        SysTick->CTRL = SYSTICK_CTRL_ENABLE | SYSTICK_CTRL_CLKSOURCE;
        sl6806_dsb();
        if (counter_moves(&SysTick->VAL))
            tick_mask = 0x00FFFFFFu;
        else
            SysTick->CTRL = 0;      /* leave it as we found it */
    }

    last_raw  = read_raw();
    cycle_acc = 0;

    /* Say so at startup rather than waiting for the first delay(). Having no
     * time source at all changes what every reported millis() means, so it
     * belongs next to the banner, not buried later in the output. */
    if (!tick_mask)
        sl6806_no_clock();
}

uint32_t sl6806_tick_mask(void)
{
    if (!time_ready)
        sl6806_time_init();
    return tick_mask;
}

uint32_t sl6806_tick_raw(void)
{
    if (!time_ready)
        sl6806_time_init();
    /* With no working source read_raw() is defined to return 0, which would
     * hide the very thing worth seeing. Report what the register actually
     * read during the probe instead: a constant nonzero says CYCCNT is not
     * implemented and reads junk, a constant zero says it is there but never
     * clocked. Those are different faults. */
    return tick_mask ? read_raw() : probe_raw;
}

uint64_t sl6806_cycles(void)
{
    uint64_t r;
    uint32_t pm, now;

    if (!time_ready)
        sl6806_time_init();

    pm = sl6806_irq_save();
    now = read_raw();
    cycle_acc += (uint64_t)((now - last_raw) & tick_mask);
    last_raw = now;
    r = cycle_acc;
    sl6806_irq_restore(pm);
    return r;
}

uint32_t millis(void)
{
    return (uint32_t)(sl6806_cycles() / (F_CPU / 1000UL));
}

uint32_t micros(void)
{
    return (uint32_t)(sl6806_cycles() / (F_CPU / 1000000UL));
}

void delayMicroseconds(uint32_t us)
{
    /* Short waits: spin directly on the raw counter. No accumulator update
     * needed because the wait is far shorter than a wrap. */
    uint32_t start, target;

    if (!time_ready)
        sl6806_time_init();

    /* No counter: waiting would mean spinning on a value that never changes.
     * Return instead - see sl6806_no_clock(). */
    if (!tick_mask) {
        sl6806_no_clock();
        return;
    }

    if (block_limit_ms && us / 1000UL > block_limit_ms) {
        sl6806_block_clamped(us / 1000UL, block_limit_ms);
        us = block_limit_ms * 1000UL;
    }

    start  = read_raw();
    target = (uint32_t)(us * (F_CPU / 1000000UL));

    while (((read_raw() - start) & tick_mask) < target) {
        /* On the 24-bit fallback a request longer than a wrap can never be
         * satisfied; break it into chunks instead of spinning forever. */
        if (tick_mask != 0xFFFFFFFFu && target > (tick_mask >> 1)) {
            uint32_t half = us / 2;
            delayMicroseconds(half);
            delayMicroseconds(us - half);
            return;
        }
    }
}

void delay(uint32_t ms)
{
    uint64_t target;

    if (!time_ready)
        sl6806_time_init();

    /* The one that mattered: with a frozen counter this loop can never reach
     * its target, and in poll mode it spins forever inside the ROM's USB
     * handler. Never wait on a clock that is not running. */
    if (!tick_mask) {
        sl6806_no_clock();
        return;
    }

    if (block_limit_ms && ms > block_limit_ms) {
        sl6806_block_clamped(ms, block_limit_ms);
        ms = block_limit_ms;
    }

    target = sl6806_cycles() + (uint64_t)ms * (F_CPU / 1000UL);

    while (sl6806_cycles() < target)
        yield();
}

/* Weak so a sketch can override it - same contract as Arduino's yield(). */
__attribute__((weak)) void yield(void)
{
    /* Reading the counter here is what keeps the wrap accumulator honest
     * during long waits. */
    (void)sl6806_cycles();
}
