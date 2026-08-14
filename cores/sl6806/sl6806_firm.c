/*
 * sl6806_firm.c - see sl6806_firm.h. Start the stock application.
 */

#include "sl6806.h"      /* SL6806_SRAM_* on the device */
#include "sl6806_firm.h"

#define RD32(p, o) ((uint32_t)((p)[(o)] | ((uint32_t)(p)[(o) + 1] << 8)      \
                             | ((uint32_t)(p)[(o) + 2] << 16)                \
                             | ((uint32_t)(p)[(o) + 3] << 24)))

/* The largest segment this will believe. The shipped image is 0x5862; a
 * header claiming more than the SRAM below the payload could hold is a
 * corrupt header, not a bigger application. */
#define MAX_LENGTH 0x00018000u

int sl6806_firm_parse(const uint8_t *header, const uint8_t *vectors,
                      sl6806_firm_t *out)
{
    uint32_t entry, load, length, stack, reset;

    if (!header || !vectors || !out)
        return SL6806_FIRM_ERR_HDR;

    if (RD32(header, 0x00) != SL6806_FIRM_HDR_LEN)
        return SL6806_FIRM_ERR_HDR;

    entry  = RD32(header, 0x14);
    length = RD32(header, 0x1C);

    /* Thumb, and inside SRAM. A Cortex-M branch to an even address faults
     * immediately, and this is the one place to notice that cheaply. */
    if (!(entry & 1u))
        return SL6806_FIRM_ERR_ENTRY;
    if ((entry & ~1u) < SL6806_SRAM_BASE || (entry & ~1u) >= SL6806_SRAM_END)
        return SL6806_FIRM_ERR_ENTRY;

    if (length == 0u || length > MAX_LENGTH)
        return SL6806_FIRM_ERR_LENGTH;

    /*
     * The load address is the entry less the vector table - see the header
     * for why, and for the two independent findings that agree on it.
     */
    load = (entry & ~1u) - SL6806_FIRM_VECTORS;
    if (load < SL6806_SRAM_BASE)
        return SL6806_FIRM_ERR_ENTRY;

    /*
     * And now check that derivation against the image rather than trusting
     * it. If the segment really does begin with the vector table, then its
     * word 1 is the reset vector and must be the entry the header named, and
     * word 0 is the initial stack pointer and must be somewhere in SRAM.
     *
     * Getting this wrong means copying the image 0x400 bytes off and jumping
     * into the middle of a function, which on a Cortex-M is a hard fault with
     * the USB link already gone - no console, no message, nothing but a
     * device that stopped. The check costs two comparisons.
     */
    stack = RD32(vectors, 0);
    reset = RD32(vectors, 4);

    if (reset != entry)
        return SL6806_FIRM_ERR_VECTORS;
    if (stack <= SL6806_SRAM_BASE || stack > SL6806_SRAM_END)
        return SL6806_FIRM_ERR_VECTORS;

    out->timestamp = RD32(header, 0x04);
    out->entry     = entry;
    out->load      = load;
    out->length    = length;
    out->stack     = stack;
    return SL6806_FIRM_OK;
}

#ifndef SL6806_FIRM_HOSTED

int sl6806_firm_read(sl6806_firm_t *out)
{
    const uint8_t *hdr = (const uint8_t *)SL6806_FIRM_XIP;
    const uint8_t *seg = hdr + SL6806_FIRM_SEG_OFF;

    return sl6806_firm_parse(hdr, seg, out);
}

int sl6806_firm_boot(const sl6806_firm_t *f)
{
    const uint32_t *src;
    uint32_t *dst;
    uint32_t n;

    if (!f || f->length == 0u || (f->entry & 1u) == 0u)
        return SL6806_FIRM_ERR_HDR;

    /*
     * Refuse if the destination would overlap this code. A payload lives at
     * 0x00820000 and the application loads at 0x00804800 for 0x5862 bytes, so
     * they do not collide - but a header claiming otherwise would have us
     * overwrite the loop we are running in, mid-copy.
     */
    if (f->load + f->length > 0x0081F000u)
        return SL6806_FIRM_ERR_LENGTH;

    src = (const uint32_t *)(SL6806_FIRM_XIP + SL6806_FIRM_SEG_OFF);
    dst = (uint32_t *)f->load;
    for (n = 0; n < (f->length + 3u) / 4u; n++)
        dst[n] = src[n];

    /*
     * Hand over a quiet machine.
     *
     * The application expects to start the way it does after a reset, and it
     * is not starting after a reset: the boot ROM has been running for
     * however long, with USB enumerated, its own interrupts enabled, and
     * SysTick ticking. Anything still enabled here fires into the
     * application's vector table between the jump and its own NVIC setup -
     * with the ROM's handler expectations and the application's stack.
     *
     * A bootloader is expected to do this and this one was not. It is the
     * second candidate for why the application's USB came up unstable, after
     * the clock tree (which examples/FirmBoot now sets and which did not fix
     * it): a USB interrupt left pending by the ROM would be delivered to the
     * application's USB handler before the application had configured its own
     * controller.
     */
    for (n = 0; n < 4u; n++) {
        ((volatile uint32_t *)0xE000E180u)[n] = 0xFFFFFFFFu;   /* NVIC_ICER */
        ((volatile uint32_t *)0xE000E280u)[n] = 0xFFFFFFFFu;   /* NVIC_ICPR */
    }
    *(volatile uint32_t *)0xE000E010u = 0;                     /* SysTick off */
    *(volatile uint32_t *)0xE000E018u = 0;                     /* and clear   */

    /*
     * Point the vector table at the copy before entering it. The application
     * assumes the bootloader did this; an interrupt arriving between here and
     * its own setup would otherwise vector through whatever the ROM left.
     */
    *(volatile uint32_t *)0xE000ED08u = f->load;

    __asm__ volatile (
        "cpsid i               \n"   /* no interrupts across the handover  */
        "dsb                   \n"
        "isb                   \n"
        "msr msp, %0           \n"   /* the application's own stack        */
        "bx  %1                \n"   /* and away                           */
        :
        : "r" (f->stack), "r" (f->entry)
        : "memory");

    return SL6806_FIRM_OK;              /* not reached */
}

#endif /* SL6806_FIRM_HOSTED */
