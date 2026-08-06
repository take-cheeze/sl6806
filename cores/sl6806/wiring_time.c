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
 * CONSEQUENCE - THE ONE RULE:
 * The counter is 32-bit (DWT) or 24-bit (SysTick fallback) and wraps. Wraps
 * are accumulated in software, but only when the counter is read. You must
 * call millis(), micros(), delay() or yield() at least once per wrap period
 * or time is silently lost. At the placeholder 120 MHz that period is ~35 s
 * (DWT) or ~140 ms (SysTick fallback). A normal loop() satisfies this easily;
 * a long blocking computation does not - call yield() inside it.
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

/* Which counter we ended up with. */
static uint32_t tick_mask;      /* counter modulus - 1 */
static uint32_t last_raw;       /* previous raw up-count */
static uint64_t cycle_acc;      /* accumulated cycles */
static uint8_t  time_ready;

/* Read the hardware counter as an up-counter. */
static inline uint32_t read_raw(void)
{
    if (tick_mask == 0xFFFFFFFFu)
        return DWT->CYCCNT;
    /* SysTick counts down; invert to get an up-count. */
    return (0x00FFFFFFu - (SysTick->VAL & 0x00FFFFFFu)) & 0x00FFFFFFu;
}

void sl6806_time_init(void)
{
    if (time_ready)
        return;

    /* Prefer the DWT cycle counter: 32-bit, 1-cycle resolution, free. It is
     * optional on Cortex-M4, so probe it rather than assume it. */
    *(volatile uint32_t *)SL6806_COREDEBUG_BASE |= COREDEBUG_DEMCR_TRCENA;
    sl6806_dsb();
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA;
    sl6806_dsb();

    if (DWT->CYCCNT != 0) {
        /* It ticked - DWT is present and running. */
        tick_mask = 0xFFFFFFFFu;
    } else {
        /* No DWT. Fall back to SysTick as a free-running 24-bit counter,
         * interrupt disabled so we never touch the ROM's vector table. */
        tick_mask = 0x00FFFFFFu;
        SysTick->CTRL = 0;
        SysTick->LOAD = 0x00FFFFFFu;
        SysTick->VAL  = 0;
        SysTick->CTRL = SYSTICK_CTRL_ENABLE | SYSTICK_CTRL_CLKSOURCE;
    }

    last_raw  = read_raw();
    cycle_acc = 0;
    time_ready = 1;
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
