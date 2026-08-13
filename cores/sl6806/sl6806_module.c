/*
 * sl6806_module.c - see sl6806_module.h. The order of the two writes and the
 * wait afterwards are the whole point; do not "simplify" them.
 */

#include <stdint.h>

#include "sl6806_module.h"
#include "sl6806_pwm.h"   /* SL6806_PLL_* and SL6806_CRU_CLK_*, documented there */
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
 * ROM 0x1E54, transcribed: the shadow AND the gate, in that order, across the
 * same four banks. The ROM tests both because setting only one leaves a
 * module that reads enabled and is not - which is the failure sl6806_module.h
 * spends a paragraph on.
 *
 * Worth having as a call rather than a comment because it is the vendor's own
 * guard: 0x00D9A7FC skips a PLL start and a group-wide clock enable when this
 * returns true, and sl6806_bt_begin() needs the same guard for the same
 * reason.
 */
int sl6806_module_enabled(unsigned id)
{
    uint32_t base, gate, shadow, bit;

    if (id >= 128)
        return 0;

    if (id < 32)        { base = CRU_BASE;    gate = 0x60; shadow = 0x70; }
    else if (id < 64)   { base = CRU_BASE;    gate = 0x64; shadow = 0x74; id -= 32; }
    else if (id < 96)   { base = CRU_BASE;    gate = 0x68; shadow = 0x78; id -= 64; }
    else                { base = MOD_BASE_HI; gate = 0x20; shadow = 0x30; id -= 96; }

    bit = 1u << id;

    return (sl6806_mmio_read(base + shadow) & bit)
        && (sl6806_mmio_read(base + gate) & bit);
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

/*
 * [V] 0x00D9A7AC. Ten request bits in [9:0], ten acknowledgements in
 * [25:16], one at a time, skipping any that is already acknowledged.
 *
 * The vendor's inner wait is unbounded. This one is not, for the usual
 * reason - a payload runs this inside the boot ROM's USB handler - and it
 * carries on to the next domain rather than giving up, because a domain that
 * does not answer is information and stopping would throw the rest away.
 */
static void power_request(void)
{
    unsigned i, spin;
    uint32_t ack;

    for (i = 0; i < SL6806_PWR_DOMAINS; i++) {
        ack = 1u << (SL6806_PWR_ACK_SHIFT + i);

        if (sl6806_mmio_read(SL6806_PWR_REQ) & ack)
            continue;

        sl6806_mmio_set(SL6806_PWR_REQ, 1u << i);

        for (spin = 0; spin < 1000u; spin++)
            if (sl6806_mmio_read(SL6806_PWR_REQ) & ack)
                break;
    }

    sl6806_mmio_write(SL6806_PWR_RELEASE, 0);
}

/*
 * [V] The PLL half of 0x00D9A7FC. Bounded poll again: in bootloader mode the
 * lock bit is known never to set (14a read it stopped, at 0x00000801), so an
 * unbounded spin here is not a hypothetical hang - it is the expected one.
 *
 * Returns 1 if it locked.
 */
static int pll_start(void)
{
    unsigned i;

    sl6806_mmio_write(SL6806_PLL_CTRL, SL6806_PLL_CONFIG);

    for (i = 0; i < 10000u; i++) {
        if (sl6806_mmio_read(SL6806_PLL_CTRL) & SL6806_PLL_LOCK) {
            sl6806_mmio_set(SL6806_PLL_CTRL, SL6806_PLL_OUT_ENABLE);
            return 1;
        }
    }
    return 0;
}

/*
 * The group bring-up, 0x00D9A7FC. See sl6806_module.h for what it measured.
 */
int sl6806_periph_group_begin(void)
{
    /* [V] The vendor's own guard: it does nothing at all if module 46 is
     * already up, so this runs once per boot and a second call does not
     * restart a PLL something else may be using. */
    if (sl6806_module_enabled(SL6806_PERIPH_GROUP_MODULE_ID))
        return 1;

    power_request();
    pll_start();

    if (!sl6806_module_enable(SL6806_PERIPH_GROUP_MODULE_ID))
        return 0;
    delay(10);

    sl6806_mmio_write(SL6806_CRU_CLK_ENABLE, SL6806_CRU_CLK_ON);
    delay(10);

    /* [V] The group's own functional clock, before any peripheral's. */
    sl6806_periph_enable(SL6806_PERIPH_GROUP_PERIPH_ID);
    return 1;
}
