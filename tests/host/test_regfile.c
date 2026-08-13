/*
 * test_regfile.c - native tests for the indexed register file.
 *
 * This driver has never run on hardware, which makes these tests a different
 * kind of thing from the ones next to them: they cannot tell you the decode
 * is right, only that the code performs the sequence the vendor's code
 * performs. That is still worth having, because the sequence is transcribed
 * from a disassembly and every one of the following is easy to "tidy" into
 * something that would fail silently on a device nobody has:
 *
 *   1. The transfer order. Data, then command, then the start bit as its own
 *      read-modify-write. A block that latches its operands on START would
 *      not care; one that latches the index when the command word is written
 *      would break if the start bit were folded in. The vendor uses two
 *      accesses, so this does.
 *
 *   2. The direction byte. 0x60 to write and 0x61 to read - one apart,
 *      because it is an I2C address and its R/W bit. Swapping them reads
 *      where it meant to write, into a chip that carries power rails.
 *
 *   3. The poll is bounded. The vendor spins forever; in payload mode that
 *      happens inside the boot ROM's USB handler and takes the device off
 *      the bus with no log. A timeout must be reported, not waited out.
 *
 *   4. Read-modify-write, never a whole byte. Register 0x03 holds the
 *      camera's field in bits [4:3] AND channel 4's voltage menu in bits 1,
 *      5 and 7. Two drivers share one byte and each preserves the other's
 *      bits; a plain write breaks the one you were not thinking about.
 *
 *   5. The voltage scales, both directions. 2800 mV must encode to 0x13 on
 *      channels 5 and 6, because that is the number the camera's power-up
 *      depends on, and the split scale's 2450..2650 gap is the vendor's own
 *      behaviour rather than an oversight to be smoothed over.
 */

#include <stdio.h>
#include <string.h>

#include "sl6806_regfile.h"

static int failures, checks;

#define CHECK(cond, what) do {                                          \
        checks++;                                                       \
        if (!(cond)) {                                                  \
            printf("  FAIL %s (%s:%d)\n", (what), __FILE__, __LINE__);  \
            failures++;                                                 \
        }                                                               \
    } while (0)

/* ------------------------------------------------------------------ */
/* A model of the mailbox and the chip behind it                       */
/* ------------------------------------------------------------------ */

static uint8_t  chip[256];      /* the register file itself             */
static uint32_t cmd, wdata, rdata, stat;
static int      hang;           /* START never clears - a dead block    */
static int      err;            /* make the next write report failure   */

/* What the model saw, in order, so the sequence itself can be asserted. */
static char     trace[512];
static unsigned n_xfer;

static void note(const char *s)
{
    if (strlen(trace) + strlen(s) + 2 < sizeof(trace)) {
        strcat(trace, s);
        strcat(trace, " ");
    }
}

static void run_transfer(void)
{
    unsigned idx = (cmd >> 8) & 0xFF;
    char buf[64];

    n_xfer++;

    if ((cmd & 0xFF) == SL6806_RF_ADDR_READ) {
        rdata = chip[idx];
        snprintf(buf, sizeof(buf), "R%02X=%02X", idx, chip[idx]);
        stat = 0;
    } else {
        if (!err)
            chip[idx] = (uint8_t)wdata;
        snprintf(buf, sizeof(buf), "W%02X=%02X", idx, (uint8_t)wdata);
        stat = err ? SL6806_RF_STAT_ERR : 0;
    }
    note(buf);

    if (!hang)
        cmd &= ~SL6806_RF_START;
}

uint32_t sl6806_mmio_read(uint32_t a)
{
    switch (a - SL6806_RF_BASE) {
    case SL6806_RF_CMD:   return cmd;
    case SL6806_RF_WDATA: return wdata;
    case SL6806_RF_RDATA: return rdata;
    case SL6806_RF_STAT:  return stat;
    default:              return 0;
    }
}

void sl6806_mmio_write(uint32_t a, uint32_t v)
{
    switch (a - SL6806_RF_BASE) {
    case SL6806_RF_CMD:
        cmd = v;
        if (v & SL6806_RF_START)
            run_transfer();
        break;
    case SL6806_RF_WDATA: wdata = v; break;
    default: break;
    }
}

/* The driver delays around channel 4; nothing to wait for here. */
void delay(uint32_t ms) { (void)ms; }

static void reset_model(void)
{
    memset(chip, 0, sizeof(chip));
    cmd = wdata = rdata = stat = 0;
    hang = err = 0;
    trace[0] = '\0';
    n_xfer = 0;
}

/* ------------------------------------------------------------------ */

static void test_transfers(void)
{
    int v;

    printf("transfers\n");

    reset_model();
    chip[0x7A] = 0x5A;
    v = sl6806_reg_read(0x7A);
    CHECK(v == 0x5A, "a read returns the register");
    CHECK((cmd & 0xFF) == SL6806_RF_ADDR_READ, "a read uses address 0x61");
    CHECK(((cmd >> 8) & 0xFF) == 0x7A, "the index goes in [15:8]");

    reset_model();
    CHECK(sl6806_reg_write(0x16, 0x80) == 0, "a write succeeds");
    CHECK(chip[0x16] == 0x80, "and lands");
    CHECK((cmd & 0xFF) == SL6806_RF_ADDR_WRITE, "a write uses address 0x60");
    CHECK(SL6806_RF_ADDR_READ - SL6806_RF_ADDR_WRITE == 1,
          "the two differ by the R/W bit");

    /*
     * The vendor's command literals are 0x40000060 and 0x40000061, whole
     * words - so bit 30 rides along on every transfer. What it means is
     * unknown, which is exactly why dropping it is easy: it looks like part
     * of a "base address" that could be split off from the "device address"
     * and forgotten. It was, in the first draft of this driver.
     */
    reset_model();
    sl6806_reg_read(0x03);
    CHECK((cmd & SL6806_RF_CMD_FIXED) == SL6806_RF_CMD_FIXED,
          "a read carries the vendor's fixed bit 30");
    reset_model();
    sl6806_reg_write(0x03, 0);
    CHECK((cmd & SL6806_RF_CMD_FIXED) == SL6806_RF_CMD_FIXED,
          "and so does a write");
    CHECK(cmd == (SL6806_RF_CMD_FIXED | SL6806_RF_ADDR_WRITE | (0x03u << 8)),
          "the whole command word is the vendor's, START having cleared");

    /* The data register must be loaded before the transfer runs, not after:
     * a model that latches wdata at START would catch the reverse. */
    reset_model();
    sl6806_reg_write(0x03, 0x42);
    CHECK(chip[0x03] == 0x42, "the data register is written before START");

    reset_model();
    err = 1;
    CHECK(sl6806_reg_write(0x03, 0x11) == 1, "STAT's error bits are reported");

    reset_model();
    hang = 1;
    CHECK(sl6806_reg_read(0x03) == -1, "a stuck START times out on read");
    CHECK(sl6806_reg_write(0x03, 1) == -1, "and on write");
}

static void test_update(void)
{
    printf("read-modify-write\n");

    /* Register 3 is shared: bits [4:3] are the camera's, bits 1/5/7 are
     * channel 4's voltage. Setting one must not disturb the other. */
    reset_model();
    chip[0x03] = 0x22;                  /* channel 4 at 2900 mV */
    CHECK(sl6806_reg_update(0x03, SL6806_RF_CAM_FIELD_KEEP,
                            SL6806_RF_CAM_FIELD_SET) == 0, "camera field set");
    CHECK(chip[0x03] == 0x2A, "the camera's bits went in");
    CHECK((chip[0x03] & 0xA2) == 0x22, "channel 4's voltage survived");

    reset_model();
    chip[0x03] = 0x08;                  /* the camera's field already set */
    CHECK(sl6806_ldo_set_mv(4, 3300) == 0, "channel 4 to 3300 mV");
    CHECK(chip[0x03] == 0xA8, "the menu bits went in");
    CHECK((chip[0x03] & 0x18) == 0x08, "the camera's field survived");

    reset_model();
    hang = 1;
    CHECK(sl6806_reg_update(0x03, 0xFF, 0x01) == -1,
          "an update whose read fails does not write");
    CHECK(n_xfer == 1, "and stops after the read");
}

static void test_rails(void)
{
    printf("rails\n");

    reset_model();
    CHECK(sl6806_ldo_enable(6) == 0, "channel 6 on");
    CHECK(chip[SL6806_RF_LDO_ENABLE] == 0x10, "bit 4 = channel 6");
    CHECK(sl6806_ldo_enable(2) == 0, "channel 2 on");
    CHECK(chip[SL6806_RF_LDO_ENABLE] == 0x11, "without disturbing channel 6");
    CHECK(sl6806_ldo_disable(6) == 0, "channel 6 off");
    CHECK(chip[SL6806_RF_LDO_ENABLE] == 0x01, "and only channel 6");

    CHECK(sl6806_ldo_enable(1) == -1, "channel 1 is refused");
    CHECK(sl6806_ldo_enable(7) == -1, "channel 7 is refused");
    CHECK(sl6806_ldo_set_mv(7, 2800) == -1, "and cannot be set");

    /* Channel 4 brackets its change with bit 7 of 0x1B, and clears it after.
     * The vendor does; a driver that skips it is doing something else. */
    reset_model();
    sl6806_ldo_enable(4);
    CHECK((chip[SL6806_RF_LDO4_GUARD] & 0x80) == 0,
          "channel 4's guard bit is cleared again");
    CHECK(strstr(trace, "W1B=80") != NULL, "after having been set");
}

static void test_voltages(void)
{
    printf("voltage scales\n");

    reset_model();

    /* The number the camera depends on. */
    CHECK(sl6806_ldo_set_mv(6, 2800) == 0, "channel 6 to 2800 mV");
    CHECK(chip[SL6806_RF_LDO6_MV] == 0x13, "encodes to 0x13");
    CHECK(sl6806_ldo_get_mv(6) == 2800, "and decodes back");

    /* Both ends of both halves of the split scale. */
    sl6806_ldo_set_mv(6, 1700);
    CHECK(chip[SL6806_RF_LDO6_MV] == 0x00 && sl6806_ldo_get_mv(6) == 1700,
          "1700 mV is code 0");
    sl6806_ldo_set_mv(6, 2450);
    CHECK(chip[SL6806_RF_LDO6_MV] == 0x0F && sl6806_ldo_get_mv(6) == 2450,
          "2450 mV is code 15");
    sl6806_ldo_set_mv(6, 2650);
    CHECK(chip[SL6806_RF_LDO6_MV] == 0x10 && sl6806_ldo_get_mv(6) == 2650,
          "2650 mV is code 16");
    sl6806_ldo_set_mv(6, 3300);
    CHECK(chip[SL6806_RF_LDO6_MV] == 0x1D && sl6806_ldo_get_mv(6) == 3300,
          "3300 mV is code 29");

    /* The gap. 2500 falls in neither range and the vendor programs 0, which
     * is 1700 mV - not the nearest value, and not an error either. */
    sl6806_ldo_set_mv(6, 2500);
    CHECK(chip[SL6806_RF_LDO6_MV] == 0x00,
          "the 2450..2650 gap programs code 0, as the vendor does");

    /* The high bits of the register are not the field. */
    reset_model();
    chip[SL6806_RF_LDO6_MV] = 0xE0;
    sl6806_ldo_set_mv(6, 2800);
    CHECK(chip[SL6806_RF_LDO6_MV] == 0xF3, "bits [7:5] are preserved");

    /* Channel 5 shares the scale, channels 2 and 3 have their own. */
    reset_model();
    sl6806_ldo_set_mv(5, 2800);
    CHECK(chip[SL6806_RF_LDO5_MV] == 0x13 && sl6806_ldo_get_mv(5) == 2800,
          "channel 5 uses the same split scale");
    sl6806_ldo_set_mv(2, 1800);
    CHECK(chip[SL6806_RF_LDO2_MV] == 0x03 && sl6806_ldo_get_mv(2) == 1800,
          "channel 2 is 1500 mV + n*100");
    sl6806_ldo_set_mv(3, 2850);
    CHECK(chip[SL6806_RF_LDO3_MV] == 0x04 && sl6806_ldo_get_mv(3) == 2850,
          "channel 3 is 2650 mV + n*50");

    /* Channel 4 is a menu, and an unlisted voltage is refused rather than
     * rounded - the vendor leaves the field alone in that case. */
    CHECK(sl6806_ldo_set_mv(4, 2750) == -1, "channel 4 refuses 2750 mV");
    CHECK(sl6806_ldo_get_mv(4) == -1, "and has no readback");
}

static void test_camera(void)
{
    printf("the camera sequence\n");

    reset_model();
    CHECK(sl6806_camera_power_on() == 0, "power-on succeeds");
    CHECK(chip[SL6806_RF_LDO6_MV] == 0x13, "rail 6 at 2800 mV");
    CHECK((chip[SL6806_RF_LDO_ENABLE] & 0x10) == 0x10, "rail 6 enabled");
    CHECK((chip[SL6806_RF_CAM_FIELD] & 0x18) == 0x08, "field [4:3] = 01");
    CHECK((chip[SL6806_RF_CAM_ENABLE] & 0x80) == 0x80, "camera enabled");

    /* The order is the vendor's: off, voltage, on - not voltage into a live
     * rail. The trace is the only place this can be checked. */
    {
        const char *off = strstr(trace, "W2C=00");
        const char *mv  = strstr(trace, "W7A=13");
        const char *on  = strstr(trace, "W2C=10");

        CHECK(off && mv && on, "all three rail writes happened");
        if (off && mv && on)
            CHECK(off < mv && mv < on, "and in the vendor's order");
    }

    CHECK(sl6806_camera_power_off() == 0, "power-off succeeds");
    CHECK((chip[SL6806_RF_LDO_ENABLE] & 0x10) == 0, "rail 6 off again");
    CHECK((chip[SL6806_RF_CAM_ENABLE] & 0x80) == 0, "camera bit cleared");
    CHECK((chip[SL6806_RF_CAM_FIELD] & 0x18) == 0, "field cleared");

    /* A failure names its step rather than returning a bare -1. */
    reset_model();
    hang = 1;
    CHECK(sl6806_camera_power_on() == -1, "a dead mailbox fails at step 1");
}

int main(void)
{
    printf("test_regfile\n");
    test_transfers();
    test_update();
    test_rails();
    test_voltages();
    test_camera();

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
