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
 *   TAKEOVER        - setup() runs, then loop() spins forever and _start
 *                     never returns. USB dies with it: no Serial, no monitor,
 *                     and the only way back is to re-enter bootloader mode.
 *                     Use it when the ROM's callback turns out not to fire,
 *                     or when you need every cycle.
 */

#include "sl6806.h"
#include "sl6806_rom.h"
#include "sl6806_console.h"
#include "wiring_time.h"

#define SL6806_RUN_HOOK     0
#define SL6806_RUN_TAKEOVER 1

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

    /* The sentinel address means "console poll", not "read this memory". It
     * is far outside any valid SL6806 address, so a genuine read can never
     * land here by accident. One round trip fetches the data and consumes
     * it, which a plain memory read cannot do. */
    if (addr == SL6806_CONSOLE_POLL_ADDR) {
        sl6806_console_pkt_t pkt;

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
    sl6806_run_loop();
    return 0;
}
#endif

/* Must live in .bss/.data of the image so it stays valid after _start
 * returns - the ROM keeps the pointer. */
static sl6806_usb_userfn_t userfn;

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
    sl6806_run_setup();

#if SL6806_RUN_MODE == SL6806_RUN_TAKEOVER
    for (;;)
        sl6806_run_loop();
#else
    userfn.cb2     = payload_idle_cb;
    userfn.scsi_cb = payload_scsi_cb;
    rom_usb_set_userfn(&userfn);

    /* Tell the ROM this command produced no data of its own. */
    SL6806_USB_RET_LEN = 0;
#endif
}
