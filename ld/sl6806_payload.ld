/*
 * sl6806_payload.ld - RAM payload image, loaded by the boot ROM.
 *
 * The image is a flat blob written to IMAGE_START and entered at
 * IMAGE_START|1 (Thumb). _start MUST therefore be the very first byte of the
 * image - hence the KEEP on .text._start ahead of everything else.
 *
 * 0x00820000 / 64 KiB is the window the stock smartlink_flash payload uses,
 * so it is known to be free while the ROM is servicing USB. Everything the
 * sketch needs - code, data, bss and heap - lives inside it. The stack is NOT
 * ours: a payload runs on the ROM's stack, on the ROM's call.
 *
 * .data shares its load and run address because the ROM copies the whole blob
 * to RAM in one shot. .bss is stripped from the file and zeroed by _start.
 */

OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(_start)

IMAGE_START = DEFINED(IMAGE_START) ? IMAGE_START : 0x00820000;
IMAGE_SIZE  = DEFINED(IMAGE_SIZE)  ? IMAGE_SIZE  : 0x00010000;

MEMORY
{
    PAYLOAD (rwx) : ORIGIN = 0x00820000, LENGTH = 64K
}

SECTIONS
{
    . = IMAGE_START;
    __image_start = .;

    .text : ALIGN(4)
    {
        KEEP(*(.text._start))       /* must be first - entry is IMAGE_START|1 */
        *(.text .text.*)
        *(.rodata .rodata.*)
        *(.gnu.linkonce.t.*)
        *(.gnu.linkonce.r.*)

        /* ARM exception tables. Kept rather than discarded so that a build
         * which does pull in unwinding still links. */
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
    } > PAYLOAD

    .data : ALIGN(4)
    {
        __data_start__ = .;
        *(.data .data.*)
        *(.gnu.linkonce.d.*)
        . = ALIGN(4);
        __data_end__ = .;
    } > PAYLOAD
    __data_load__ = ADDR(.data);        /* loaded in place - no copy needed */

    /* Everything above this point is in the .bin file. */
    __image_load_end = .;

    .bss (NOLOAD) : ALIGN(4)
    {
        __bss_start__ = .;
        *(.bss .bss.*)
        *(.gnu.linkonce.b.*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end__ = .;
    } > PAYLOAD

    .heap (NOLOAD) : ALIGN(8)
    {
        __heap_start__ = .;
        . = ORIGIN(PAYLOAD) + LENGTH(PAYLOAD);
        __heap_end__ = .;
    } > PAYLOAD

    __image_end = .;

    ASSERT(__image_load_end - IMAGE_START <= IMAGE_SIZE,
           "payload image overflows its 64 KiB window")
    ASSERT(__heap_end__ > __heap_start__,
           "no room left for a heap - shrink the sketch")

    /DISCARD/ : { *(.comment) *(.note.*) }
}
