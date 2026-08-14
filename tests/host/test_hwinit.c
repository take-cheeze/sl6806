/*
 * test_hwinit.c - the PLL rate decode and the write sequence.
 *
 * Two things, both of which would be silent on hardware if wrong:
 *
 *   1. **The rate decode.** ROM 0x1FB8 computes (mul * 48 / div) MHz from
 *      one register. Reading it as a plain divisor, or forgetting the 48,
 *      gives a plausible-looking number that is wrong by a factor - and the
 *      only check on hardware is whether it happens to read 384000000.
 *
 *   2. **The two writes preserve what the vendor preserves.** ROM 0x1F6C
 *      masks each register before or-ing its bits in, and those masks are
 *      not "clear everything": 0xFC1F0000 and 0x00003800 keep fields this
 *      code knows nothing about. Widening either mask would clear a field
 *      the chip needs and nothing here would notice.
 */

#include <stdio.h>
#include "sl6806_hwinit.h"

static int failures, checks;

#define CHECK(cond, what) do {                                          \
        checks++;                                                       \
        if (!(cond)) {                                                  \
            printf("  FAIL %s (%s:%d)\n", (what), __FILE__, __LINE__);  \
            failures++;                                                 \
        }                                                               \
    } while (0)

static uint32_t status, ctrl, mul;

uint32_t sl6806_mmio_read(uint32_t addr)
{
    if (addr == SL6806_PLL_STATUS) return status;
    if (addr == SL6806_PLL_CTRL)   return ctrl;
    if (addr == SL6806_PLL_MUL)    return mul;
    return 0;
}

void sl6806_mmio_write(uint32_t addr, uint32_t value)
{
    if (addr == SL6806_PLL_CTRL) { ctrl = value; return; }
    if (addr == SL6806_PLL_MUL)  { mul = value; return; }
}

int main(void)
{
    printf("test_hwinit\n");

    printf("the rate decode\n");
    status = (8u << 8) | 1u;
    CHECK(sl6806_pll_hz() == 384000000u, "m=8 d=1 is 384 MHz - the vendor's");
    status = (16u << 8) | 2u;
    CHECK(sl6806_pll_hz() == 384000000u, "m=16 d=2 is the same rate");
    status = (12u << 8) | 1u;
    CHECK(sl6806_pll_hz() == 576000000u, "m=12 d=1 is 576 MHz");
    status = 0x00000800u;
    CHECK(sl6806_pll_hz() == 0u, "a zero divider is not a rate, and does not divide");

    printf("the write sequence\n");
    /* Fill the fields the vendor's masks preserve with a pattern, and require
     * it to survive. */
    ctrl = 0xFC1F0000u | 0x00E0FFFFu;
    mul  = 0x00003800u | 0x7FFFC3FFu;
    status = (8u << 8) | 1u;

    CHECK(sl6806_pll_set_384() == 1, "it reports success when the rate reads back");

    CHECK((ctrl & 0xFC1F0000u) == 0xFC1F0000u,
          "CTRL's preserved field survived");
    CHECK((ctrl & 0x00003040u) == 0x00003040u, "and the vendor's bits went in");
    CHECK((mul & 0x00003800u) == 0x00003800u, "MUL's preserved field survived");
    CHECK((mul & 0x80000400u) == 0x80000400u, "and its fixed bits went in");
    /* The multiplier is 17 bits, so it occupies 14..30 - bit 31 is the fixed
     * 0x80000000 above it, and masking 18 bits would catch that too. */
    CHECK(((mul >> 14) & 0x1FFFFu) == 0x186C2u,
          "with the multiplier at bit 14, below the fixed top bit");

    status = 0;
    CHECK(sl6806_pll_set_384() == 0, "and it reports failure when it does not");

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
