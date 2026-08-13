/*
 * sl6806_module.c - see sl6806_module.h. The order of the two writes and the
 * wait afterwards are the whole point; do not "simplify" them.
 */

#include <stdint.h>

#include "sl6806_module.h"
#include "sl6806_mmio.h"

#define CRU_BASE        0x40080000u
#define MOD_BASE_HI     0x400F1000u

int sl6806_module_enable(unsigned id)
{
    uint32_t base, gate, shadow, bit;
    unsigned i;

    if (id >= 128)
        return 0;

    if (id < 32)        { base = CRU_BASE;    gate = 0x60; shadow = 0x70; }
    else if (id < 64)   { base = CRU_BASE;    gate = 0x64; shadow = 0x74; id -= 32; }
    else if (id < 96)   { base = CRU_BASE;    gate = 0x68; shadow = 0x78; id -= 64; }
    else                { base = MOD_BASE_HI; gate = 0x20; shadow = 0x30; id -= 96; }

    bit = 1u << id;

    sl6806_mmio_write(base + shadow, sl6806_mmio_read(base + shadow) | bit);
    sl6806_mmio_write(base + gate,   sl6806_mmio_read(base + gate)   | bit);

    for (i = 0; i < 200u; i++)
        if (sl6806_mmio_read(base + gate) & bit)
            return 1;
    return 0;
}
