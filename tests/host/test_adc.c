/*
 * test_adc.c - native tests for the ADC driver and the key decode.
 *
 * The ADC driver works on hardware, which is rarer in this tree than it
 * should be, so what this file protects is the two things that were wrong for
 * four hardware runs and would be silently wrong again if someone tidied the
 * code:
 *
 *   1. The module-clock enable writes the SHADOW register before the GATE and
 *      then waits for the gate to read the bit back. Any other order leaves
 *      the block gated, and a gated block still reads plausible values - so
 *      the failure is invisible without hardware. A model that only accepts
 *      the right order is the only place this can be checked.
 *
 *   2. Bring-up leaves every channel disabled, because the vendor's init
 *      sequence zeroes the enables. A channel has to be turned on after
 *      init. Getting that backwards produces a block that is configured,
 *      idle, and completely convincing.
 *
 * Plus the key thresholds, against the levels actually measured on the board.
 */

#include <stdio.h>
#include <string.h>

#include "sl6806_adc.h"
#include "variant.h"

static int failures, checks;

#define CHECK(cond, what) do {                                          \
        checks++;                                                       \
        if (!(cond)) {                                                  \
            printf("  FAIL %s (%s:%d)\n", (what), __FILE__, __LINE__);  \
            failures++;                                                 \
        }                                                               \
    } while (0)

/* --- a model of the gate pair and the ADC block --------------------- */

#define CRU     0x40080000u
#define ADC     SL6806_ADC_BASE

static uint32_t cru[0x40];      /* words from CRU+0x60 */
static uint32_t adcreg[0x40];
static int      gated_on;
static int      bad_order;      /* gate set while shadow was clear */

uint32_t sl6806_mmio_read(uint32_t a)
{
    if (a >= CRU + 0x60 && a < CRU + 0x160)
        return cru[(a - CRU - 0x60) / 4];
    if (a >= ADC && a < ADC + 0x100) {
        if (!gated_on)
            return 0x00100000u;         /* plausible, and a lie */
        return adcreg[(a - ADC) / 4];
    }
    return 0;
}

void sl6806_mmio_write(uint32_t a, uint32_t v)
{
    if (a >= CRU + 0x60 && a < CRU + 0x160) {
        unsigned i = (a - CRU - 0x60) / 4;
        uint32_t bit = 1u << 20;

        /* id 84 -> gate +0x68 (index 2), shadow +0x78 (index 6), bit 20. */
        if (i == 2 && (v & bit)) {
            if (!(cru[6] & bit))
                bad_order = 1;          /* gate before shadow */
            else
                gated_on = 1;
        }
        cru[i] = v;
        return;
    }
    if (a >= ADC && a < ADC + 0x100) {
        if (!gated_on)
            return;                     /* gated: writes vanish */
        adcreg[(a - ADC) / 4] = v;
    }
}

static void reset_model(void)
{
    memset(cru, 0, sizeof cru);
    memset(adcreg, 0, sizeof adcreg);
    gated_on = 0;
    bad_order = 0;
}

int main(void)
{
    uint32_t ctrl;
    int i;

    printf("test_adc\n");

    /* 1. Bring-up opens the gate, in the right order, and takes writes. */
    reset_model();
    CHECK(!sl6806_adc_writable(), "gated block must not accept writes");
    CHECK(sl6806_adc_begin() == 1, "begin succeeds once the gate opens");
    CHECK(!bad_order, "shadow is written before the gate");
    CHECK(adcreg[SL6806_ADC_CTRL / 4] == SL6806_ADC_INIT_CTRL, "CTRL init value");
    CHECK(adcreg[SL6806_ADC_CFG / 4] == SL6806_ADC_INIT_CFG, "CFG init value");

    /* 2. Init leaves every channel off - the trap that cost a hardware run. */
    ctrl = adcreg[SL6806_ADC_CTRL / 4];
    for (i = 0; i < SL6806_ADC_CHANNELS; i++)
        CHECK(!(ctrl & (1u << i)), "init leaves the channel disabled");

    /* 3. ...and enabling one puts it back, in both registers. */
    sl6806_adc_channel(SL6806_KEY_ADC_CHANNEL, 1);
    CHECK(adcreg[SL6806_ADC_CTRL / 4] & (1u << SL6806_KEY_ADC_CHANNEL),
          "channel enable sets CTRL");
    CHECK(adcreg[SL6806_ADC_IRQ / 4] & (1u << SL6806_KEY_ADC_CHANNEL),
          "channel enable sets +0x0C");
    CHECK((adcreg[SL6806_ADC_CTRL / 4] & 0xFFFF0000u)
          == (SL6806_ADC_INIT_CTRL & 0xFFFF0000u),
          "channel enable leaves the config bits alone");

    sl6806_adc_channel(SL6806_KEY_ADC_CHANNEL, 0);
    CHECK(!(adcreg[SL6806_ADC_CTRL / 4] & (1u << SL6806_KEY_ADC_CHANNEL)),
          "channel disable clears CTRL");

    /* 4. The result register is +0x04 inside the channel block. */
    reset_model();
    sl6806_adc_begin();
    adcreg[SL6806_ADC_RESULT(0) / 4] = 0xD58;
    CHECK(SL6806_ADC_RESULT(0) == 0x24, "channel 0 result is at +0x24");
    CHECK(sl6806_adc_read(0) == 0xD58, "read returns the result register");
    CHECK(SL6806_ADC_RESULT(1) == 0x34, "channel 1 result is 0x10 further on");
    CHECK(sl6806_adc_read(SL6806_ADC_CHANNELS) == 0, "out-of-range channel");

    /* 5. The key decode, against levels measured on the board. */
    CHECK(sl6806_key_decode(0xFEA) == SL6806_KEY_NONE, "idle plateau");
    CHECK(sl6806_key_decode(0xFF0) == SL6806_KEY_NONE, "idle, high jitter");
    CHECK(sl6806_key_decode(0xD58) == SL6806_KEY_VOL_UP, "volume up plateau");
    CHECK(sl6806_key_decode(0xD5B) == SL6806_KEY_VOL_UP, "volume up, jitter");
    CHECK(sl6806_key_decode(0x23) == SL6806_KEY_VOL_DOWN, "volume down plateau");
    CHECK(sl6806_key_decode(0x1B) == SL6806_KEY_VOL_DOWN, "volume down, jitter");
    /* Boundaries, since these are thresholds and not equalities. */
    CHECK(sl6806_key_decode(SL6806_KEY_LEVEL_DOWN - 1) == SL6806_KEY_VOL_DOWN,
          "just below the down threshold");
    CHECK(sl6806_key_decode(SL6806_KEY_LEVEL_DOWN) == SL6806_KEY_VOL_UP,
          "at the down threshold it is already up");
    CHECK(sl6806_key_decode(SL6806_KEY_LEVEL_UP - 1) == SL6806_KEY_VOL_UP,
          "just below the up threshold");
    CHECK(sl6806_key_decode(SL6806_KEY_LEVEL_UP) == SL6806_KEY_NONE,
          "at the up threshold nothing is pressed");

    /* 6. An id in the fourth block must not be written to the CRU. */
    reset_model();
    sl6806_module_enable(100);
    for (i = 0; i < 0x40; i++)
        CHECK(cru[i] == 0, "ids 96..127 do not touch the CRU");

    printf("  %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
