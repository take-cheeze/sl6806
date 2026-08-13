/*
 * sl6806_module.c - see sl6806_module.h. The order of the two writes and the
 * wait afterwards are the whole point; do not "simplify" them.
 */

#include <stdint.h>

#include "sl6806_module.h"
#include "sl6806_mmio.h"
#include "wiring_time.h"

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

/*
 * The functional clock. See the long note in sl6806_module.h for why this is
 * a second, separate enable and why 0x400E0000 was recorded as unreachable
 * for so long: it is gated by a module clock of its own.
 */
int sl6806_periph_enable(unsigned id)
{
    uint32_t bit;

    if (id >= 32)
        return 0;

    bit = 1u << id;
    sl6806_mmio_set(SL6806_PERIPH_ENABLE_REG, bit);
    delay(10);                          /* [V] 0x00D9A734's own wait */
    return (sl6806_mmio_read(SL6806_PERIPH_ENABLE_REG) & bit) != 0;
}

void sl6806_periph_reset(unsigned id)
{
    uint32_t bit;

    if (id >= 32)
        return;

    /* [V] 0x00D9A768: clear, wait, set. A different register from the
     * enable, which is the part that is easy to miss. */
    bit = 1u << id;
    sl6806_mmio_clr(SL6806_PERIPH_RESET_REG, bit);
    delay(10);
    sl6806_mmio_set(SL6806_PERIPH_RESET_REG, bit);
    delay(10);
}
