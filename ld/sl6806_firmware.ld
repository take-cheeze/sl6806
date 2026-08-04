/*
 * sl6806_firmware.ld - image for the FIRM partition.
 *
 * The HLKJ first-stage loader copies `length` bytes from the FIRM payload to
 * `loadToRam` and jumps to `runFrom`. The stock application asks for only
 * ~0x5862 bytes and runs the rest XIP from 0x00C10000; this framework instead
 * links everything to SRAM and asks the loader to copy all of it. That trades
 * some RAM for not having to know where the XIP/RAM boundary is, and keeps
 * one memory model for the whole image.
 *
 * Layout, matching the addresses the stock app header already uses:
 *   0x00804C00  vector table, then code/rodata/data
 *   ...         bss, then heap
 *   0x0083C000  stack top (grows down toward the heap)
 *
 * The 0x0083C000 stack top is a choice, not a measured value. The stock app
 * used 0x0082D63C; we sit above that because we are not sharing SRAM with the
 * vendor application. If your firmware misbehaves in ways that look like
 * memory corruption, suspect that high SRAM is used by something we have not
 * identified and move this down.
 */

OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(Reset_Handler)

MEMORY
{
    /* 0x00804C00 .. 0x0083C000 */
    RAM (rwx) : ORIGIN = 0x00804C00, LENGTH = 0x37400
}

__stack_size__ = DEFINED(__stack_size__) ? __stack_size__ : 8K;

SECTIONS
{
    .isr_vector : ALIGN(128)
    {
        KEEP(*(.isr_vector))        /* must be first: VTOR points at it */
    } > RAM

    .text : ALIGN(4)
    {
        *(.text .text.*)
        *(.rodata .rodata.*)
        *(.gnu.linkonce.t.*)
        *(.gnu.linkonce.r.*)

        *(.ARM.extab* .gnu.linkonce.armextab.*)
        . = ALIGN(4);
        __exidx_start = .;
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)
        __exidx_end = .;

        . = ALIGN(4);
        __preinit_array_start = .;
        KEEP(*(.preinit_array))
        __preinit_array_end = .;

        . = ALIGN(4);
        __init_array_start = .;
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        __init_array_end = .;

        . = ALIGN(4);
        __fini_array_start = .;
        KEEP(*(SORT(.fini_array.*)))
        KEEP(*(.fini_array))
        __fini_array_end = .;
    } > RAM

    .data : ALIGN(4)
    {
        __data_start__ = .;
        *(.data .data.*)
        *(.gnu.linkonce.d.*)
        . = ALIGN(4);
        __data_end__ = .;
    } > RAM
    __data_load__ = ADDR(.data);    /* image is copied whole - VMA == LMA */

    __image_load_end = .;

    .bss (NOLOAD) : ALIGN(4)
    {
        __bss_start__ = .;
        *(.bss .bss.*)
        *(.gnu.linkonce.b.*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end__ = .;
    } > RAM

    /* Heap fills what is left, stopping short of the stack. The bounds are
     * written as ORIGIN/LENGTH expressions inside SECTIONS rather than as
     * symbols defined above it: ld resolves those consistently here. */
    .heap (NOLOAD) : ALIGN(8)
    {
        __heap_start__ = .;
        . = ORIGIN(RAM) + LENGTH(RAM) - __stack_size__;
        __heap_end__ = .;
    } > RAM

    __stack_top__ = ORIGIN(RAM) + LENGTH(RAM);

    ASSERT(__heap_end__ > __heap_start__,
           "image + stack leave no room for a heap")

    /DISCARD/ : { *(.comment) *(.note.*) }
}
