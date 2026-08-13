/*
 * sl6806_bt.c - the gate in front of 0x400E2000, performed.
 *
 * This is a transcription of 0x00D9A7FC and of the first four instructions of
 * the module registration at 0x00D98C9C. It has never run. Read the warning
 * on sl6806_bt_begin() in the header before calling it: this is the one
 * function in the framework that starts a PLL.
 */

#include "sl6806_bt.h"
#include "sl6806_module.h"
#include "wiring_time.h"

int sl6806_bt_begin(void)
{
    /* The group gate - module 46, the PLL, 0x4008011C and 0x400E0000 bit 5.
     * It lives in sl6806_module.c because it is the whole window's, not
     * Bluetooth's; this block just needs it first. */
    if (!sl6806_periph_group_begin())
        return 0;

    if (!sl6806_periph_enable(SL6806_BT_PERIPH_ID))
        return 0;

    sl6806_periph_reset(SL6806_BT_RESET_ID);
    return 1;
}

int sl6806_bt_clocks(uint32_t out[4])
{
    static uint32_t last[4];
    static int seeded;
    uint32_t now[4];
    int moved = 0;
    unsigned i;

    now[0] = SL6806_BT_CLK0 & SL6806_BT_CLK0_MASK;
    now[1] = SL6806_BT_CLK1 & SL6806_BT_CLK_MASK;
    now[2] = SL6806_BT_CLK2 & SL6806_BT_CLK_MASK;
    now[3] = SL6806_BT_CLK3 & SL6806_BT_CLK_MASK;

    for (i = 0; i < 4; i++) {
        if (seeded && now[i] != last[i])
            moved = 1;
        last[i] = now[i];
        out[i] = now[i];
    }
    seeded = 1;
    return moved;
}
