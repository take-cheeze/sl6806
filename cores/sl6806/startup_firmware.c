/*
 * startup_firmware.c - entry point and vector table for FIRMWARE builds.
 *
 * A firmware build replaces the stock application in the FIRM partition. The
 * HLKJ first-stage loader copies the image to RAM and jumps to it, so unlike
 * the stock application (which runs mostly XIP from 0x00C10000) this image is
 * linked to run entirely from SRAM at 0x00804C00. That is what the stock app
 * header already asks for, just with a bigger length, and it avoids having to
 * work out the XIP/RAM split before anything can run.
 *
 * READ docs/FLASHING.md BEFORE USING THIS. Flashing is the only operation
 * here that can leave you with a non-booting device. It is recoverable - USB
 * download mode lives in mask ROM, not in flash - but only if you have a
 * known-good dump to write back and have tested that you can re-enter
 * bootloader mode.
 */

#include "sl6806.h"

extern uint32_t __data_start__, __data_end__, __data_load__;
extern uint32_t __bss_start__, __bss_end__;
extern uint32_t __stack_top__;

extern void (*__init_array_start[])(void);
extern void (*__init_array_end[])(void);

void sl6806_run_setup(void);
void sl6806_run_loop(void);
void sl6806_panic(const char *msg);

void Reset_Handler(void);
static void Default_Handler(void);

/* Fault handlers report through the console ring, which the monitor can read
 * afterwards - the alternative is a silent hang with no evidence. */
static void HardFault_Handler(void)  { sl6806_panic("HardFault"); }
static void MemManage_Handler(void)  { sl6806_panic("MemManage fault"); }
static void BusFault_Handler(void)   { sl6806_panic("BusFault"); }
static void UsageFault_Handler(void) { sl6806_panic("UsageFault"); }

#define WEAK_ALIAS(name) \
    void name(void) __attribute__((weak, alias("Default_Handler")))

WEAK_ALIAS(NMI_Handler);
WEAK_ALIAS(SVC_Handler);
WEAK_ALIAS(DebugMon_Handler);
WEAK_ALIAS(PendSV_Handler);
WEAK_ALIAS(SysTick_Handler);

/*
 * The number of external interrupts this SoC implements is not known, so the
 * table carries 64 slots. Spare entries are harmless; a table that is too
 * short is not. Override any of them by defining IRQ<n>_Handler.
 */
#define SL6806_NUM_IRQ 64

void IRQ_Handler_Default(void) __attribute__((weak, alias("Default_Handler")));

__attribute__((section(".isr_vector"), used))
void (* const g_vectors[16 + SL6806_NUM_IRQ])(void) = {
    (void (*)(void))&__stack_top__,
    Reset_Handler,
    NMI_Handler,
    HardFault_Handler,
    MemManage_Handler,
    BusFault_Handler,
    UsageFault_Handler,
    0, 0, 0, 0,
    SVC_Handler,
    DebugMon_Handler,
    0,
    PendSV_Handler,
    SysTick_Handler,
    [16 ... 16 + SL6806_NUM_IRQ - 1] = IRQ_Handler_Default,
};

static void Default_Handler(void)
{
    sl6806_panic("unhandled interrupt");
}

void Reset_Handler(void)
{
    uint32_t *src, *dst;
    int i, n;

    /* Point the core at our table; the loader jumped here with VTOR still
     * set to whatever the bootloader used. */
    SCB->VTOR = (uint32_t)g_vectors;
    sl6806_dsb();

    /* Cortex-M4F: the FPU is off out of reset. Enable full access to CP10/CP11
     * before any float code runs, or the first VFP instruction faults. */
    *(volatile uint32_t *)(SL6806_SCB_BASE + 0x88) |= (0xFu << 20);
    sl6806_dsb();
    sl6806_isb();

    /* Initialised data. When the image is loaded whole into RAM these are the
     * same address and the loop does nothing. */
    src = &__data_load__;
    dst = &__data_start__;
    if (src != dst)
        while (dst < &__data_end__)
            *dst++ = *src++;

    for (dst = &__bss_start__; dst < &__bss_end__; dst++)
        *dst = 0;

    n = __init_array_end - __init_array_start;
    for (i = 0; i < n; i++)
        __init_array_start[i]();

    sl6806_run_setup();
    for (;;)
        sl6806_run_loop();
}
