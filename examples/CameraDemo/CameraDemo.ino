/*
 * CameraDemo - power up the P20's camera sensor and find it on its I2C bus.
 *
 * The sensor is the 1 MP part the stock firmware calls "sc101", on TWI bus 0
 * at address 0x68, decoded register for register in docs/sl6806_re_notes.md
 * §7h. Its identity is two byte reads: register 0xF7 must be 0xDA and 0xF8
 * must be 0x4A.
 *
 * ===================================================================
 *  THIS FINDS THE SENSOR. IT DOES NOT TAKE A PICTURE.
 * ===================================================================
 * Pixels leave the sensor on a DVP/CSI parallel bus into hardware JPEG and
 * H.264 encoders, and none of that is decoded - the firmware reaches the
 * front end as a named channel resource, not as a register block. So the
 * honest scope is: bring the sensor out of reset, get on its bus, and find
 * out what is there.
 *
 * NO TWI CONTROLLER, SAME AS THE TOUCH PANEL. The SL6806's TWI base has never
 * been located. examples/TouchDemo answered that for bus 1 by bit-banging the
 * two TWI1 pads as GPIO, and bus 0 is the same situation with different ids:
 *
 *     SDA/SCL  0x00046938, 0x00046138   bank 4 pins 13 and 12, alt function 2
 *     RESET    0x00047080               bank 4 pin 14, output
 *     PWDN     0x00047880               bank 4 pin 15, output
 *     MCLK     0x00041930               bank 4 pin 3,  alt function 2
 *
 * All five are [V], read as immediates out of this unit's own firmware.
 *
 * ===================================================================
 *  RESULT: THE BUS WORKS. THE CAMERA IS NOT THE THING ON IT.
 * ===================================================================
 * Run on a P20, in three rounds, and the middle one is the useful one:
 *
 *   1. Reset the sensor, read 0xF7 under four combinations - nothing
 *      answered under any of them.
 *   2. Measure the lines and scan the whole address range instead of asking
 *      0x68 alone. Both bus pads idle high and move. With **SDA on bank 4
 *      pin 12 and SCL on pin 13** three addresses answered - 0x10, 0x11 and
 *      0x60 - and with the pair the other way round, nothing did.
 *   3. Those three are the FM tuner, not the camera. The firmware's own FM
 *      driver reads 0x5808 as a chip id over bus 0 at address 0x10, which is
 *      an RDA5807 and its two aliases.
 *
 * Two conclusions, and they point in opposite directions. §7h lists the TWI0
 * pair in an order that turns out to be the wrong way round, and this sketch
 * now leads with the assignment that works. And the bus itself is no longer
 * a suspect: it has a device on it that answers with the exact value its own
 * driver checks for, which this sketch verifies as a self-test before it
 * draws any conclusion about a silent 0x68. What is left for the camera is
 * MCLK and sensor power, neither of which is on this bus.
 *
 * So the sketch measures rather than asking, in three stages:
 *
 * 1. THE LINES, before any transfer. Each bus pad is put on function 0 - the
 *    one function §7g measured as leaving the input buffer alive - through
 *    every pull selector in the ROM's table, and read back. That says which
 *    selectors hold the line high, or whether *nothing* does, which would
 *    mean there is no usable pull-up and bit-banged I2C on this bus is dead
 *    before it starts. Then each line is driven low and released, to see
 *    whether the pad moves at all.
 *
 * 2. WHICH FAILURE. The I2C primitives now record why they gave up. A slave
 *    that never ACKs and a clock line that never rises both used to print
 *    "no answer", and they have opposite causes: the first is about the
 *    device, the second means nothing was ever clocked and no conclusion
 *    about any device is available.
 *
 * 3. WHO IS THERE, not just whether 0x68 is. Each pass scans 0x08..0x77 and
 *    reports every address that ACKs. A sensor at an unexpected address, a
 *    bus with something else on it, and an empty bus are three different
 *    findings that "0x68 did not answer" cannot tell apart. A scan that ACKs
 *    at *every* address is its own finding - SDA stuck low reads as an ACK
 *    from everybody - and is called out rather than printed 112 times.
 *
 * Those run under a matrix of the three things that are assumptions rather
 * than readings:
 *
 *   SDA/SCL     §7h lists the pair in one order, and in that table the id
 *               order and the pin order disagree, so which pad is which is
 *               a guess. Both ways are tried.
 *   MCLK        The vendor muxes it to alternate function 2 and programs
 *               clock channel 6 with 2800; that channel's registers are
 *               unknown. This family clocks its register block from MCLK, so
 *               with no clock the chip can be alive and never ACK. Tried as
 *               the vendor's mux (in case something already drives the
 *               channel) and as a software square wave on the pad.
 *   RESET/PWDN  §7h attributes 0x47080 to RESET and 0x47880 to PWDN, and
 *               under that reading the vendor's sequence ends with PWDN
 *               *high* before it reads the chip id - which is backwards for
 *               a line called power-down, and reads much more naturally if
 *               the two ids are the other way round. So both assignments are
 *               tried; the sequence itself is the vendor's either way.
 *
 * IT ONLY EVER WRITES ONE SENSOR REGISTER, and only after a chip id has come
 * back: the page selector 0xF0, which is how a 16-bit register number is
 * addressed at all. It is restored to 0 afterwards.
 *
 *     make SKETCH=examples/CameraDemo run
 *     make SKETCH=examples/CameraDemo RUN_MODE=poll run   (if loop() never ticks)
 *
 * Payload mode never writes flash, so an unplug undoes anything here.
 */

#include <Arduino.h>
#include "sl6806_padctl.h"

/* ------------------------------------------------------------- the pads */

/*
 * [V] §7h. Both power lines already carry function 1 - output - in the
 * vendor's word, so these are the configuration ids and the write ids at
 * once. They are also PIN_CAM_RESET and PIN_CAM_PWDN in the variant; the
 * sketch drives them by id because it swaps their roles as one of its
 * variables, which a fixed pin number cannot express.
 */
#define CAM_PAD_RESET  0x00047080u   /* bank 4 pin 14, = PIN_CAM_RESET */
#define CAM_PAD_PWDN   0x00047880u   /* bank 4 pin 15, = PIN_CAM_PWDN  */

/* [V] MCLK as the vendor leaves it: bank 4 pin 3, alternate function 2,
 * drive 3. Useful only if something is driving clock channel 6. */
#define CAM_MCLK_MUX   0x00041930u

/* The same pad as a plain output, so software can square-wave it. Only the
 * function nibble differs from the vendor's word - drive 3 and the rest come
 * along unchanged. */
#define CAM_MCLK_GPIO  ((CAM_MCLK_MUX & ~(0x0Fu << 7)) | \
                        (SL6806_PAD_FUNC_OUTPUT << 7))

/* [V] The TWI0 pair, in the order §7h lists them. Only the bank and pin
 * fields are used - the function nibble is this sketch's to drive. */
#define CAM_PAD_A      0x00046938u   /* bank 4 pin 13 */
#define CAM_PAD_B      0x00046138u   /* bank 4 pin 12 */

/* Bank and pin of a pad id, with every configuration field cleared. */
#define PAD_WHICH(id) ((uint32_t)(id) & 0x000FF800u)

/*
 * A bus pad at rest: input (function 0), the vendor's drive 3, and a pull
 * selector chosen at run time by the sweep below rather than assumed. That
 * is what makes open drain possible on a pad controller with no open-drain
 * mode: configure once as a pulled-up input with a 0 latched in the output
 * register, then flip the function nibble to 1 to drive low and back to 0 to
 * release. sl6806_pad_set_func() touches that nibble only, so the drive and
 * pull set here survive every edge.
 */
#define PAD_AT_REST(id, sel)  (PAD_WHICH(id) | (3u << 4) | ((sel) & 0x0Fu))

/* [V] §7h's own selector for these pads, and the sweep's starting point. */
#define PULL_VENDOR    8u

/* ---------------------------------------------------------- the sensor */

#define CAM_ADDR        0x68u   /* [V] 7-bit; §7h                          */

/*
 * The other thing on this bus, and the reason the probe can now prove itself.
 *
 * A scan on hardware found 0x10, 0x11 and 0x60 answering - and the firmware
 * says what they are. `FM chip id: 0x%x` is logged at 0x00D3D97A, the driver
 * around it compares what it reads against **0x5808**, and its two I2C
 * wrappers at 0x00D3D6F8 / 0x00D3D70A call the vendor's twi_read / twi_write
 * with **bus 0, address 0x10**. That is an RDA5807-family FM tuner: 0x10 is
 * its sequential-access address, 0x11 its random-access one and 0x60 its
 * TEA5767-compatible alias, which is exactly the trio the scan saw.
 *
 * So this bus has a known-good device on it with a known-good expected
 * answer, which turns the probe into something better than a probe: reading
 * 0x5808 off the tuner proves the pads, the assignment, the timing, the ACK
 * handling and the byte order all at once. After that, a silent 0x68 is a
 * statement about the camera and not about this sketch.
 */
#define FM_ADDR_SEQ     0x10u   /* [V] the address the driver uses          */
#define FM_ADDR_RANDOM  0x11u   /* [V] seen by the scan                     */
#define FM_ADDR_TEA     0x60u   /* [V] seen by the scan                     */
#define FM_CHIP_ID      0x5808u /* [V] 0x00D3D988                           */
#define FM_READ_LEN     10      /* [V] the driver's own read length         */
#define CAM_REG_ID_HI   0xF7u   /* [V] must read 0xDA                      */
#define CAM_REG_ID_LO   0xF8u   /* [V] must read 0x4A                      */
#define CAM_ID_HI       0xDAu   /* [V]                                     */
#define CAM_ID_LO       0x4Au   /* [V]                                     */
#define CAM_REG_PAGE    0xF0u   /* [V] high byte of a 16-bit register       */

/* The usual 7-bit scan range: 0x00-0x07 and 0x78-0x7F are reserved. */
#define SCAN_FIRST      0x08u
#define SCAN_LAST       0x77u

/* More than a handful of ACKs is not a populated bus, it is a stuck line. */
#define SCAN_TOO_MANY   8

/* --------------------------------------------------------- bus timing */

/* Half a bit, microseconds, when MCLK is not being toggled. The vendor runs
 * bus 0 at 100 kHz (twi_init(0, 0x186A0), §7h); this is about the same
 * before call overhead, and slower is always safe. */
#define I2C_HALF_US    5

/* How long to keep MCLK running after the power-up before asking the sensor
 * anything. A part of this family brings its register block up off MCLK, so
 * a clock that only exists during the transfer may be a clock that arrives
 * too late; this gives it a settled one first. */
#define MCLK_SETTLE_MS  30

/* How long to wait for a slave that is stretching the clock. Generous: this
 * is an error path, and in poll mode it still has to return promptly. */
#define I2C_STRETCH_US 2000

/* Settling time after changing a pull, before believing what the pad reads.
 * An internal pull against a line's capacitance is not instant, and reading
 * too early would report every pull-up as broken. */
#define PULL_SETTLE_US 500

/* How long to run MCLK inside one loop() call while a power-up delay is being
 * waited out. Short enough that poll mode's SCSI handler still returns in
 * time, long enough that the gaps between loop() calls are the only breaks in
 * the clock. */
#define MCLK_BURST_US   1000

/* ---------------------------------------------------------- the sketch */

#define HEARTBEAT_MS   5000    /* proof of life while nothing happens      */
#define SAMPLE_MS      2000    /* how often to re-read a sensor that works */

enum { MCLK_MUX, MCLK_TOGGLE };

typedef struct {
    uint32_t sda;
    uint32_t scl;
    uint32_t reset;
    uint32_t pwdn;
    uint8_t  mclk;
} attempt_t;

/*
 * ===================================================================
 *  SDA AND SCL ARE SETTLED, AND §7h HAD THEM THE OTHER WAY ROUND
 * ===================================================================
 * On hardware, a scan answered from three addresses with SDA on **bank 4 pin
 * 12** and SCL on **pin 13**, and answered from nothing at all the other way
 * round. That is not a matter of degree - one assignment has an entire FM
 * tuner on it and the other has silence - so the pair below leads with it and
 * §7h's listed order is now known to be the wrong reading of that table.
 *
 * The other assignment stays in the matrix, because it costs one pass and
 * because it is the control: if a future unit answers on the other pairing,
 * that is a board difference worth seeing rather than a mystery.
 */
static const attempt_t attempts[] = {
    { CAM_PAD_B, CAM_PAD_A, CAM_PAD_RESET, CAM_PAD_PWDN,  MCLK_MUX    },
    { CAM_PAD_B, CAM_PAD_A, CAM_PAD_RESET, CAM_PAD_PWDN,  MCLK_TOGGLE },
    { CAM_PAD_A, CAM_PAD_B, CAM_PAD_RESET, CAM_PAD_PWDN,  MCLK_MUX    },
    { CAM_PAD_A, CAM_PAD_B, CAM_PAD_RESET, CAM_PAD_PWDN,  MCLK_TOGGLE },
    /* The same four with the power lines read the other way round. */
    { CAM_PAD_B, CAM_PAD_A, CAM_PAD_PWDN,  CAM_PAD_RESET, MCLK_MUX    },
    { CAM_PAD_B, CAM_PAD_A, CAM_PAD_PWDN,  CAM_PAD_RESET, MCLK_TOGGLE },
    { CAM_PAD_A, CAM_PAD_B, CAM_PAD_PWDN,  CAM_PAD_RESET, MCLK_MUX    },
    { CAM_PAD_A, CAM_PAD_B, CAM_PAD_PWDN,  CAM_PAD_RESET, MCLK_TOGGLE },
};
#define NATTEMPTS ((int)(sizeof(attempts) / sizeof(attempts[0])))

enum {
    STEP_ANNOUNCE,      /* say loop() is alive before any delay            */
    STEP_PULL_SWEEP,    /* which pull selectors hold each line high        */
    STEP_DRIVE_TEST,    /* does each line move when driven low             */
    STEP_POWER_LOW,     /* PWDN low, RESET low                             */
    STEP_RESET_HIGH,    /* release RESET after 5 ms                        */
    STEP_PWDN_HIGH,     /* release PWDN after another 5 ms                 */
    STEP_SETTLE,        /* the vendor's 20 ms before the chip-id read      */
    STEP_SCAN,          /* who ACKs, anywhere on the bus                   */
    STEP_TALK,          /* the sensor answered: keep reading it            */
    STEP_STUCK          /* nothing answered under any combination          */
};

static uint8_t  step = STEP_ANNOUNCE;
static uint32_t step_at;

static int      attempt;              /* index into attempts[]          */
static uint32_t pad_sda, pad_scl;     /* the assignment being tried     */
static uint32_t pad_reset, pad_pwdn;
static uint8_t  mclk_mode;
static unsigned pull_sel = PULL_VENDOR;

static bool     lines_idle_high;      /* both lines rest high           */
static bool     saw_fm;               /* the FM tuner answered a scan   */
static bool     bus_proved;           /* and gave its documented id     */
static uint32_t mclk_khz;             /* what the square wave measured  */
static unsigned ctrl_drive;           /* drive strength for RESET/PWDN  */
static bool     reset_stuck;          /* RESET cannot be driven high    */
static bool     reset_floats;         /* nothing pulls the RESET net    */
static bool     pwdn_floats;          /* nor the PWDN one               */
static uint32_t heartbeat_at;
static uint32_t sample_at;
static uint32_t samples;

/* Why the last transfer stopped. A NACK is about the device; a clock that
 * never rose means nothing was clocked and says nothing about any device. */
enum { BUS_OK, BUS_NACK, BUS_SCL_STUCK };
static uint8_t bus_error;

/* ------------------------------------------------------------ the MCLK */

/*
 * The fast path to one pad.
 *
 * sl6806_pad_write() decodes the packed id, looks the bank base up and range
 * checks it on every call, which is right for a GPIO API and wrong for a
 * clock: it put MCLK somewhere around a megahertz, which is at the bottom of
 * what a sensor of this family will run its register block on. The bank base
 * table and the set/clear offsets are both public (sl6806_padctl.h), so the
 * two addresses can be worked out once and the edge becomes a single store.
 */
static volatile uint32_t *mclk_set;
static volatile uint32_t *mclk_clr;
static uint32_t           mclk_mask;

static void mclkFastInit(void)
{
    uint32_t base = sl6806_pad_bank_base[SL6806_PAD_BANK(CAM_MCLK_GPIO)];

    /* +0x034 sets a pin, +0x038 clears it; one bit per pin. [V], ROM 0x6DE. */
    mclk_set  = (volatile uint32_t *)(base + 0x034u);
    mclk_clr  = (volatile uint32_t *)(base + 0x038u);
    mclk_mask = 1u << SL6806_PAD_PIN(CAM_MCLK_GPIO);
}

static inline void mclk_pulse(void)
{
    *mclk_set = mclk_mask;
    *mclk_clr = mclk_mask;
}

/*
 * Run MCLK for a length of *time* rather than a number of edges.
 *
 * This is the fix for the second half of the problem. Counting pulses meant
 * the clock ran for however long that many edges happened to take, so making
 * the edges faster made the clock stop sooner - the duty cycle of the whole
 * scheme stayed just as bad. Spinning on micros() instead means the pad is
 * clocked for the entire interval, and the interval is also the I2C bit
 * timing, so the sensor gets a clock during precisely the transfers that need
 * one.
 */
static void mclkSpin(uint32_t us)
{
    uint32_t start;

    if (mclk_mode != MCLK_TOGGLE) {
        delayMicroseconds(us);
        return;
    }

    /* Never spin on a clock that is not running: if no counter advances,
     * micros() is frozen and this would never return - and in poll mode that
     * takes the device off the USB bus until it is unplugged. */
    if (!sl6806_tick_mask()) {
        delayMicroseconds(us);
        return;
    }

    start = micros();
    do {
        int i;
        /* A batch between clock reads: micros() costs far more than an edge,
         * and reading it every cycle would halve the frequency. */
        for (i = 0; i < 32; i++)
            mclk_pulse();
    } while (micros() - start < us);
}

/* Measure what the pad is actually being given, because "a software square
 * wave" is not a specification and the answer decides whether MCLK stays a
 * suspect. Counts edges across a fixed window and returns kHz. */
static uint32_t mclkMeasure(void)
{
    uint32_t start, elapsed, pulses = 0;

    if (!sl6806_tick_mask())
        return 0;

    start = micros();
    do {
        int i;
        for (i = 0; i < 32; i++)
            mclk_pulse();
        pulses += 32;
    } while ((elapsed = micros() - start) < 10000u);

    if (!elapsed)
        return 0;
    return (pulses * 1000u) / elapsed;      /* cycles per ms = kHz */
}

/* ------------------------------------------------------- the I2C lines */

static inline void sda_low(void)     { sl6806_pad_set_func(pad_sda, SL6806_PAD_FUNC_OUTPUT); }
static inline void sda_release(void) { sl6806_pad_set_func(pad_sda, SL6806_PAD_FUNC_INPUT); }
static inline void scl_low(void)     { sl6806_pad_set_func(pad_scl, SL6806_PAD_FUNC_OUTPUT); }
static inline int  sda_read(void)    { return sl6806_pad_read(pad_sda); }

/*
 * Half a bit period. In toggle mode the wait *is* the clock: the sensor gets
 * MCLK edges exactly while the bus is moving, which is when it needs them
 * most. In mux mode this is a plain delay.
 */
static void half(void)
{
    /* mclkSpin() falls back to a plain delay when MCLK is not being toggled,
     * so the bus timing is the same either way. */
    mclkSpin(I2C_HALF_US);
}

/* Release SCL and wait for it to actually rise: a slave is allowed to hold it
 * down to buy itself time, and a bit-banged master that ignores that reads
 * the previous bit twice. Returns 0 if it never rose, and records that, since
 * it is a different finding from a NACK. */
static int scl_release(void)
{
    uint32_t waited = 0;

    sl6806_pad_set_func(pad_scl, SL6806_PAD_FUNC_INPUT);

    while (sl6806_pad_read(pad_scl) == 0) {
        if (waited >= I2C_STRETCH_US) {
            bus_error = BUS_SCL_STUCK;
            return 0;
        }
        mclkSpin(1);
        waited++;
    }
    return 1;
}

/* Both bus pads to their resting state, with a 0 waiting in the output
 * register so that "output" means "low" for the rest of the run. */
static void busInit(void)
{
    sl6806_pad_configure(PAD_AT_REST(pad_sda, pull_sel));
    sl6806_pad_configure(PAD_AT_REST(pad_scl, pull_sel));
    sl6806_pad_write(pad_sda, 0);
    sl6806_pad_write(pad_scl, 0);
}

/*
 * If a previous transfer was cut off mid-byte the slave can still be holding
 * SDA down, and no START will be seen while it does. Nine clocks walk it out
 * of that byte. Standard I2C recovery, and cheap insurance for a bus that
 * gets interrupted every time the device is unplugged.
 */
static void busRecover(void)
{
    int i;

    for (i = 0; i < 9 && sda_read() == 0; i++) {
        scl_low();
        half();
        (void)scl_release();
        half();
    }
}

static void i2cStart(void)
{
    sda_release();
    (void)scl_release();
    half();
    sda_low();
    half();
    scl_low();
    half();
}

static void i2cStop(void)
{
    sda_low();
    half();
    (void)scl_release();
    half();
    sda_release();
    half();
}

/* Returns 1 if the slave ACKed. */
static int i2cWrite(uint8_t byte)
{
    int i, ack;

    for (i = 7; i >= 0; i--) {
        if ((byte >> i) & 1)
            sda_release();
        else
            sda_low();
        half();
        if (!scl_release())
            return 0;
        half();
        scl_low();
    }

    /* The ninth clock: the slave owns SDA. */
    sda_release();
    half();
    if (!scl_release())
        return 0;
    ack = (sda_read() == 0);
    half();
    scl_low();

    if (!ack)
        bus_error = BUS_NACK;
    return ack;
}

static uint8_t i2cRead(int ack)
{
    uint8_t v = 0;
    int i;

    sda_release();
    for (i = 0; i < 8; i++) {
        half();
        if (!scl_release())
            return v;
        v = (uint8_t)((v << 1) | (sda_read() & 1));
        half();
        scl_low();
    }

    /* Our turn on the ninth: low means "keep going". */
    if (ack)
        sda_low();
    else
        sda_release();
    half();
    (void)scl_release();
    half();
    scl_low();
    sda_release();

    return v;
}

/* Address one device and stop. The whole content of a bus scan. */
static int i2cPing(uint8_t addr7)
{
    int ack;

    bus_error = BUS_OK;
    i2cStart();
    ack = i2cWrite((uint8_t)(addr7 << 1));
    i2cStop();
    return ack;
}

/*
 * A plain read with no register phase: address for reading, then n bytes.
 * This is what the vendor's twi_read(bus, addr, buf, len) does, and the FM
 * tuner is addressed that way - its register pointer is implicit.
 */
static int i2cReadFrom(uint8_t addr7, uint8_t *buf, int n)
{
    int i;

    bus_error = BUS_OK;
    i2cStart();
    if (!i2cWrite((uint8_t)((addr7 << 1) | 1))) {
        i2cStop();
        return 0;
    }

    for (i = 0; i < n; i++)
        buf[i] = i2cRead(i != n - 1);

    i2cStop();
    return 1;
}

/* A plain write with no register phase - the other half of the vendor's
 * twi_write(bus, addr, buf, len, stop). */
static int i2cWriteTo(uint8_t addr7, const uint8_t *buf, int n)
{
    int i;

    bus_error = BUS_OK;
    i2cStart();
    if (!i2cWrite((uint8_t)(addr7 << 1)))
        goto fail;
    for (i = 0; i < n; i++)
        if (!i2cWrite(buf[i]))
            goto fail;
    i2cStop();
    return 1;

fail:
    i2cStop();
    return 0;
}

/*
 * Read `n` bytes from register `reg`, the way the vendor driver does: address
 * for writing, the register number, a repeated START, then address for
 * reading. Returns 1 on success.
 */
static int camRead(uint8_t reg, uint8_t *buf, int n)
{
    int i;

    bus_error = BUS_OK;
    i2cStart();
    if (!i2cWrite((uint8_t)(CAM_ADDR << 1)))       goto fail;
    if (!i2cWrite(reg))                            goto fail;

    i2cStart();                                    /* repeated START */
    if (!i2cWrite((uint8_t)((CAM_ADDR << 1) | 1))) goto fail;

    for (i = 0; i < n; i++)
        buf[i] = i2cRead(i != n - 1);

    i2cStop();
    return 1;

fail:
    i2cStop();
    return 0;
}

/* Write one 8-bit register. The only register this sketch ever writes is the
 * page selector. */
static int camWrite(uint8_t reg, uint8_t val)
{
    bus_error = BUS_OK;
    i2cStart();
    if (!i2cWrite((uint8_t)(CAM_ADDR << 1))) goto fail;
    if (!i2cWrite(reg))                      goto fail;
    if (!i2cWrite(val))                      goto fail;
    i2cStop();
    return 1;

fail:
    i2cStop();
    return 0;
}

/*
 * [V] §7h: the register space is paged. Register 0xF0 holds the high byte of
 * a 16-bit register number and the low byte is then an ordinary 8-bit
 * register, so "F0=32 then 03=78" is how the firmware writes 0x3203 = 0x78.
 * Reading works the same way round.
 */
static int camRead16(uint16_t reg, uint8_t *val)
{
    if (!camWrite(CAM_REG_PAGE, (uint8_t)(reg >> 8)))
        return 0;
    return camRead((uint8_t)(reg & 0xFF), val, 1);
}

/* Two consecutive paged registers as one big-endian value - how every
 * coordinate in the crop block is stored. */
static int camRead16Pair(uint16_t reg, unsigned *out)
{
    uint8_t hi, lo;

    if (!camRead16(reg, &hi) || !camRead16((uint16_t)(reg + 1), &lo))
        return 0;
    *out = ((unsigned)hi << 8) | lo;
    return 1;
}

/* -------------------------------------------------------- the pad tests */

/* Read a pad as a plain input under one pull selector. Function 0 is used
 * because §7g measured it as one of only two functions that leave the input
 * buffer alive - in an alternate function the register cannot see the pin at
 * all, so a sweep across functions would measure nothing. */
static int padLevelUnderPull(uint32_t id, unsigned sel)
{
    sl6806_pad_configure(PAD_AT_REST(id, sel));
    delayMicroseconds(PULL_SETTLE_US);
    return sl6806_pad_read(id);
}

/*
 * Sweep the ROM's pull table for one pad and report which selectors hold it
 * high. Returns the lowest selector that does, or 0 if none does - and none
 * is the finding that matters: with no pull-up there is no released-high
 * state, so nothing can ever ACK and every transfer below is meaningless.
 */
static unsigned sweepPulls(uint32_t id, const char *name)
{
    unsigned sel, found = 0;

    Serial.print("  ");
    Serial.print(name);
    Serial.print(" (bank 4 pin ");
    Serial.print(SL6806_PAD_PIN(id));
    Serial.print("): ");

    for (sel = 4; sel <= 15; sel++) {
        int level = padLevelUnderPull(id, sel);

        Serial.print(sel);
        Serial.print(level ? ":1 " : ":0 ");
        if (level && !found)
            found = sel;
    }

    /* Selector 0 is outside the table, which the driver reads as "no pull".
     * What the pad floats to is worth one column of its own: a line with an
     * external pull-up reads high here too, and that would mean the bus does
     * not depend on the internal ones at all. */
    Serial.print(" none:");
    Serial.println(padLevelUnderPull(id, 0) ? "1" : "0");

    return found;
}

/*
 * Does the pad actually move? Drive it low, then release and read.
 *
 * The read cannot happen while the pad is an output - the input buffer is off
 * outside function 0 and 14 (§7g) - so this releases first and reads
 * immediately, racing the pull against the line's capacitance. A 0 here means
 * the drive worked and the line was still low when it was sampled. A 1 does
 * not prove the opposite: a fast pull on a short trace can win the race. So
 * this reports rather than concludes.
 */
/*
 * The complement, and for a control line the more important half: drive the
 * pad HIGH and find out whether the line follows.
 *
 * A reset net with a pull-down on it is ordinary design - the part stays in
 * reset until the host asserts otherwise - so "stays low when released" says
 * nothing bad by itself. What matters is whether the push-pull output wins
 * while it is driving. This charges the line, then releases to an input with
 * *no* pull applied, so the level read afterwards is the board's doing and
 * not this sketch's.
 *
 *   immediate 1  the output reached the pin; a later 0 is just the pull-down
 *                taking it back, which is expected and harmless.
 *   immediate 0  the line is low even while being driven. Something is
 *                holding it there, and a part wired to it never leaves reset.
 *
 * `drive` is the pad's drive-strength field. The vendor configures these two
 * pads with drive 0, which is worth testing against drive 3 rather than
 * assuming: a weak driver into a stiff pull-down is exactly the failure this
 * distinguishes.
 */
static int driveHighTest(uint32_t id, const char *name, unsigned drive)
{
    uint32_t cfg = PAD_WHICH(id) | (SL6806_PAD_FUNC_OUTPUT << 7) |
                   ((drive & 3u) << 4);        /* pull selector 0 = no pull */
    int immediate, settled;

    sl6806_pad_configure(cfg);
    sl6806_pad_write(id, 1);
    delayMicroseconds(200);

    sl6806_pad_set_func(id, SL6806_PAD_FUNC_INPUT);
    immediate = sl6806_pad_read(id);
    delayMicroseconds(PULL_SETTLE_US);
    settled = sl6806_pad_read(id);

    Serial.print("  ");
    Serial.print(name);
    Serial.print(" driven high at drive ");
    Serial.print(drive);
    Serial.print(": reads ");
    Serial.print(immediate);
    Serial.print(" on release, ");
    Serial.print(settled);
    Serial.println(" after settling");

    return immediate;
}

static void driveTest(uint32_t id, const char *name)
{
    int after_release, settled;

    sl6806_pad_configure(PAD_AT_REST(id, pull_sel));
    sl6806_pad_write(id, 0);

    sl6806_pad_set_func(id, SL6806_PAD_FUNC_OUTPUT);
    delayMicroseconds(50);
    sl6806_pad_set_func(id, SL6806_PAD_FUNC_INPUT);
    after_release = sl6806_pad_read(id);

    delayMicroseconds(PULL_SETTLE_US);
    settled = sl6806_pad_read(id);

    Serial.print("  ");
    Serial.print(name);
    Serial.print(": driven low then released reads ");
    Serial.print(after_release);
    Serial.print(", after settling ");
    Serial.println(settled);

    if (after_release == 0 && settled == 1) {
        Serial.println("    moves in both directions - this line is a usable open drain.");
    } else if (settled == 0) {
        /*
         * On a bus line this is fatal. On a control line it is not even bad
         * news: a reset net is normally held low by a pull-down so the part
         * stays in reset until the host says otherwise. Saying "I2C cannot
         * work on it" about RESET, as this used to, was simply wrong.
         */
        Serial.println("    stays low after release - something external holds it down.");
        if (id == CAM_PAD_A || id == CAM_PAD_B) {
            Serial.println("    On a bus line that is fatal: no ACK can ever be seen.");
        } else {
            Serial.println("    On a control line that is ordinary - a pull-down keeps the");
            Serial.println("    part in reset. What matters is the driven test below.");
        }
    } else {
        Serial.println("    never seen low. Either the pull won the race - possible and");
        Serial.println("    harmless - or driving the pad does not reach the pin.");
    }
}

/*
 * How long does the pad hold a charge once nothing is driving it?
 *
 * This is the test that tells a connected net from an empty one, and it came
 * out of a contradiction: RESET stayed *low* after being driven low under a
 * pull-up, and stayed *high* after being driven high with no pull. A line
 * that keeps whichever state it was last given is not being pulled anywhere
 * by anything - it is a floating capacitor, which is what a pad with nothing
 * on the other end looks like.
 *
 * So: drive a level, release to an input with no pull, and time how long the
 * level survives. The physics is one line - an external resistor drags the
 * net back in microseconds, while leakage alone takes many milliseconds.
 *
 *   both directions hold the window    nothing external on this net
 *   high decays quickly               there is a pull-down out there
 *   low decays quickly                there is a pull-up out there
 *
 * The two bus pads are the control group: they are known to have external
 * pull-ups, because the sweep read them high with no internal pull at all. If
 * this test does not show their lows decaying fast, the test is wrong rather
 * than the board being interesting.
 */
#define HOLD_LIMIT_US   10000u

static uint32_t holdTime(uint32_t id, int level)
{
    uint32_t cfg = PAD_WHICH(id) | (SL6806_PAD_FUNC_OUTPUT << 7) | (3u << 4);
    uint32_t start, waited;

    sl6806_pad_configure(cfg);          /* pull selector 0 - no pull */
    sl6806_pad_write(id, level);
    delayMicroseconds(200);
    sl6806_pad_set_func(id, SL6806_PAD_FUNC_INPUT);

    if (!sl6806_tick_mask())
        return 0;

    start = micros();
    while ((waited = micros() - start) < HOLD_LIMIT_US) {
        if (sl6806_pad_read(id) != level)
            return waited;
    }
    return HOLD_LIMIT_US;
}

/* Returns 1 if the net looks floating - neither level decays. */
static int holdTest(uint32_t id, const char *name)
{
    uint32_t hi = holdTime(id, 1);
    uint32_t lo = holdTime(id, 0);
    int floating = (hi >= HOLD_LIMIT_US && lo >= HOLD_LIMIT_US);

    Serial.print("  ");
    Serial.print(name);
    Serial.print(": high held ");
    if (hi >= HOLD_LIMIT_US) Serial.print(">10ms"); else { Serial.print(hi); Serial.print("us"); }
    Serial.print(", low held ");
    if (lo >= HOLD_LIMIT_US) Serial.print(">10ms"); else { Serial.print(lo); Serial.print("us"); }
    Serial.println();

    if (floating)
        Serial.println("    holds both - nothing external pulls this net either way.");
    else if (lo < HOLD_LIMIT_US && hi >= HOLD_LIMIT_US)
        Serial.println("    low decays - there is a pull-up on this net.");
    else if (hi < HOLD_LIMIT_US && lo >= HOLD_LIMIT_US)
        Serial.println("    high decays - there is a pull-down on this net.");
    else
        Serial.println("    both decay - something is actively driving this net.");

    return floating;
}

/* ----------------------------------------------------------- reporting */

static void printAttempt(void)
{
    Serial.println();
    Serial.print("attempt ");
    Serial.print(attempt + 1);
    Serial.print("/");
    Serial.print(NATTEMPTS);
    Serial.print(": SDA = pin ");
    Serial.print(SL6806_PAD_PIN(pad_sda));
    Serial.print(", SCL = pin ");
    Serial.print(SL6806_PAD_PIN(pad_scl));
    Serial.print(", RESET = pin ");
    Serial.print(SL6806_PAD_PIN(pad_reset));
    Serial.print(", MCLK ");
    Serial.println(mclk_mode == MCLK_TOGGLE
                       ? "square-waved in software"
                       : "on alt function 2 (the vendor's mux)");
}

/*
 * The crop window, read back through the paged path. Before an init table has
 * been applied these are the sensor's own power-on defaults, not the vendor's
 * - table B would write 320/120/967/607, a centred 648x488 VGA window in a
 * 1280x720 array (§7h). So the numbers are not expected to match anything;
 * what they demonstrate is that 16-bit register addressing works, which is
 * the thing every later use of this sensor depends on.
 */
static void reportCrop(void)
{
    unsigned x0, y0, x1, y1;
    int ok;

    ok = camRead16Pair(0x3200, &x0) && camRead16Pair(0x3202, &y0) &&
         camRead16Pair(0x3204, &x1) && camRead16Pair(0x3206, &y1);

    /*
     * Put the page back. 0xF0 is sticky - it stays wherever it was last
     * written - so leaving it at 0x32 would turn the next read of 0xF7 into a
     * read of 0x32F7, and the chip id would come back as garbage from a
     * register that has nothing to do with it. That would look exactly like
     * the sensor dropping off the bus, which is the one symptom this sketch
     * most needs to keep meaningful.
     */
    (void)camWrite(CAM_REG_PAGE, 0);

    if (!ok) {
        Serial.println("  paged read failed - the 0xF0 page write or the 16-bit");
        Serial.println("  register scheme is not what notes 7h says it is.");
        return;
    }

    Serial.print("  crop window regs 0x3200..0x3207: x ");
    Serial.print(x0);
    Serial.print("..");
    Serial.print(x1);
    Serial.print(", y ");
    Serial.print(y0);
    Serial.print("..");
    Serial.println(y1);
    Serial.println("  (power-on defaults - no init table has been applied)");
}

/* Read and report the chip id. Returns 1 if it was the sc101's. */
static int reportChipId(void)
{
    uint8_t hi = 0, lo = 0;

    if (!camRead(CAM_REG_ID_HI, &hi, 1) || !camRead(CAM_REG_ID_LO, &lo, 1)) {
        Serial.println("  0x68 ACKed its address but would not give up 0xF7/0xF8.");
        Serial.println("  Whatever is there is not answering register reads.");
        return 0;
    }

    Serial.print("  chip id: 0xF7 = 0x");
    Serial.print(hi, HEX);
    Serial.print(", 0xF8 = 0x");
    Serial.println(lo, HEX);

    if (hi == CAM_ID_HI && lo == CAM_ID_LO) {
        Serial.println("  that is the sc101 - the sensor is up and talking, on pads");
        Serial.println("  alone. Notes 7h confirmed on hardware.");
        reportCrop();
        Serial.println();
        Serial.println("Still missing for an image: the DVP/CSI front end and the");
        Serial.println("JPEG/H.264 encoders behind it, none of which is decoded.");
        Serial.println("This is the bus, not the pixels.");
        return 1;
    }

    Serial.println("  not 0xDA / 0x4A. Either this part needs a real MCLK to read");
    Serial.println("  its own fuses, or it is not the sensor in the dump.");
    return 0;
}

/*
 * Prove the bus against the FM tuner, whose right answer the firmware states.
 *
 * The vendor's own sequence at 0x00D3D92C is: write the two bytes {0x00,
 * 0x02} to 0x10, wait, read ten bytes back, and compare bytes 8 and 9 as a
 * big-endian word against 0x5808. This does the read first, on its own,
 * because a read cannot change anything; only if that does not produce the
 * expected id does it add the vendor's two-byte write, which on an RDA5807
 * lands in the register whose low bit is ENABLE. That write is the one thing
 * in this sketch that changes the state of a chip it is not investigating, so
 * it is announced, it is the vendor's own constant, and payload mode gives it
 * back on an unplug.
 *
 * Returns 1 if the tuner gave its documented id.
 */
static int fmSelfTest(void)
{
    static const uint8_t enable[2] = { 0x00, 0x02 };   /* [V] 0x00D3D952 */
    uint8_t buf[FM_READ_LEN];
    unsigned id;
    int i, wrote = 0;

    Serial.println("  the bus has an FM tuner on it - checking this sketch against it.");

    for (;;) {
        if (!i2cReadFrom(FM_ADDR_SEQ, buf, FM_READ_LEN)) {
            Serial.println("    0x10 ACKed its address but would not clock out data.");
            return 0;
        }

        id = ((unsigned)buf[8] << 8) | buf[9];

        Serial.print("    read ");
        for (i = 0; i < FM_READ_LEN; i++) {
            Serial.print(" 0x");
            Serial.print(buf[i], HEX);
        }
        Serial.println();

        if (id == FM_CHIP_ID)
            break;

        if (wrote) {
            Serial.print("    id reads 0x");
            Serial.print(id, HEX);
            Serial.println(", not the 0x5808 the firmware checks for.");
            Serial.println("    Bytes came back, so the bus clocks - but they are not the");
            Serial.println("    expected ones, which points at the byte order or at this");
            Serial.println("    part not being the one in the dump.");
            return 0;
        }

        Serial.println("    not 0x5808 yet; applying the vendor's own enable write");
        Serial.println("    ({0x00, 0x02} to 0x10, from 0x00D3D952) and re-reading.");
        if (!i2cWriteTo(FM_ADDR_SEQ, enable, sizeof(enable))) {
            Serial.println("    the write was not ACKed.");
            return 0;
        }
        wrote = 1;
        delay(50);            /* [V] the driver's own wait */
    }

    Serial.print("    chip id 0x");
    Serial.print(id, HEX);
    Serial.println(" - the RDA5807 the firmware expects.");
    Serial.println();
    Serial.println("  THAT SETTLES THE BUS. A device on these two pads answered with");
    Serial.println("  the exact value its driver checks for, so the pads, the SDA/SCL");
    Serial.println("  assignment, the timing, the ACKs and the byte order are all");
    Serial.println("  right. Anything silent from here on is silent for its own");
    Serial.println("  reasons - starting with 0x68.");
    return 1;
}

/*
 * Scan the bus and report every address that answers. Returns 1 if 0x68 was
 * among them.
 */
static int scanBus(void)
{
    unsigned addr, hits = 0;
    int found_sensor = 0;

    busRecover();

    Serial.print("  scan:");

    for (addr = SCAN_FIRST; addr <= SCAN_LAST; addr++) {
        if (!i2cPing((uint8_t)addr)) {
            if (bus_error == BUS_SCL_STUCK) {
                Serial.println();
                Serial.println("    SCL never rose. Nothing was clocked, so this pass says");
                Serial.println("    nothing about any device - the clock line is held low,");
                Serial.println("    or that pad is not SCL, or it does not reach the pin.");
                return 0;
            }
            continue;
        }

        hits++;
        if (addr == CAM_ADDR)
            found_sensor = 1;
        if (addr == FM_ADDR_SEQ)
            saw_fm = true;
        if (hits <= SCAN_TOO_MANY) {
            Serial.print(" 0x");
            Serial.print(addr, HEX);
            /* Name what is known, so the line reads as a finding rather than
             * as three numbers to go and look up. */
            if (addr == FM_ADDR_SEQ || addr == FM_ADDR_RANDOM ||
                addr == FM_ADDR_TEA)
                Serial.print("(FM)");
            else if (addr == CAM_ADDR)
                Serial.print("(camera)");
        }
    }

    if (hits > SCAN_TOO_MANY) {
        Serial.println();
        Serial.print("    everything ACKs (");
        Serial.print(hits);
        Serial.println(" addresses). That is SDA reading low no matter what");
        Serial.println("    we do, not a populated bus - a stuck line answers for");
        Serial.println("    every device at once.");
        return 0;
    }

    if (hits == 0) {
        Serial.println(" nobody answered.");
        return 0;
    }

    Serial.println();
    return found_sensor;
}

/* ------------------------------------------------------------------------ */

void setup()
{
    Serial.begin(115200);
    Serial.println();
    Serial.println("=== SL6806 camera probe ===");
    Serial.println("sc101, 1 MP, TWI 0 addr 0x68 - notes 7h.");
    Serial.println("I2C is bit-banged on the two TWI0 pads, so this needs no TWI");
    Serial.println("controller. It measures the two lines first, then scans the");
    Serial.println("bus under every combination of the three things notes 7h");
    Serial.println("assumes rather than reads: which pad is SDA, where MCLK comes");
    Serial.println("from, and which of the two power lines is RESET.");
    Serial.println("This finds the sensor; it does not capture an image.");
    Serial.println();

    /* The pad controller is only wired into digitalRead()/digitalWrite() when
     * something installs it. The display bring-up does; this sketch has no
     * display, so it does it itself. Everything here goes through the pad API
     * by id - see the note on CAM_PAD_RESET. */
    sl6806_gpio_vendor_register(sl6806_padctl_vendor());
    mclkFastInit();

    if (!sl6806_gpio_available())
        Serial.println("GPIO reports unavailable - nothing below will mean anything.");

    /* Every delay in the power-up is paced with millis(), which needs a
     * counter that advances. If neither does, the sketch would sit in its
     * first 5 ms wait forever and look like a loop() that never ticks - say
     * which it is instead. */
    if (!sl6806_tick_mask()) {
        Serial.println("No timekeeping counter is running, so the power-up delays");
        Serial.println("below will never expire. Nothing after this line will print.");
    }

    attempt   = 0;
    pad_sda   = attempts[0].sda;
    pad_scl   = attempts[0].scl;
    pad_reset = attempts[0].reset;
    pad_pwdn  = attempts[0].pwdn;
    mclk_mode = attempts[0].mclk;

    step_at = millis();
    heartbeat_at = step_at;
}

void loop()
{
    uint32_t now;

    /* Keep MCLK alive across whatever wait we are in. In mux mode this costs
     * nothing; in toggle mode it is the only reason the sensor sees a clock
     * outside a transfer. */
    if (mclk_mode == MCLK_TOGGLE)
        mclkSpin(MCLK_BURST_US);

    switch (step) {

    case STEP_ANNOUNCE:
        /* Before any delay, so that a loop() which never runs and a loop()
         * which wedges in its first wait do not look identical on the
         * monitor - they have opposite fixes. */
        Serial.println("loop() is running.");
        Serial.println();
        Serial.println("the two bus lines, before anything is transferred.");
        Serial.println("pull selector -> level, as a plain input:");
        step = STEP_PULL_SWEEP;
        break;

    case STEP_PULL_SWEEP: {
        unsigned a = sweepPulls(CAM_PAD_A, "pin 13");
        unsigned b = sweepPulls(CAM_PAD_B, "pin 12");

        /*
         * Prefer the vendor's own selector if it works, since that is what
         * the firmware uses on these pads; otherwise take any selector that
         * holds both lines high, because without a released-high state I2C
         * has no signalling at all.
         */
        if (a == 0 || b == 0) {
            Serial.println();
            Serial.println("  at least one line never reads high under any pull.");
            Serial.println("  That alone explains a silent bus: with no released-high");
            Serial.println("  state nothing can signal, and no ACK can ever be seen.");
            Serial.println("  Either something external holds the line down, or these");
            Serial.println("  pads are not the bus, or bank 4's pulls are not pull-ups.");
            Serial.println("  The scans below still run, and should be read in that");
            Serial.println("  light rather than as evidence about the sensor.");
            lines_idle_high = false;
            pull_sel = PULL_VENDOR;
        } else {
            lines_idle_high = true;
            pull_sel = (a == PULL_VENDOR && b == PULL_VENDOR) ? PULL_VENDOR
                                                              : (a > b ? a : b);
            Serial.println();
            Serial.print("  both lines idle high; using pull selector ");
            Serial.print(pull_sel);
            Serial.println(pull_sel == PULL_VENDOR ? " (the vendor's)" : "");
        }

        Serial.println();
        step = STEP_DRIVE_TEST;
        break;
    }

    case STEP_DRIVE_TEST:
        Serial.println("can software move them?");
        driveTest(CAM_PAD_A, "SDA/SCL pin 13");
        driveTest(CAM_PAD_B, "SDA/SCL pin 12");
        /*
         * The control pads deserve the same test and never got it. Everything
         * this sketch concludes about the sensor assumes RESET and PWDN
         * actually move: a sensor held in reset by a pad that does nothing is
         * silent for a reason that has no more to do with MCLK than with the
         * bus. The bus pads were verified from the first version; these were
         * taken on faith.
         *
         * What it can and cannot show: this reads the pad's own input
         * register, so it proves the pad drives, not that the trace reaches a
         * module. That is still the half that is testable from here.
         */
        driveTest(CAM_PAD_RESET, "RESET pin 14");
        driveTest(CAM_PAD_PWDN, "PWDN pin 15");

        /*
         * And the half that decides whether the sensor can ever leave reset.
         * The vendor configures both control pads with drive 0; if that
         * cannot hold the line high against whatever is pulling it down,
         * drive 3 is a one-field change and worth knowing about.
         */
        Serial.println();
        Serial.println("and can they be driven high?");
        if (driveHighTest(CAM_PAD_RESET, "RESET pin 14", 0)) {
            ctrl_drive = 0;
            Serial.println("    the vendor's drive 0 reaches the pin.");
        } else if (driveHighTest(CAM_PAD_RESET, "RESET pin 14", 3)) {
            ctrl_drive = 3;
            Serial.println("    drive 0 does not reach the pin but drive 3 does. Using 3");
            Serial.println("    below - the vendor's own value would leave the sensor in");
            Serial.println("    reset, which would explain everything so far.");
        } else {
            ctrl_drive = 3;
            reset_stuck = true;
            Serial.println("    NEITHER DRIVE STRENGTH CAN PULL THIS LINE HIGH.");
            Serial.println("    A part wired to it is held in reset permanently, and that");
            Serial.println("    is a complete explanation for a silent 0x68 - no clock or");
            Serial.println("    bus question needed. Something drives this net low, or it");
            Serial.println("    is shorted to ground, or nothing is on the other end.");
        }
        (void)driveHighTest(CAM_PAD_PWDN, "PWDN pin 15", ctrl_drive);

        /*
         * And the question those two answers raise between them: if RESET
         * keeps whichever level it was last given, is anything out there at
         * all? The bus pads are the control - they are known to have external
         * pull-ups, so their lows must decay.
         */
        Serial.println();
        Serial.println("what is on these nets? (charge held with nothing driving)");
        (void)holdTest(CAM_PAD_B, "SDA/SCL pin 12");
        (void)holdTest(CAM_PAD_A, "SDA/SCL pin 13");
        reset_floats = holdTest(CAM_PAD_RESET, "RESET pin 14");
        pwdn_floats  = holdTest(CAM_PAD_PWDN, "PWDN pin 15");

        printAttempt();
        step = STEP_POWER_LOW;
        break;

    case STEP_POWER_LOW:
        /*
         * [V] The vendor's power-up, 0x00D44B1C: mux MCLK, then PWDN low and
         * RESET low, 5 ms, RESET high, 5 ms, PWDN high, 20 ms, chip id. The
         * delays are paced with millis() rather than delay() so poll mode has
         * nothing to clamp: no single loop() call blocks.
         */
        sl6806_pad_configure(mclk_mode == MCLK_TOGGLE ? CAM_MCLK_GPIO
                                                      : CAM_MCLK_MUX);
        /* The vendor's ids carry drive 0; ctrl_drive is what the driven-high
         * test above found actually reaches the pin. */
        sl6806_pad_configure(PAD_WHICH(pad_reset) |
                             (SL6806_PAD_FUNC_OUTPUT << 7) |
                             ((ctrl_drive & 3u) << 4));
        sl6806_pad_configure(PAD_WHICH(pad_pwdn) |
                             (SL6806_PAD_FUNC_OUTPUT << 7) |
                             ((ctrl_drive & 3u) << 4));
        busInit();

        sl6806_pad_write(pad_pwdn, 0);
        sl6806_pad_write(pad_reset, 0);
        step = STEP_RESET_HIGH;
        step_at = millis();
        break;

    case STEP_RESET_HIGH:
        if (millis() - step_at < 5)
            break;
        sl6806_pad_write(pad_reset, 1);
        step = STEP_PWDN_HIGH;
        step_at = millis();
        break;

    case STEP_PWDN_HIGH:
        if (millis() - step_at < 5)
            break;
        sl6806_pad_write(pad_pwdn, 1);
        step = STEP_SETTLE;
        step_at = millis();
        break;

    case STEP_SETTLE:
        if (millis() - step_at < 20)
            break;

        /*
         * The vendor's 20 ms is up. Before asking anything, give a
         * software-clocked sensor a settled run of MCLK and say how fast it
         * actually is - the whole "is it MCLK?" question turns on a number
         * that was never measured, only assumed to be "slow".
         */
        if (mclk_mode == MCLK_TOGGLE) {
            uint32_t khz = mclkMeasure();

            mclk_khz = khz;
            Serial.print("  MCLK measured at ");
            if (khz >= 1000) {
                Serial.print(khz / 1000);
                Serial.print(".");
                Serial.print((khz % 1000) / 100);
                Serial.print(" MHz");
            } else {
                Serial.print(khz);
                Serial.print(" kHz");
            }
            Serial.print(", settling it for ");
            Serial.print(MCLK_SETTLE_MS);
            Serial.println(" ms");

            /* All of it inside this one loop() call. That is deliberate - the
             * point is a clock with no gaps in it - and 30 ms is well inside
             * what poll mode can spend in the ROM's USB handler. */
            {
                uint32_t spun;
                for (spun = 0; spun < MCLK_SETTLE_MS; spun++)
                    mclkSpin(1000);
            }
        }

        step = STEP_SCAN;
        break;

    case STEP_SCAN:
        if (scanBus() && reportChipId()) {
            Serial.println();
            samples   = 0;
            sample_at = millis();
            step      = STEP_TALK;
            break;
        }

        /* The camera did not answer, but if the tuner did then this sketch
         * can be checked against a device whose right answer is known. Once
         * is enough - it is a property of the wiring, not of the attempt. */
        if (saw_fm && !bus_proved)
            bus_proved = fmSelfTest();

        if (++attempt < NATTEMPTS) {
            pad_sda   = attempts[attempt].sda;
            pad_scl   = attempts[attempt].scl;
            pad_reset = attempts[attempt].reset;
            pad_pwdn  = attempts[attempt].pwdn;
            mclk_mode = attempts[attempt].mclk;
            printAttempt();
            step = STEP_POWER_LOW;
            break;
        }

        Serial.println();
        Serial.print("nothing answered under any of the ");
        Serial.print(NATTEMPTS);
        Serial.println(" combinations.");
        Serial.println();

        if (bus_proved) {
            /* The strongest case: another device on these exact pads gave the
             * value its own driver checks for. Nothing about the bus is left
             * to suspect, so say so plainly and point at what is. */
            Serial.println("But the bus is proven: the FM tuner on these same two pads");
            Serial.println("answered with the id its driver expects. So the pads, the");
            Serial.println("SDA/SCL assignment, the pull-ups and every line of the I2C");
            Serial.println("above are right, and 0x68 is simply not answering.");
            Serial.println();

            /*
             * MCLK used to head this list. It does not any more, and the
             * reason is the measurement above rather than an argument.
             */
            if (mclk_khz >= 2500) {
                Serial.print("MCLK is no longer a good explanation either: it measured ");
                Serial.print(mclk_khz / 1000);
                Serial.print(".");
                Serial.print((mclk_khz % 1000) / 100);
                Serial.println(" MHz here, and the vendor programs clock channel 6");
                Serial.println("with 2800 - which as kHz is 2.8 MHz, the same clock. It ran");
                Serial.println("for 30 ms before the first transfer and through every bit");
                Serial.println("of it. A sensor given its own nominal clock on a proven bus");
                Serial.println("and still not answering is not a clock problem.");
            } else {
                Serial.print("MCLK only measured ");
                Serial.print(mclk_khz);
                Serial.println(" kHz, which is low enough to stay a suspect.");
            }

            Serial.println();

            if (reset_stuck) {
                Serial.println("AND RESET COULD NOT BE DRIVEN HIGH AT ALL. That outranks");
                Serial.println("everything below it: a sensor whose reset line is held low");
                Serial.println("is in reset, and a part in reset does not answer its");
                Serial.println("address. Whatever else is true, this alone accounts for");
                Serial.println("every silent scan above.");
                Serial.println();
            }

            if (reset_floats && pwdn_floats) {
                Serial.println("AND BOTH CONTROL NETS FLOAT. Driven either way they keep");
                Serial.println("the level, for longer than any resistor would allow, while");
                Serial.println("the bus pads next to them behave exactly as pulled-up lines");
                Serial.println("should. A camera module would put something on reset and");
                Serial.println("power-down - a pull, an input, a load. There is nothing.");
                Serial.println();
                Serial.println("The most economical reading of every result in this run is");
                Serial.println("that THIS UNIT HAS NO CAMERA FITTED: the SoC pads are real");
                Serial.println("and work, the firmware's driver is real, and the module the");
                Serial.println("firmware was written for is not on the end of the traces.");
                Serial.println("Look at the case for a lens - that is the confirmation, and");
                Serial.println("if it is absent this line of investigation is finished.");
            } else {
                Serial.println("What is left:");
                Serial.println("  1. Sensor power. RESET and PWDN are pads and they drive,");
                Serial.println("     but a module rail behind a regulator nothing here");
                Serial.println("     drives is not - and the vendor's power-up has no such");
                Serial.println("     call in it either, so that would be on the module.");
                Serial.println("  2. No camera fitted in this unit. Cheap players ship one");
                Serial.println("     firmware image across several hardware builds. Look at");
                Serial.println("     the case for a lens.");
                Serial.println();
                Serial.println("Neither is on this bus, and neither is fixable in software.");
            }
        } else if (saw_fm) {
            Serial.println("Something did answer the scan, so the bus is not dead - but");
            Serial.println("the tuner check above did not confirm it. Read that first:");
            Serial.println("until a known device gives a known answer, a silent 0x68 is");
            Serial.println("not yet evidence about the camera.");
        } else if (!lines_idle_high) {
            Serial.println("Read the pull sweep at the top first: a line that never");
            Serial.println("reads high is a complete explanation on its own, and no");
            Serial.println("scan below it can say anything about the sensor.");
        } else {
            Serial.println("The lines are electrically sane - they idle high and they");
            Serial.println("move - and nothing at all answered. On the unit this was");
            Serial.println("written against, one assignment found three devices, so an");
            Serial.println("entirely empty bus here is itself the finding: suspect the");
            Serial.println("pads and the board before the sensor.");
        }
        step = STEP_STUCK;
        break;

    case STEP_TALK:
        /* Keep reading, both as proof of life and because a sensor that
         * answers once and then stops is exactly what a clock that only runs
         * during transfers would look like. */
        now = millis();
        if (now - sample_at >= SAMPLE_MS) {
            uint8_t hi = 0, lo = 0;

            sample_at = now;
            samples++;

            if (camRead(CAM_REG_ID_HI, &hi, 1) &&
                camRead(CAM_REG_ID_LO, &lo, 1)) {
                Serial.print("[");
                Serial.print(now / 1000);
                Serial.print("s] read ");
                Serial.print(samples);
                Serial.print(": id 0x");
                Serial.print(hi, HEX);
                Serial.println(lo, HEX);
            } else {
                Serial.print("[");
                Serial.print(now / 1000);
                Serial.println("s] the sensor stopped answering.");
                Serial.println("  It answered before, so the pads and the address are");
                Serial.println("  right and something is going away - MCLK is the");
                Serial.println("  first suspect.");
            }
        }
        break;

    case STEP_STUCK:
        now = millis();
        if (now - heartbeat_at >= HEARTBEAT_MS) {
            heartbeat_at = now;
            Serial.print("[");
            Serial.print(now / 1000);
            Serial.println("s] idle. Unplug when you have read the above.");
        }
        break;
    }
}
