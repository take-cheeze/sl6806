/*
 * sl6806_hwinit.c - see sl6806_hwinit.h. The bootloader's clock tree,
 * performed in place rather than jumped into.
 */

#include "sl6806_hwinit.h"
#include "sl6806_mmio.h"

uint32_t sl6806_pll_hz(void)
{
    /* [V] ROM 0x1FB8, exactly: the multiplier is bits [15:8] in units of
     * 48 MHz and the divider is bits [3:0]. */
    uint32_t v = sl6806_mmio_read(SL6806_PLL_STATUS);
    uint32_t mul = (v >> 8) & 0xFFu;
    uint32_t div = v & 0xFu;

    if (div == 0u)
        return 0u;
    return mul * 48000000u / div;
}

int sl6806_pll_set_384(void)
{
    /*
     * [V] ROM 0x1F6C, transcribed. Two read-modify-writes and no poll - the
     * vendor's routine does not wait for anything, and inventing a lock bit
     * for it would be inventing a register.
     *
     * The constants are the branch for "any frequency except 24.576 MHz",
     * which is the audio clock's special case (§25). 384 MHz takes this one.
     */
    sl6806_mmio_write(SL6806_PLL_CTRL,
                      (sl6806_mmio_read(SL6806_PLL_CTRL) & SL6806_PLL_CTRL_MASK)
                      | SL6806_PLL_CTRL_BITS);

    sl6806_mmio_write(SL6806_PLL_MUL,
                      (sl6806_mmio_read(SL6806_PLL_MUL) & SL6806_PLL_MUL_MASK)
                      | SL6806_PLL_MUL_BITS
                      | (SL6806_PLL_MUL_VALUE << 14));

    return sl6806_pll_hz() == SL6806_PLL_TARGET_HZ;
}

uint32_t sl6806_pll_set_divider(unsigned d)
{
    uint32_t v;

    if (d == 0u || d > 0xFu)
        return 0u;

    /* Read-modify-write the divider alone. Every other bit of this register
     * is either the multiplier, the lock (bit 28, §17) or something nobody
     * here has decoded, and clearing any of them to set four bits would be
     * the mistake §30 already recorded once. */
    v = sl6806_mmio_read(SL6806_PLL_STATUS);
    sl6806_mmio_write(SL6806_PLL_STATUS, (v & ~0xFu) | (uint32_t)d);

    return sl6806_pll_hz();
}
