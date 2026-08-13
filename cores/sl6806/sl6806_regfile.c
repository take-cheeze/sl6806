/*
 * sl6806_regfile.c - the indexed register file behind the mailbox at
 * 0x400F7000+0x100, and the LDO channels on it.
 *
 * Every sequence here is a transcription. sl6806_regfile.h says where each
 * one comes from and why the file was thought unfindable for so long.
 */

#include "sl6806_regfile.h"
#include "wiring_time.h"

/*
 * The vendor takes a mutex around each transfer when the scheduler is up
 * (0x008066AC / 0x008066D8, guarded by a byte at 0x0082BDB8). A payload is
 * single-threaded and has no scheduler, so there is nothing to take - but
 * anything that ever calls these from an interrupt will need to think about
 * it, because the mailbox is one set of registers and a nested transfer would
 * overwrite a pending one's command word.
 */

#define RF(off) (SL6806_RF_BASE + (off))

static int rf_wait(void)
{
    unsigned n = SL6806_RF_TIMEOUT;

    while (n--) {
        if (!(sl6806_mmio_read(RF(SL6806_RF_CMD)) & SL6806_RF_START))
            return 0;
    }
    return -1;
}

int sl6806_reg_read(unsigned idx)
{
    /* 0x00804EAC. */
    sl6806_mmio_write(RF(SL6806_RF_CMD),
                      SL6806_RF_CMD_FIXED | SL6806_RF_ADDR_READ
                      | ((idx & 0xFFu) << 8));
    sl6806_mmio_set(RF(SL6806_RF_CMD), SL6806_RF_START);

    if (rf_wait() < 0)
        return -1;

    return (int)(sl6806_mmio_read(RF(SL6806_RF_RDATA)) & 0xFFu);
}

int sl6806_reg_write(unsigned idx, uint8_t val)
{
    /* 0x00804E44. Data first, then the command, then the start bit - the
     * order the vendor uses, and the only one that can be right for a block
     * that latches its operands when START goes in. The start bit is a
     * separate read-modify-write rather than being folded into the command
     * word, which is also the vendor's: it writes the command, reads it
     * back, ors in bit 31 and stores. Whether the block needs the two
     * accesses is unknown, so it gets them. */
    sl6806_mmio_write(RF(SL6806_RF_WDATA), val);
    sl6806_mmio_write(RF(SL6806_RF_CMD),
                      SL6806_RF_CMD_FIXED | SL6806_RF_ADDR_WRITE
                      | ((idx & 0xFFu) << 8));
    sl6806_mmio_set(RF(SL6806_RF_CMD), SL6806_RF_START);

    if (rf_wait() < 0)
        return -1;

    return (sl6806_mmio_read(RF(SL6806_RF_STAT)) & SL6806_RF_STAT_ERR) ? 1 : 0;
}

int sl6806_reg_update(unsigned idx, uint8_t keep, uint8_t set)
{
    int old = sl6806_reg_read(idx);

    if (old < 0)
        return -1;

    return sl6806_reg_write(idx, (uint8_t)(((unsigned)old & keep) | set));
}

/* ------------------------------------------------------------------ */
/* The rails                                                           */
/* ------------------------------------------------------------------ */

int sl6806_ldo_enable(unsigned ch)
{
    int rc;

    if (ch < 2 || ch > 6)
        return -1;

    /* 0x00CC734C: channel 4 only, and it is undone again below. */
    if (ch == 4) {
        rc = sl6806_reg_update(SL6806_RF_LDO4_GUARD, 0xFF, 0x80);
        if (rc)
            return rc;
        /* Its voltage menu is re-asserted on enable. With no argument the
         * vendor programs 0x02, which is the 2700 mV entry. */
        rc = sl6806_reg_update(SL6806_RF_LDO4_MV, 0x5D, 0x02);
        if (rc)
            return rc;
    }

    /* 0x00CC73A4, and for four of the five channels this is the whole
     * operation. */
    rc = sl6806_reg_update(SL6806_RF_LDO_ENABLE, 0xFF, (uint8_t)(1u << (ch - 2)));
    if (rc)
        return rc;

    if (ch == 4) {
        delay(2);
        rc = sl6806_reg_update(SL6806_RF_LDO4_GUARD, 0x7F, 0x00);
        if (rc)
            return rc;
    }
    return 0;
}

int sl6806_ldo_disable(unsigned ch)
{
    int rc;

    if (ch < 2 || ch > 6)
        return -1;

    /* 0x00CC7410. `orn r1, r0, #0x5D` sets bits 1, 5 and 7 - every bit of
     * channel 4's voltage menu at once, which is not any of the six values
     * the menu encodes. Transcribed rather than explained. */
    if (ch == 4) {
        rc = sl6806_reg_update(SL6806_RF_LDO4_MV, 0xFF, 0xA2);
        if (rc)
            return rc;
    }

    /* 0x00CC7422. */
    return sl6806_reg_update(SL6806_RF_LDO_ENABLE,
                             (uint8_t)~(1u << (ch - 2)), 0x00);
}

/* [V] The split scale channels 5 and 6 share, 0x00CC74F2 and 0x00CC7536. */
static uint8_t ldo_split_code(unsigned mv)
{
    if (mv <= 2450)
        return (uint8_t)(((mv - 1700) / 50) & 0x1F);
    if (mv > 2649)
        return (uint8_t)((((mv - 2650) / 50) + 16) & 0x1F);
    return 0;
}

int sl6806_ldo_set_mv(unsigned ch, unsigned mv)
{
    switch (ch) {
    case 2:     /* 0x00CC744A */
        return sl6806_reg_update(SL6806_RF_LDO2_MV, 0xE0,
                                 (uint8_t)(((mv - 1500) / 100) & 0x1F));
    case 3:     /* 0x00CC746A */
        return sl6806_reg_update(SL6806_RF_LDO3_MV, 0xF0,
                                 (uint8_t)(((mv - 2650) / 50) & 0x0F));
    case 4:     /* 0x00CC748A - a menu, not a scale. */
        switch (mv) {
        case 2700: return sl6806_reg_update(SL6806_RF_LDO4_MV, 0x5D, 0x02);
        case 2800: return sl6806_reg_update(SL6806_RF_LDO4_MV, 0x5D, 0x20);
        case 2900: return sl6806_reg_update(SL6806_RF_LDO4_MV, 0x5D, 0x22);
        case 3000: return sl6806_reg_update(SL6806_RF_LDO4_MV, 0x5D, 0x80);
        case 3200: return sl6806_reg_update(SL6806_RF_LDO4_MV, 0x5D, 0x82);
        case 3300: return sl6806_reg_update(SL6806_RF_LDO4_MV, 0x5D, 0xA0);
        default:   return -1;   /* the vendor writes the field unchanged */
        }
    case 5:     /* 0x00CC74F2 */
        return sl6806_reg_update(SL6806_RF_LDO5_MV, 0xE0, ldo_split_code(mv));
    case 6:     /* 0x00CC7536 */
        return sl6806_reg_update(SL6806_RF_LDO6_MV, 0xE0, ldo_split_code(mv));
    default:
        return -1;
    }
}

/* [V] The inverse, 0x00CC75BC, shared by channels 5 and 6. */
static int ldo_split_mv(int code)
{
    code &= 0x1F;
    if (code > 15)
        return 2650 + (code - 16) * 50;
    return 1700 + code * 50;
}

int sl6806_ldo_get_mv(unsigned ch)
{
    int v;

    switch (ch) {
    case 2:     /* 0x00CC758A */
        v = sl6806_reg_read(SL6806_RF_LDO2_MV);
        return v < 0 ? -1 : 1500 + (v & 0x1F) * 100;
    case 3:     /* 0x00CC75A2 */
        v = sl6806_reg_read(SL6806_RF_LDO3_MV);
        return v < 0 ? -1 : 2650 + (v & 0x0F) * 50;
    case 5:
        v = sl6806_reg_read(SL6806_RF_LDO5_MV);
        return v < 0 ? -1 : ldo_split_mv(v);
    case 6:
        v = sl6806_reg_read(SL6806_RF_LDO6_MV);
        return v < 0 ? -1 : ldo_split_mv(v);
    default:
        /* Channel 4 included: the vendor reads its answer out of a RAM
         * variable, not the register, so there is nothing to invert. */
        return -1;
    }
}

/* ------------------------------------------------------------------ */
/* The camera                                                          */
/* ------------------------------------------------------------------ */

int sl6806_camera_power_on(void)
{
    int rc;

    /* 0x00D44B1C, steps 1-4. The 5 ms is the vendor's, between the disable
     * and the new voltage. */
    rc = sl6806_ldo_disable(SL6806_LDO_CAMERA);
    if (rc)
        return -1;

    delay(5);

    rc = sl6806_ldo_set_mv(SL6806_LDO_CAMERA, SL6806_LDO_CAMERA_MV);
    if (rc)
        return -3;

    rc = sl6806_ldo_enable(SL6806_LDO_CAMERA);
    if (rc)
        return -4;

    /* 0x00D44EA6 and 0x00D44F06, the sensor driver's own two writes. */
    rc = sl6806_reg_update(SL6806_RF_CAM_FIELD,
                           SL6806_RF_CAM_FIELD_KEEP, SL6806_RF_CAM_FIELD_SET);
    if (rc)
        return -5;

    rc = sl6806_reg_update(SL6806_RF_CAM_ENABLE, 0xFF, SL6806_RF_CAM_ENABLE_BIT);
    if (rc)
        return -6;

    return 0;
}

int sl6806_camera_power_off(void)
{
    int rc;

    rc = sl6806_reg_update(SL6806_RF_CAM_ENABLE,
                           (uint8_t)~SL6806_RF_CAM_ENABLE_BIT, 0x00);
    if (rc)
        return -6;

    rc = sl6806_reg_update(SL6806_RF_CAM_FIELD, SL6806_RF_CAM_FIELD_KEEP, 0x00);
    if (rc)
        return -5;

    rc = sl6806_ldo_disable(SL6806_LDO_CAMERA);
    if (rc)
        return -1;

    return 0;
}
