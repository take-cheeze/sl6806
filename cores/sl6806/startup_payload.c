/*
 * startup_payload.c - entry point for PAYLOAD builds.
 *
 * A payload build is a RAM image that the boot ROM loads and calls while the
 * chip is in USB bootloader mode. Nothing is written to flash, so this mode
 * cannot brick the device - it is the mode to develop in.
 *
 * The ROM calls _start once, as an ordinary AAPCS function, and expects it to
 * return so the ROM can carry on servicing USB. That shapes the two run modes:
 *
 *   HOOK (default)  - setup() runs, then loop() is driven from the ROM's
 *                     periodic callback and _start returns. USB stays alive,
 *                     so Serial and the monitor keep working.
 *   POLL            - setup() runs, then loop() is driven from the vendor
 *                     SCSI handler instead: once per host poll. USB stays
 *                     alive. Use this when cb2 turns out not to be periodic.
 *   TAKEOVER        - setup() runs, then loop() spins forever and _start
 *                     never returns. USB dies with it: no Serial, no monitor,
 *                     and the only way back is to re-enter bootloader mode.
 *                     Use it when you need every cycle and no output.
 *
 * WHICH ONE YOU WANT. cb2 being periodic is an inference, not a measurement,
 * and on at least one real ROM it is NOT called: the sketch prints setup()'s
 * output and then never ticks. If that is what you see, build with
 * RUN_MODE=poll. TAKEOVER is not the answer to that symptom - it gives you a
 * running loop() you cannot observe.
 *
 * WHAT POLL MODE COSTS. loop() only advances while something is polling, so
 * the sketch effectively runs at the monitor's poll rate and stops when you
 * disconnect.
 *
 * AND IT MUST NOT BLOCK. loop() runs inside the ROM's USB command handler. If
 * it does not return before the host's SCSI timeout - about a second - the
 * transaction is abandoned mid-flight, the endpoint desynchronises, and the
 * device stops answering anything at all, inquiry included, until it is
 * unplugged. A plain delay(1000) in loop() is enough to do it, which is not
 * "slow polling" but a hung device.
 *
 * So poll mode caps how long delay() may block, at
 * SL6806_POLL_BLOCK_LIMIT_MS, and reports once when it clamps. That keeps an
 * ordinary blocking sketch alive and honest rather than wedging the link, but
 * its timing will be wrong - pace loop() with millis() if the timing matters.
 *
 * THE VENDOR HANDLER ANSWERS TWO QUERIES OF ITS OWN, at sentinel addresses no
 * real read can land on: the console poll (sl6806_console.h) and a status
 * report (sl6806_stat.h) that exists so the host can measure the real CPU
 * clock. Both are answered here rather than by a sketch, so they work
 * whatever the sketch is doing - including not ticking at all.
 */

#include "sl6806.h"
#include "sl6806_rom.h"
#include "sl6806_console.h"
#include "sl6806_stat.h"
#include "wiring_time.h"

/*
 * How long loop() may block in poll mode. Comfortably under the host's ~1 s
 * SCSI timeout, with room for the USB round trip either side. Override it if
 * you know your host is more patient; setting it to 0 removes the cap, which
 * will wedge the device the first time loop() blocks.
 */
#ifndef SL6806_POLL_BLOCK_LIMIT_MS
#define SL6806_POLL_BLOCK_LIMIT_MS 50
#endif

#ifndef SL6806_RUN_MODE
#define SL6806_RUN_MODE SL6806_RUN_HOOK
#endif

/* Provided by the linker script. */
extern uint32_t __bss_start__, __bss_end__;
extern void (*__init_array_start[])(void);
extern void (*__init_array_end[])(void);

/* Implemented in main.cpp. */
void sl6806_run_setup(void);
void sl6806_run_loop(void);

/* Same vendor opcode the stock payload uses, so the unmodified
 * `smtlink_dump read_mem2 <addr> <size> <file>` reads memory from a sketch.
 * This is what tools/sl6806-monitor polls the console ring with. */
#define CMD_USER_READMEM 64

#if SL6806_RUN_MODE != SL6806_RUN_TAKEOVER

/*
 * The host decodes this with a fixed struct format (see
 * tools/sl6806-calibrate), so padding here would silently desync the two
 * sides. Pin the contract down at compile time, the way the console does.
 */
#include <stddef.h>
_Static_assert(sizeof(sl6806_stat_t) == 40,
               "host unpacks the status reply as '<10I'");
_Static_assert(offsetof(sl6806_stat_t, cycles_lo) == 8,
               "the cycle counter must be the third word");

/* Counters reported by SL6806_STAT_ADDR. Not volatile: everything that
 * touches them runs from the ROM's USB thread, and the host only ever sees a
 * snapshot copied out under the same call. */
static uint32_t stat_polls;
static uint32_t stat_loops;

/* Every path that runs loop() goes through here, so `loops` counts what the
 * device really did rather than what the build mode implies. In hook mode
 * that makes it the direct answer to "does this ROM's cb2 actually fire?" -
 * poll the status a second apart and see whether it moved. */
static void run_loop_counted(void)
{
    stat_loops++;
    sl6806_run_loop();
}

static void fill_stat(sl6806_stat_t *st)
{
    uint64_t c = sl6806_cycles();

    st->magic     = SL6806_STAT_MAGIC;
    st->version   = SL6806_STAT_VERSION;
    st->cycles_lo = (uint32_t)c;
    st->cycles_hi = (uint32_t)(c >> 32);
    st->polls     = stat_polls;
    st->loops     = stat_loops;
    st->f_cpu     = (uint32_t)F_CPU;
    st->run_mode  = SL6806_RUN_MODE;
    st->tick_mask = sl6806_tick_mask();
    st->raw       = sl6806_tick_raw();
}

static int payload_scsi_cb(uint8_t *cdb)
{
    uint32_t len, addr;

    /* Not a vendor command, or not ours: hand it back so the ROM applies its
     * own handling. This is what keeps read_flash/write_mem working while a
     * sketch is resident, and it is what lets the monitor push input with
     * write_mem. */
    if (cdb[0] != SL6806_SCSI_VENDOR_OP)
        return SL6806_USB_ERROR;

    if (cdb[1] != CMD_USER_READMEM)
        return SL6806_USB_ERROR;

    len  = *(uint32_t *)(cdb + 2);
    addr = *(uint32_t *)(cdb + 6);

    if (len > SL6806_USB_RET_BUF_SIZE)
        return SL6806_USB_ERROR;

    stat_polls++;

    /*
     * Status query. Deliberately the cheapest branch here and free of side
     * effects beyond sampling the counter - it does not drive loop() even in
     * poll mode. tools/sl6806-calibrate times these round trips to recover
     * the real CPU clock, and work that varies per poll would land in that
     * measurement as jitter. See sl6806_stat.h.
     */
    if (addr == SL6806_STAT_ADDR) {
        sl6806_stat_t st;

        fill_stat(&st);
        rom_memcpy(SL6806_USB_RET_BUF, &st, sizeof(st));
        SL6806_USB_RET_LEN = sizeof(st);
        return SL6806_USB_DATA;
    }

    /* The sentinel address means "console poll", not "read this memory". It
     * is far outside any valid SL6806 address, so a genuine read can never
     * land here by accident. One round trip fetches the data and consumes
     * it, which a plain memory read cannot do. */
    if (addr == SL6806_CONSOLE_POLL_ADDR) {
        sl6806_console_pkt_t pkt;

#if SL6806_RUN_MODE == SL6806_RUN_POLL
        /* Drive loop() from the one callback that is known to fire. Run it
         * before draining the ring so whatever it prints goes out in this
         * same round trip rather than waiting for the next one. */
        run_loop_counted();
#endif
        sl6806_console_poll(&pkt);
        rom_memcpy(SL6806_USB_RET_BUF, &pkt, sizeof(pkt));
        SL6806_USB_RET_LEN = sizeof(pkt);
        return SL6806_USB_DATA;
    }

    rom_memcpy(SL6806_USB_RET_BUF, (const void *)addr, len);
    SL6806_USB_RET_LEN = (uint16_t)len;
    return SL6806_USB_DATA;
}

#if SL6806_RUN_MODE == SL6806_RUN_HOOK
static int payload_idle_cb(void)
{
    run_loop_counted();
    return 0;
}
#endif

/* Must live in .bss/.data of the image so it stays valid after _start
 * returns - the ROM keeps the pointer. */
static sl6806_usb_userfn_t userfn;

/*
 * Weak: a sketch can override this to install its own callbacks in the table
 * before it is handed to the ROM. Only scsi_cb and cb2 have established
 * meaning, and cb2's is doubtful - see the note at the top - so this exists
 * mainly so examples/CallbackProbe can find out which slots the ROM actually
 * calls on a given unit. Do not overwrite scsi_cb unless you are replacing
 * the console.
 */
__attribute__((weak)) void sl6806_usb_userfn_init(sl6806_usb_userfn_t *fn)
{
    (void)fn;
}
#endif /* != SL6806_RUN_TAKEOVER */

static void runtime_init(void)
{
    uint32_t *p;
    int i, n;

    /* The loaded image contains .text/.rodata/.data only; .bss is ours to
     * clear. Do this before touching any global. */
    for (p = &__bss_start__; p < &__bss_end__; p++)
        *p = 0;

    sl6806_console_init();
    sl6806_time_init();

    /* C++ static constructors. */
    n = __init_array_end - __init_array_start;
    for (i = 0; i < n; i++)
        __init_array_start[i]();
}

__attribute__((section(".text._start"), used))
void _start(void)
{
    runtime_init();

#if SL6806_RUN_MODE == SL6806_RUN_POLL
    /* Before setup(), so a blocking setup() cannot wedge the link either. */
    sl6806_set_block_limit(SL6806_POLL_BLOCK_LIMIT_MS);
#endif

    sl6806_run_setup();

#if SL6806_RUN_MODE == SL6806_RUN_TAKEOVER
    for (;;)
        sl6806_run_loop();
#else
#if SL6806_RUN_MODE == SL6806_RUN_HOOK
    userfn.cb2     = payload_idle_cb;
#endif
    userfn.scsi_cb = payload_scsi_cb;
    sl6806_usb_userfn_init(&userfn);
    rom_usb_set_userfn(&userfn);

    /* Tell the ROM this command produced no data of its own. */
    SL6806_USB_RET_LEN = 0;
#endif
}
