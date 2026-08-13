/*
 * FmDemo - tune the P20's FM radio, and prove it tuned.
 *
 * The tuner is an RDA5807-family part on TWI bus 0, found while looking for
 * the camera and documented in docs/sl6806_re_notes.md §7i. The stock
 * firmware's driver reads a chip id of 0x5808 from it at address 0x10, and
 * examples/CameraDemo already reads that same id back over bit-banged GPIO -
 * so the bus, the pads and the part are all established before this sketch
 * starts. What is new here is writing to it.
 *
 * ===================================================================
 *  HOW THIS PROVES ITSELF WITHOUT A SPEAKER
 * ===================================================================
 * "The radio works" is normally something you check with your ears, and this
 * framework has no audio driver - the codec, the DAC and the headphone path
 * are all undecoded. So the demo is built around what the tuner itself will
 * tell you over I2C:
 *
 *   STC      the chip sets it when a tune completes. It is the difference
 *            between "the write was accepted" and "the PLL locked".
 *   READCHAN the channel the chip believes it is on, which should read back
 *            as the channel that was asked for.
 *   RSSI     received signal strength, 0..127. Different frequencies giving
 *            different values is the strongest evidence available here that
 *            an antenna and a real radio are on the other end, rather than a
 *            register file that happily stores whatever it is given.
 *   FM_TRUE  the chip's own opinion that the current frequency is a station.
 *
 * A sweep across the band that returns a flat RSSI would mean the writes are
 * landing somewhere inert. A sweep with peaks means a working receiver.
 *
 * NOTE ON THE ANTENNA: on this device the headphone lead is the aerial (the
 * stock UI says so in five languages). With nothing plugged in, expect weak
 * and mostly flat RSSI - that is the antenna missing, not the driver failing.
 *
 * ===================================================================
 *  WHAT IT WRITES
 * ===================================================================
 * Only the RDA5807's own registers, and only the ones a tune needs: 0x02
 * (enable, mute state) and 0x03 (channel and tune). Nothing else on the board
 * is touched - no pads beyond the two bus lines, no clock, no power register.
 * Audio comes up muted and stays muted unless FM_UNMUTE is set below, because
 * an MP3 player that suddenly makes noise is a poor way to learn that a
 * register write worked.
 *
 *     make SKETCH=examples/FmDemo RUN_MODE=poll run
 *
 * Poll mode because the scan is many small steps; see examples/RegFileProbe
 * for what the default run mode does to a sketch shaped like this.
 */

#include <Arduino.h>
#include "sl6806_padctl.h"
#include "sl6806_console.h"

/* ------------------------------------------------------- the bus pads */

/* [V] §7h/§7i, and settled on hardware: SDA is bank 4 pin 12, SCL pin 13 -
 * the opposite of the order §7h lists them in. Pull selector 8 is the
 * vendor's own for these pads. */
#define FM_PAD_SDA   0x00046138u
#define FM_PAD_SCL   0x00046938u
#define PAD_WHICH(id) ((uint32_t)(id) & 0x000FF800u)
#define PAD_AT_REST(id) (PAD_WHICH(id) | (3u << 4) | 8u)

/* --------------------------------------------------------- the tuner */

#define FM_ADDR_SEQ     0x10u   /* [V] sequential access, from register 0x02 */
#define FM_ADDR_RANDOM  0x11u   /* [V] random access, register number first  */
#define FM_CHIP_ID      0x5808u /* [V] 0x00D3D988                            */

/*
 * Published RDA5807 register bits. These are not from the dump - the vendor
 * driver only ever reads the id and writes the enable - so they carry the
 * datasheet's authority rather than this device's, and a mismatch here is a
 * mismatch with the part, not with the SL6806.
 */
#define R02_DHIZ        0x8000u   /* audio output enabled (not high-Z) */
#define R02_DMUTE       0x4000u   /* 1 = un-muted                      */
#define R02_MONO        0x2000u
#define R02_SEEK        0x0100u
#define R02_CLK_32K     0x0000u
#define R02_NEW_METHOD  0x0004u
#define R02_RESET       0x0002u
#define R02_ENABLE      0x0001u

#define R03_TUNE        0x0010u
#define R03_BAND_87_108 0x0000u
#define R03_SPACE_100K  0x0000u

#define R0A_STC         0x4000u
#define R0A_ST          0x0400u
#define R0B_FM_TRUE     0x0100u

/* Leave the audio muted. Set to 1 only if you want to hear it. */
#define FM_UNMUTE       0

/* The band, in 100 kHz channels above 87.0 MHz. */
#define FM_BASE_KHZ     87000
#define FM_STEP_KHZ     100
#define FM_CHANNELS     210      /* 87.0 .. 108.0 MHz */

/* Which frequencies to sweep, and how long to give each one to tune. */
#define SWEEP_STEP_CH   5        /* every 500 kHz */
#define TUNE_TIMEOUT_MS 60

#define I2C_HALF_US     5
#define I2C_STRETCH_US  2000

/* ------------------------------------------------------- the I2C lines */

static inline void sda_low(void)     { sl6806_pad_set_func(FM_PAD_SDA, SL6806_PAD_FUNC_OUTPUT); }
static inline void sda_release(void) { sl6806_pad_set_func(FM_PAD_SDA, SL6806_PAD_FUNC_INPUT); }
static inline void scl_low(void)     { sl6806_pad_set_func(FM_PAD_SCL, SL6806_PAD_FUNC_OUTPUT); }
static inline int  sda_read(void)    { return sl6806_pad_read(FM_PAD_SDA); }
static void half(void) { delayMicroseconds(I2C_HALF_US); }

static int scl_release(void)
{
    uint32_t waited = 0;

    sl6806_pad_set_func(FM_PAD_SCL, SL6806_PAD_FUNC_INPUT);
    while (sl6806_pad_read(FM_PAD_SCL) == 0) {
        if (waited >= I2C_STRETCH_US)
            return 0;
        delayMicroseconds(1);
        waited++;
    }
    return 1;
}

static void busInit(void)
{
    sl6806_pad_configure(PAD_AT_REST(FM_PAD_SDA));
    sl6806_pad_configure(PAD_AT_REST(FM_PAD_SCL));
    sl6806_pad_write(FM_PAD_SDA, 0);
    sl6806_pad_write(FM_PAD_SCL, 0);
}

static void i2cStart(void)
{
    sda_release(); (void)scl_release(); half();
    sda_low();     half();
    scl_low();     half();
}

static void i2cStop(void)
{
    sda_low();     half();
    (void)scl_release(); half();
    sda_release(); half();
}

static int i2cWrite(uint8_t byte)
{
    int i, ack;

    for (i = 7; i >= 0; i--) {
        if ((byte >> i) & 1) sda_release(); else sda_low();
        half();
        if (!scl_release()) return 0;
        half();
        scl_low();
    }
    sda_release();
    half();
    if (!scl_release()) return 0;
    ack = (sda_read() == 0);
    half();
    scl_low();
    return ack;
}

static uint8_t i2cRead(int ack)
{
    uint8_t v = 0;
    int i;

    sda_release();
    for (i = 0; i < 8; i++) {
        half();
        if (!scl_release()) return v;
        v = (uint8_t)((v << 1) | (sda_read() & 1));
        half();
        scl_low();
    }
    if (ack) sda_low(); else sda_release();
    half();
    (void)scl_release();
    half();
    scl_low();
    sda_release();
    return v;
}

/* ---------------------------------------------------- the tuner's API */

/*
 * Sequential access: a write starts at register 0x02 and every further pair
 * of bytes advances one register, so writing four bytes sets 0x02 and 0x03.
 * That is the whole interface a tune needs.
 */
static int fmWriteRegs(const uint16_t *regs, int n)
{
    int i;

    i2cStart();
    if (!i2cWrite((uint8_t)(FM_ADDR_SEQ << 1))) goto fail;
    for (i = 0; i < n; i++) {
        if (!i2cWrite((uint8_t)(regs[i] >> 8))) goto fail;
        if (!i2cWrite((uint8_t)(regs[i]      ))) goto fail;
    }
    i2cStop();
    return 1;
fail:
    i2cStop();
    return 0;
}

/*
 * Sequential read starts at register 0x0A, which is where the status lives -
 * the same read the vendor driver uses to fetch the chip id out of bytes 8
 * and 9. `out` receives registers 0x0A upwards.
 */
static int fmReadRegs(uint16_t *out, int n)
{
    int i;

    i2cStart();
    if (!i2cWrite((uint8_t)((FM_ADDR_SEQ << 1) | 1))) {
        i2cStop();
        return 0;
    }
    for (i = 0; i < n; i++) {
        uint8_t hi = i2cRead(1);
        uint8_t lo = i2cRead(i != n - 1);
        out[i] = (uint16_t)((hi << 8) | lo);
    }
    i2cStop();
    return 1;
}

static unsigned chanToKHz(unsigned ch) { return FM_BASE_KHZ + ch * FM_STEP_KHZ; }

static void printMHz(unsigned khz)
{
    Serial.print(khz / 1000);
    Serial.print(".");
    Serial.print((khz % 1000) / 100);
}

/* ------------------------------------------------------- the sketch */

enum { ST_ID, ST_ENABLE, ST_SWEEP, ST_REPORT, ST_DONE };

static uint8_t  state = ST_ID;
static unsigned ch;
static uint32_t step_at;
static int      tuned_ok, best_rssi, best_ch;

static int roomToPrint(void)
{
    return sl6806_console_space() >= (int)(SL6806_CONSOLE_SIZE / 2);
}

/* Tune to a channel and wait for the chip to say it got there. Returns RSSI,
 * or -1 if the tune never completed. */
static int tuneTo(unsigned channel)
{
    uint16_t regs[2], st[3];
    uint32_t start;

    regs[0] = R02_DHIZ | R02_NEW_METHOD | R02_ENABLE
            | (FM_UNMUTE ? R02_DMUTE : 0);
    regs[1] = (uint16_t)((channel << 6) | R03_TUNE | R03_BAND_87_108
                                        | R03_SPACE_100K);
    if (!fmWriteRegs(regs, 2))
        return -1;

    start = millis();
    do {
        if (!fmReadRegs(st, 3))
            return -1;
        if (st[0] & R0A_STC) {
            int rssi = (st[1] >> 9) & 0x7F;
            /* Report what the chip thinks it tuned to, not what we asked. */
            unsigned readch = st[0] & 0x03FF;
            if (readch != channel)
                return -2 - (int)readch;   /* encoded mismatch */
            return rssi;
        }
    } while (millis() - start < TUNE_TIMEOUT_MS);

    return -1;
}

void setup()
{
    Serial.begin(115200);
    Serial.println();
    Serial.println("=== SL6806 FM tuner demo ===");
    Serial.println("RDA5807 on TWI 0 at 0x10, bit-banged on two pads - notes 7i.");
    Serial.println("No audio driver exists on this chip, so the proof is the");
    Serial.println("tuner's own status: tune-complete, the channel it reads back,");
    Serial.println("and RSSI varying across the band.");
    Serial.println();

    sl6806_gpio_vendor_register(sl6806_padctl_vendor());
    busInit();
    step_at = millis();
}

void loop()
{
    if (state == ST_DONE || !roomToPrint())
        return;

    switch (state) {

    case ST_ID: {
        uint16_t r[5];

        if (!fmReadRegs(r, 5)) {
            Serial.println("no answer at 0x10. Is this a P20 with the FM part?");
            state = ST_DONE;
            break;
        }
        Serial.print("chip id 0x");
        Serial.print(r[4], HEX);
        if (r[4] != FM_CHIP_ID) {
            Serial.println(" - not the 0x5808 the vendor driver expects.");
            Serial.println("Carrying on anyway; the register map may differ.");
        } else {
            Serial.println(" - the RDA5807 the vendor driver expects.");
        }
        state = ST_ENABLE;
        break;
    }

    case ST_ENABLE: {
        uint16_t regs[1];

        Serial.println("enabling the tuner (audio stays muted) ...");
        regs[0] = R02_DHIZ | R02_NEW_METHOD | R02_ENABLE;
        if (!fmWriteRegs(regs, 1)) {
            Serial.println("  the enable write was not ACKed.");
            state = ST_DONE;
            break;
        }
        ch = 0;
        best_rssi = -1;
        best_ch = -1;
        state = ST_SWEEP;
        step_at = millis();
        break;
    }

    case ST_SWEEP: {
        int rssi;

        if (millis() - step_at < 5)      /* let the last tune settle */
            break;

        rssi = tuneTo(ch);
        if (rssi >= 0) {
            tuned_ok++;
            Serial.print("  ");
            printMHz(chanToKHz(ch));
            Serial.print(" MHz  RSSI ");
            Serial.print(rssi);
            if (rssi > best_rssi) { best_rssi = rssi; best_ch = (int)ch; }
            Serial.println();
        } else if (rssi == -1) {
            Serial.print("  ");
            printMHz(chanToKHz(ch));
            Serial.println(" MHz  no tune-complete");
        } else {
            Serial.print("  ");
            printMHz(chanToKHz(ch));
            Serial.print(" MHz  chip reports channel ");
            Serial.println(-(rssi + 2));
        }

        ch += SWEEP_STEP_CH;
        step_at = millis();
        if (ch >= FM_CHANNELS)
            state = ST_REPORT;
        break;
    }

    case ST_REPORT:
        Serial.println();
        if (!tuned_ok) {
            Serial.println("Nothing tuned. The chip answers its address but never");
            Serial.println("reports tune-complete, so the writes are reaching a part");
            Serial.println("that is not tuning - check the register map before the bus.");
        } else {
            Serial.print(tuned_ok);
            Serial.println(" channels tuned and reported back their own channel");
            Serial.println("number, which is the tuner acting on what it was told.");
            Serial.print("Strongest: ");
            printMHz(chanToKHz((unsigned)best_ch));
            Serial.print(" MHz at RSSI ");
            Serial.println(best_rssi);
            Serial.println();
            Serial.println("If RSSI is flat across the band, the aerial is missing -");
            Serial.println("on this device that is the headphone lead. Plug headphones");
            Serial.println("in and run it again to see the peaks appear.");
        }
        state = ST_DONE;
        break;
    }
}
