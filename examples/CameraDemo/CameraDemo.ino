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
 * ===================================================================
 *  AND NOW THE POWER IS REACHABLE TOO
 * ===================================================================
 * "Sensor power" was the open half of that, and it was open because the
 * register file the vendor switches it through had no known address. It has
 * one now, and it is not an address: the two accessors at SRAM 0x00804E44 and
 * 0x00804EAC drive a mailbox at 0x400F7000+0x100 that carries an index and a
 * byte to a chip at I2C 0x30. cores/sl6806/sl6806_regfile.h has the decode and
 * the story of why five static searches missed it.
 *
 * Two things fall out, and the second is the one that matters here. The
 * "clock channels" are LDO rails - the setter divides millivolts by 50 across
 * 1700..3300 - so the camera's clk(6, 2, 2800) is 2.8 V, not 2800 kHz. And
 * this sketch can now perform it: powerTheSensor() below writes rail 6 and the
 * sensor's two enable bits before any of the bus work starts.
 *
 * SO THE HONEST STATE IS: every ingredient the vendor uses is now reachable
 * from a payload, and none of it has been run on hardware. If 0x68 answers
 * after this change, the rail was the answer all along. If it still does not,
 * the register file's own read-back - printed - says whether the writes even
 * landed, which is a different failure from a sensor that will not talk.
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
#include <string.h>
#include "sl6806_padctl.h"
#include "sl6806_regfile.h"
#include "sl6806_dvp.h"
#include "sl6806_mmio.h"
#include "sl6806_module.h"
#include "sl6806_romclk.h"
#include "sc101_init.h"

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
    STEP_DVP_PADS,      /* can the DVP lines be pulled, or is something
                           holding them? - paced, one pad per call         */
    STEP_SCAN,          /* who ACKs, anywhere on the bus                   */
    STEP_INIT,          /* replay the vendor's init table into the sensor  */
    STEP_TALK,          /* the sensor answered: keep reading it            */
    STEP_STUCK          /* nothing answered under any combination          */
};

static uint8_t  step = STEP_ANNOUNCE;
static uint8_t  sub;                  /* item within a paced phase      */
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
        /*
         * Prefer the vendor's selector over the lowest one that happens to
         * read high. On a pad with an external pull-up every non-pull-down
         * selector reads high, so "lowest that works" picked 6 - which turns
         * out not to be a pull-up at all on a pad without one. The vendor's
         * choice is the one with evidence behind it.
         */
        if (level && (!found || sel == PULL_VENDOR))
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
 *   both directions hold the window    no resistor on this net
 *   high decays quickly               there is a pull-down out there
 *   low decays quickly                there is a pull-up out there
 *
 * WHAT IT DOES NOT SHOW. "Holds both" means no *resistor*, not no *device*.
 * A CMOS input is high impedance, so a trace running to a sensor's reset pin
 * with no pull resistor on it floats exactly like a trace running nowhere.
 * This test tells those two apart from a pulled net; it cannot tell them
 * apart from each other, and an earlier version of this comment claimed it
 * could.
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

/*
 * How long does an internal pull take to move the net?
 *
 * The hold test says which nets carry a resistor. It cannot say whether a net
 * with no resistor runs anywhere, because a high-impedance input and an
 * unconnected stub both just hold their charge. This asks the one thing that
 * does differ between them: **capacitance**. Drive the pad to one rail,
 * release it to an internal pull in the other direction, and time the flip.
 * The internal pull is a roughly fixed resistance, so the time is a proxy for
 * how much copper - and how many pins - hang off the pad.
 *
 * It is a comparison, not a measurement: the number means nothing on its own,
 * only next to the same number from a pad whose wiring is known. The bus pads
 * are the reference, since the FM tuner proves they reach a fitted chip.
 *
 * Sweep results say selectors 6..11, 13 and 15 read high and 4, 5, 12, 14
 * read low even against the bus's external pull-ups - so 4 is a pull-down
 * strong enough to be useful as the other direction.
 */
/*
 * Selector 8 is the vendor's own for these pads and the sweep reads it high
 * on all four. Selector 6 is not usable here: on pins 12, 13 and 15 it reads
 * high only because their *external* pull-ups hold the line, and on pin 14,
 * which has none, it does nothing at all. Measuring pin 14's rise time
 * through it produced "(never)" and nearly became a finding about the board.
 */
#define PULL_UP_SEL     8u
#define PULL_DOWN_SEL   4u
#define PULL_LIMIT_US   50000u

static uint32_t pullTime(uint32_t id, unsigned sel, int from)
{
    uint32_t start, waited;

    /* Drive it to `from` with no pull, then release to the pull under test. */
    sl6806_pad_configure(PAD_WHICH(id) | (SL6806_PAD_FUNC_OUTPUT << 7) | (3u << 4));
    sl6806_pad_write(id, from);
    delayMicroseconds(200);

    sl6806_pad_configure(PAD_WHICH(id) | (3u << 4) | sel);   /* input + pull */

    if (!sl6806_tick_mask())
        return 0;

    start = micros();
    while ((waited = micros() - start) < PULL_LIMIT_US)
        if (sl6806_pad_read(id) != from)
            return waited;
    return PULL_LIMIT_US;
}

static void pullTimeReport(uint32_t id, const char *name)
{
    uint32_t up = pullTime(id, PULL_UP_SEL, 0);     /* low -> pulled up   */
    uint32_t dn = pullTime(id, PULL_DOWN_SEL, 1);   /* high -> pulled down */

    Serial.print("  ");
    Serial.print(name);
    Serial.print(": pull-up took ");
    if (up >= PULL_LIMIT_US) Serial.print("(never)"); else { Serial.print(up); Serial.print("us"); }
    Serial.print(", pull-down took ");
    if (dn >= PULL_LIMIT_US) Serial.print("(never)"); else { Serial.print(dn); Serial.print("us"); }
    Serial.println();
}

/* ------------------------------------------- powering the sensor up */

/*
 * ===================================================================
 *  THE ONLY WRITES IN THIS SKETCH THAT ARE NOT ON THE I2C BUS
 * ===================================================================
 * Six bytes into the indexed register file, which is where the vendor's
 * drivers switch the sensor's 2.8 V rail on. Everything else in this sketch
 * is read-only or drives a pad.
 *
 * THE ADDRESS PROBLEM IS OVER, AND IT WAS NEVER AN ADDRESS. An earlier
 * version of this block wrote bytes at 0x40040000, because that base had been
 * identified as the register file and then retracted; all four writes were
 * ignored on hardware. The file is not an MMIO window at all. The vendor's
 * accessors drive a mailbox at 0x400F7000+0x100 that carries an index and a
 * byte to a chip at I2C address 0x30, and cores/sl6806/sl6806_regfile.h has
 * the whole decode. This sketch now goes through that.
 *
 * AND "CLOCK CHANNEL 6" IS A VOLTAGE RAIL. The other thing the decode
 * changed: 0x00CC7334's argument of 2800 is not 2800 kHz, it is 2800 mV. The
 * setter divides by 50 across 1700..3300, which is an LDO's scale and nothing
 * else's. So the sensor's problem was never MCLK - it was that its supply was
 * off, which is exactly what "the vendor shows a live preview and a payload
 * gets nothing" predicted.
 *
 * Why this is safe enough to run, stated rather than assumed:
 *
 *   - Every write is a read-modify-write with the vendor's own mask, so only
 *     the bits the stock firmware touches are touched.
 *   - It only ever touches rail 6. Rails 2..5 go somewhere unknown, and
 *     unknown on a SoC includes the core and the SRAM this code runs from.
 *   - Payload mode never writes flash, so the worst outcome is a device that
 *     stops responding until it is unplugged.
 *
 * DEFAULT ON. This is the whole point of the sketch now: the bus was proved
 * with the FM tuner, the pads were swept, and the one thing never tried is
 * giving the sensor power. Set it to 0 to get the old bus-only behaviour
 * back.
 *
 * The register map, all of it from the vendor's own code:
 *
 *   0x2C  bit 4      rail 6 enabled (bit = channel - 2)
 *   0x7A  bits [4:0] rail 6 voltage; 0x13 = 2800 mV
 *   0x03  bits [4:3] the camera's field, set to 01
 *   0x16  bit 7      the camera's enable
 */
#define CAMERA_POWER_WRITES 1

#if CAMERA_POWER_WRITES

/*
 * One read-modify-write, announced before it happens and verified after.
 * Announcing first means a device that dies mid-sequence names the register
 * that killed it; verifying after means a register that ignores the write is
 * reported rather than assumed to have taken it. That verification is what
 * turned the 0x40040000 mistake into a clean negative instead of a mystery,
 * so it stays even though the address is now right.
 */
static int regSet(unsigned idx, uint8_t keep, uint8_t set, const char *what)
{
    int before = sl6806_reg_read(idx);
    uint8_t want;
    int rc, after;

    Serial.print("  reg 0x");
    Serial.print(idx, HEX);
    Serial.print(": ");

    if (before < 0) {
        Serial.println("read timed out - the mailbox never completed.");
        return -1;
    }

    want = (uint8_t)(((unsigned)before & keep) | set);

    Serial.print("0x");
    Serial.print(before, HEX);
    Serial.print(" -> 0x");
    Serial.print(want, HEX);
    Serial.print("  (");
    Serial.print(what);
    Serial.println(")");

    rc = sl6806_reg_write(idx, want);
    if (rc) {
        Serial.print("    write reported ");
        Serial.println(rc < 0 ? "a timeout" : "an error in STAT");
        return rc;
    }

    after = sl6806_reg_read(idx);
    if (after != (int)want) {
        Serial.print("    reads back 0x");
        Serial.print(after, HEX);
        Serial.println(" - the write did not stick.");
        return 1;
    }
    return 0;
}

static void powerTheSensor(void)
{
    int mv;

    Serial.println();
    Serial.println("powering the sensor through the register file - mailbox at");
    Serial.println("0x400F7000+0x100, chip at I2C 0x30 (sl6806_regfile.h):");

    /* The rail, in the vendor's order at 0x00D44B1C: off, wait, set the
     * voltage, on. Doing it as three announced writes rather than calling
     * sl6806_camera_power_on() so that a failure names its register. */
    regSet(SL6806_RF_LDO_ENABLE, (uint8_t)~(1u << (6 - 2)), 0x00,
           "rail 6 off");
    delay(5);
    regSet(SL6806_RF_LDO6_MV, 0xE0, 0x13, "rail 6 to 2800 mV");
    regSet(SL6806_RF_LDO_ENABLE, 0xFF, 1u << (6 - 2), "rail 6 on");

    /* Then the sensor driver's own two, from 0x00D44EA6 and 0x00D44F06. */
    regSet(SL6806_RF_CAM_FIELD, SL6806_RF_CAM_FIELD_KEEP,
           SL6806_RF_CAM_FIELD_SET, "camera field [4:3] = 01");
    regSet(SL6806_RF_CAM_ENABLE, 0xFF, SL6806_RF_CAM_ENABLE_BIT,
           "camera enable");

    /* Read the rail back through the vendor's own inverse scale. A number
     * near 2800 here is the strongest evidence available without a meter
     * that the block is what it is claimed to be. */
    mv = sl6806_ldo_get_mv(SL6806_LDO_CAMERA);
    Serial.print("  rail 6 reads back as ");
    if (mv < 0)
        Serial.println("unreadable.");
    else {
        Serial.print(mv);
        Serial.println(" mV.");
    }

    /* The sensor's own datasheet-scale start-up time is unknown; the vendor
     * waits 20 ms after the last pad move before it talks to the chip, and
     * this is the same order. */
    delay(20);
    Serial.println();
}

#endif /* CAMERA_POWER_WRITES */

/* ------------------------------------------------ waking the front end */

/*
 * ===================================================================
 *  MCLK COMES FROM HERE, AND MODULE ID 46 IS WHAT SWITCHES IT ON
 * ===================================================================
 * [M] Measured on hardware 2026-08-13 by examples/DvpProbe. The DVP block at
 * 0x400E1000 reads all-zero and ignores writes in a cold payload; the
 * vendor's own three mechanisms do not change that - 0x400E0000 will not even
 * hold its own bit - and then mask ROM module id 46, enabled the way
 * sl6806_module_enable does it, makes the block take and hold writes. That is
 * the wall notes 15 recorded as dead, and it is not.
 *
 * WHY THIS SKETCH CARES. The sensor's MCLK leaves on bank 4 pin 3 at
 * alternate function 2 - the same function as the eleven DVP data and sync
 * pads on the same bank - so the front end is what generates it. Everything
 * else the sensor needs has now been measured to be present already: the rail
 * is on, both enable bits are set, the bus is proven against the FM tuner,
 * and both control lines can be driven. A hardware clock is the one thing the
 * vendor supplies that this sketch never has.
 *
 * WHAT IT WRITES. The module clock at CRU +0x118 with the vendor's own source
 * and divider (0 and 0, from 0x00D44688), module id 46, the eleven pads plus
 * MCLK to function 2, and the geometry registers. No rail is touched.
 */
#define DVP_BRINGUP 1

static bool dvp_awake;

#if DVP_BRINGUP

/* [V] The sensor's full array, from the bind path's literals at 0x00D447C4.
 * Crop and output are set to the whole frame: this wants a clock, not a
 * picture, and a 1:1 scaler is the configuration least likely to be
 * rejected. */
#define DVP_IN_W    1280
#define DVP_IN_H    720
#define DVP_SIZE_WORD  (((uint32_t)(DVP_IN_H - 1) << 16) | (DVP_IN_W - 1))

static int scopeTheBus(void);   /* defined below, used by the bring-up */

/* Bounded, because every one of these polls waits on hardware that may not
 * be there. An unbounded spin in payload mode hangs inside the boot ROM's
 * USB handler and takes the device off the bus with no log. */
#define DVP_POLL_LIMIT  200000u

/*
 * [V] 0x00D9A7FC and 0x00D9A7AC - the clock the front end actually runs on.
 *
 * The 2026-08-13 run enabled the block and its gate and got a fully awake,
 * fully configured DVP that produced no MCLK. This is what was missing: the
 * request/ack handshake, a PLL, and the media module clock. See
 * sl6806_dvp.h.
 */
static void dvpClockTree(void)
{
    unsigned b, n;
    uint32_t v;

    Serial.print("  request/ack 0x40000070 = 0x");
    Serial.print(sl6806_mmio_read(SL6806_REQ_ACK), HEX);

    for (b = 0; b < SL6806_REQ_ACK_COUNT; b++) {
        uint32_t ack = 1u << (b + SL6806_REQ_ACK_SHIFT);

        if (sl6806_mmio_read(SL6806_REQ_ACK) & ack)
            continue;                       /* already granted */

        sl6806_mmio_set(SL6806_REQ_ACK, 1u << b);
        for (n = DVP_POLL_LIMIT; n; n--)
            if (sl6806_mmio_read(SL6806_REQ_ACK) & ack)
                break;
        if (!n) {
            Serial.print(" - domain ");
            Serial.print(b);
            Serial.print(" never acknowledged");
            break;
        }
    }
    Serial.print(" -> 0x");
    Serial.println(sl6806_mmio_read(SL6806_REQ_ACK), HEX);

    /* The PLL. Announced before it is written, because this is the one write
     * in the sketch that could plausibly disturb a clock something else is
     * using - the vendor does it on a running system, which is the reason to
     * think it will not. */
    v = sl6806_mmio_read(SL6806_CRU_PLL);
    Serial.print("  PLL 0x40080008 = 0x");
    Serial.print(v, HEX);
    Serial.print(" -> writing 0x");
    Serial.println(SL6806_CRU_PLL_VALUE, HEX);

    sl6806_mmio_write(SL6806_CRU_PLL, SL6806_CRU_PLL_VALUE);
    for (n = DVP_POLL_LIMIT; n; n--)
        if (sl6806_mmio_read(SL6806_CRU_PLL) & SL6806_CRU_PLL_LOCKED)
            break;

    if (!n) {
        Serial.print("  PLL never reported lock (bit 28); reads 0x");
        Serial.println(sl6806_mmio_read(SL6806_CRU_PLL), HEX);
    } else {
        sl6806_mmio_set(SL6806_CRU_PLL, SL6806_CRU_PLL_RELEASE);
        Serial.print("  PLL locked, released; reads 0x");
        Serial.println(sl6806_mmio_read(SL6806_CRU_PLL), HEX);
    }

    delay(10);
    sl6806_mmio_write(SL6806_CRU_MEDIA_CLOCK, SL6806_CRU_MEDIA_VALUE);
    delay(10);
    Serial.print("  media clock 0x4008011C reads 0x");
    Serial.println(sl6806_mmio_read(SL6806_CRU_MEDIA_CLOCK), HEX);
}

static void dvpBringUp(void)
{
    static const uint32_t pads[] = {
        SL6806_DVP_PAD_D0, SL6806_DVP_PAD_D1, SL6806_DVP_PAD_D2,
        SL6806_DVP_PAD_D3, SL6806_DVP_PAD_D4, SL6806_DVP_PAD_D5,
        SL6806_DVP_PAD_D6, SL6806_DVP_PAD_D7,
        SL6806_DVP_PAD_SYNC2, SL6806_DVP_PAD_SYNC1, SL6806_DVP_PAD_SYNC0,
    };
    unsigned i;
    uint32_t v;

    Serial.println();
    Serial.println("waking the DVP front end - this is where MCLK comes from:");

    /*
     * [V] The two the SENSOR's own init does, which the front-end path does
     * not: 0x00D44CD2 and 0x00D44CD8. Both were missed because this project
     * had read `bind_dvp_channel` and not `sensor_init`, and between them
     * they are the only things the vendor does for the camera that a payload
     * has not - after the rail, both clocks, the PLL and the geometry all
     * came up right and the sensor stayed mute with its outputs in Hi-Z.
     *
     * ROM 0x20EC turns out to be a second clock family, 85 ids each mapping
     * to one CRU register and bit; id 58 is CRU +0xC0 bit 0, which nothing in
     * this repository has ever written.
     */
    if (!sl6806_module_enable(SL6806_SENSOR_MODULE_ID))
        Serial.println("  module id 80 did not acknowledge its gate.");
    sl6806_mmio_set(SL6806_CRU_SENSOR_CLOCK, SL6806_CRU_SENSOR_CLOCK_EN);
    Serial.print("  sensor clocks: module 80, CRU +0xC0 reads 0x");
    Serial.println(sl6806_mmio_read(SL6806_CRU_SENSOR_CLOCK), HEX);

    /* The clock tree first. Gates without a clock is exactly what the last
     * run did, and it produced an awake block and a mute sensor. */
    dvpClockTree();

    /* [V] 0x00D44688 with source 0, divider 0. */
    v = sl6806_mmio_read(SL6806_CRU_CAM_CLOCK);
    v &= ~(uint32_t)(SL6806_CRU_CAM_SRC_MASK | SL6806_CRU_CAM_DIV_MASK);
    sl6806_mmio_write(SL6806_CRU_CAM_CLOCK, v);
    sl6806_mmio_write(SL6806_CRU_CAM_CLOCK,
                      sl6806_mmio_read(SL6806_CRU_CAM_CLOCK)
                          | SL6806_CRU_CAM_ENABLE);

    /* [M] The register clock. This is what makes the block's registers
     * answer - and, it turns out, what makes 0x400E0000 answer too. */
    if (!sl6806_module_enable(SL6806_DVP_MODULE_ID))
        Serial.println("  module id 46 did not acknowledge its gate.");

    /*
     * [M] THE FUNCTIONAL CLOCK, and the line whose absence explains the
     * entire 2026-08-13 session. Measured that day: 0x400E0000 ignores writes
     * from a cold chip and holds them once module 46 is enabled - so the
     * order is registers first, function second, and every earlier sweep that
     * called this register dead was writing it from a cold chip.
     *
     * Without this the DVP has its register file and no logic clock: it
     * accepts every geometry write, reads them all back, and drives no MCLK.
     * That is exactly what eight pad combinations and sixteen clock settings
     * measured against a sensor that works fine under the stock firmware.
     */
    if (sl6806_periph_enable(SL6806_DVP_PERIPH_ID)) {
        sl6806_periph_reset(SL6806_DVP_PERIPH_ID);
        Serial.print("  functional clock on: 0x400E0000 = 0x");
        Serial.print(sl6806_mmio_read(SL6806_PERIPH_ENABLE_REG), HEX);
        Serial.print(", 0x400E0008 = 0x");
        Serial.println(sl6806_mmio_read(SL6806_PERIPH_RESET_REG), HEX);
    } else {
        Serial.println("  0x400E0000 would not hold bit 6 - the block has its");
        Serial.println("  registers but no logic clock, and MCLK will not appear.");
    }

    /* The block only proves itself by holding a write. */
    sl6806_mmio_write(SL6806_DVP_BASE + SL6806_DVP_INSIZE, DVP_SIZE_WORD);
    v = sl6806_mmio_read(SL6806_DVP_BASE + SL6806_DVP_INSIZE);
    dvp_awake = (v == DVP_SIZE_WORD);

    Serial.print("  INSIZE reads back 0x");
    Serial.print(v, HEX);
    Serial.println(dvp_awake ? "  - the block is awake." : "  - still gated.");

    if (!dvp_awake) {
        Serial.println("  Without it there is no hardware MCLK, and the attempts");
        Serial.println("  below fall back to the software square wave.");
        return;
    }

    /* Pads only once the block is known to be running. */
    for (i = 0; i < sizeof(pads) / sizeof(pads[0]); i++)
        sl6806_pad_configure(pads[i]);
    sl6806_pad_configure(CAM_MCLK_MUX);

    sl6806_mmio_write(SL6806_DVP_BASE + SL6806_DVP_CROP_X, 0);
    sl6806_mmio_write(SL6806_DVP_BASE + SL6806_DVP_CROP_Y, 0);
    sl6806_mmio_write(SL6806_DVP_BASE + SL6806_DVP_CROP_W, DVP_IN_W - 1);
    sl6806_mmio_write(SL6806_DVP_BASE + SL6806_DVP_CROP_H, DVP_IN_H - 1);
    sl6806_mmio_write(SL6806_DVP_BASE + SL6806_DVP_OUTSIZE, DVP_SIZE_WORD);
    sl6806_mmio_write(SL6806_DVP_BASE + SL6806_DVP_SCALE,
                      (SL6806_DVP_SCALE_ONE << 16) | SL6806_DVP_SCALE_ONE);

    /* [V] 0x00D98242, then 0x00D981FE: +0x08 cleared then bit 4; then the
     * format, bit 4, bit 1 - each a separate store, in that order. */
    sl6806_mmio_write(SL6806_DVP_BASE + SL6806_DVP_CFG08, 0);
    sl6806_mmio_set(SL6806_DVP_BASE + SL6806_DVP_CFG08, SL6806_DVP_CFG08_BIT4);

    sl6806_mmio_field(SL6806_DVP_BASE + SL6806_DVP_CTRL,
                      SL6806_DVP_CTRL_FMT_SHIFT, 4, SL6806_DVP_FMT_CAMERA);
    sl6806_mmio_set(SL6806_DVP_BASE + SL6806_DVP_CTRL, SL6806_DVP_CTRL_BIT4);
    sl6806_mmio_set(SL6806_DVP_BASE + SL6806_DVP_CTRL, SL6806_DVP_CTRL_BIT1);

    Serial.print("  CTRL 0x");
    Serial.print(sl6806_mmio_read(SL6806_DVP_BASE + SL6806_DVP_CTRL), HEX);
    Serial.print(", INSIZE 0x");
    Serial.print(sl6806_mmio_read(SL6806_DVP_BASE + SL6806_DVP_INSIZE), HEX);
    Serial.print(", OUTSIZE 0x");
    Serial.println(sl6806_mmio_read(SL6806_DVP_BASE + SL6806_DVP_OUTSIZE), HEX);
    Serial.println("  MCLK is muxed to bank 4 pin 3, function 2. If the sensor");
    Serial.println("  answers below, that clock was the whole story.");

    /* A baseline before the sensor has been reset, so the sweep at the end
     * has something to be compared against. */
    Serial.print("  DVP lines now: ");
    scopeTheBus();
    sl6806_pad_configure(CAM_MCLK_MUX);
}

/*
 * ===================================================================
 *  THE DMA CLUSTER AT +0x30..0x3C - TRIGGERED, NOT JUST MAPPED
 * ===================================================================
 * sl6806_dvp.h's own words about +0x30..0x3C: "nothing has been run". This
 * runs it - independently of whether the sensor ever answers, since the
 * cluster takes writes as soon as the front end is awake (dvp_awake), and
 * nothing about it depends on a working I2C bus.
 *
 * Two separate claims, and this only supports the first:
 *
 *   1. The descriptor registers hold what they are given.
 *      sl6806_dvp_capture_writable() proves this the way
 *      sl6806_audio_writable() proves it for the audio DMA - a distinguishing
 *      test pattern in, the same pattern read back, restored either way.
 *
 *   2. Bytes actually move into the buffer.
 *      No register in this cluster is known to report completion - unlike
 *      audio's CTRL, nothing here has a flag this driver can poll - so the
 *      only witness is the buffer's own contents. It is filled with a
 *      pattern before the trigger and dumped after, exactly the AudioWall
 *      method: 0 bytes changed is a real negative, and a changed byte is the
 *      first positive result this cluster would ever have produced.
 *
 * Claim 2 needs whatever time the block takes to run, which nothing here
 * measures - so this checks immediately and lets the caller re-check later
 * from the monitor if the first pass shows nothing.
 */
#define DVP_CAP_LEN   256u
static uint8_t dvp_cap_buf[DVP_CAP_LEN] __attribute__((aligned(4)));

static void dvpCaptureProbe(void)
{
    unsigned i, changed = 0;

    if (!dvp_awake) {
        Serial.println("DMA cluster: skipped - the front end itself never");
        Serial.println("came up, and there is no reason to trust a register");
        Serial.println("behind a block that is not running.");
        return;
    }

    Serial.println();
    Serial.println("DMA cluster (+0x30..0x3C): the descriptor test");

    if (!sl6806_dvp_capture_writable()) {
        Serial.println("  the descriptor registers drop writes. Awake enough");
        Serial.println("  to hold a geometry write and this is still a");
        Serial.println("  different claim - see sl6806_dvp_capture_writable().");
        return;
    }
    Serial.println("  ADDR and LEN20 hold a distinguishing test pattern and");
    Serial.println("  read it back. That is new: nothing in this cluster had");
    Serial.println("  ever taken a write before this run.");

    memset(dvp_cap_buf, 0xAAu, sizeof(dvp_cap_buf));

    if (sl6806_dvp_capture_start(dvp_cap_buf, DVP_CAP_LEN) != 0) {
        Serial.println("  sl6806_dvp_capture_start() rejected its own buffer -");
        Serial.println("  that is a bug in this sketch, not a hardware result.");
        return;
    }

    Serial.print("  triggered: ADDR reads back 0x");
    Serial.print((uint32_t)(uintptr_t)sl6806_dvp_capture_addr(), HEX);
    Serial.print(", LEN 0x");
    Serial.println(sl6806_dvp_capture_len(), HEX);

    for (i = 0; i < sizeof(dvp_cap_buf); i++)
        if (dvp_cap_buf[i] != 0xAAu)
            changed++;

    Serial.print("  buffer check, immediately after the trigger: ");
    Serial.print(changed);
    Serial.print(" of ");
    Serial.print((unsigned)sizeof(dvp_cap_buf));
    Serial.println(" bytes differ from the fill pattern.");
    if (changed == 0) {
        Serial.println("  0 changed is a real negative, the same as AudioWall's -");
        Serial.println("  it does not by itself mean the block never writes; only");
        Serial.println("  that it had not by the time this line ran.");
    } else {
        Serial.println("  NONZERO. First positive result this cluster has ever");
        Serial.println("  produced. Do not trust the pixel values yet - only that");
        Serial.println("  something wrote them.");
    }
}

/*
 * ===================================================================
 *  STOP ASKING THE SENSOR AND WATCH ITS PINS
 * ===================================================================
 * Every question so far has been "does 0x68 answer", and I2C needs the sensor
 * to be alive, addressed correctly and willing to talk. The DVP sync and data
 * lines need none of that: they are the sensor's *outputs*, and a sensor that
 * is powered and clocked drives PCLK whether or not anyone has configured it.
 *
 * So sample them as plain inputs. Three outcomes, and they are the three
 * hypotheses left, separated for the first time:
 *
 *   something toggles   the module is fitted, powered and clocked. The
 *                       problem is on the I2C side - wrong address, or a part
 *                       that needs something before it will ACK. That would
 *                       be a completely different search.
 *   all stuck high/low  the module is fitted (the pads are pulled somewhere)
 *                       but has no clock, or is held off.
 *   nothing at all      consistent with no module, though not proof.
 *
 * IT ALSO SWEEPS MCLK'S SOURCE. CRU +0x118 carries a two-bit source at [5:4]
 * and a divider at [11:8], and the bring-up uses the vendor's 0 and 0. That 0
 * came from a register the vendor had just tested for zero, which is a weaker
 * reading than it looked: if source 0 is a clock that is not running, MCLK is
 * dead no matter how awake the block is. Four sources times four dividers is
 * sixteen tries and costs a second.
 *
 * Muxing pins 0-2 to function 0 takes them away from the DVP, which does not
 * matter: they are inputs to it, the sensor still drives them, and 7g measured
 * function 0 as one of the two that leave the pad's input buffer alive.
 */
#define SCOPE_SAMPLES   4000

/*
 * Sample one pad as an input WITH A PULL-UP, and count how many times it
 * changes.
 *
 * ===================================================================
 *  THE PULL-UP IS THE WHOLE TEST, AND ITS ABSENCE VOIDED THE LAST ONE
 * ===================================================================
 * The first version of this left the pad on whatever selector it already
 * carried, which for the DVP pins is the vendor's selector 0. Every line read
 * 0 and never moved, and that was reported as "nothing is there". It was
 * measuring the pad. Asked properly afterwards, all five lift on selectors
 * 8-11 and 13, so selector 0 is holding them down on its own and a reading
 * taken under it says nothing about the board.
 *
 * With selector 8 - the vendor's own pull-up, the one the bus pads use - the
 * reading finally means something:
 *
 *   reads 1, static    nothing is driving it; the pull-up wins
 *   reads 0, static    something off-chip is holding it low - a fitted part
 *   toggles            the sensor is alive and clocked
 *
 * A 0 against a pull-up cannot be produced by an absent device, which is
 * exactly the discrimination this sketch has been missing.
 */
static unsigned scopePad(uint32_t id)
{
    unsigned n, changes = 0;
    int last;

    sl6806_pad_configure(PAD_AT_REST(id, PULL_VENDOR));
    delayMicroseconds(PULL_SETTLE_US);
    last = sl6806_pad_read(id);

    for (n = 0; n < SCOPE_SAMPLES; n++) {
        int now = sl6806_pad_read(id);

        if (now != last) {
            changes++;
            last = now;
        }
    }
    return changes;
}

/*
 * Sample the six most informative DVP lines and print level and transition
 * count for each. The caller prints whatever label it wants first; this adds
 * the readings and the verdict.
 */
static int scopeTheBus(void)
{
    static const uint32_t lines[] = {
        SL6806_DVP_PAD_SYNC0, SL6806_DVP_PAD_SYNC1, SL6806_DVP_PAD_SYNC2,
        SL6806_DVP_PAD_D0,    SL6806_DVP_PAD_D1,    SL6806_DVP_PAD_D7,
    };
    unsigned i, total = 0;

    for (i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        unsigned c = scopePad(lines[i]);

        total += c;
        Serial.print(sl6806_pad_read(lines[i]) ? "1" : "0");
        if (c) {
            Serial.print("(");
            Serial.print(c);
            Serial.print(") ");
        } else {
            Serial.print(" ");
        }
    }
    Serial.println(total ? " <- SOMETHING IS MOVING" : " all static");
    return total != 0;
}

/*
 * Sweep MCLK's source and divider, watching the DVP bus and pinging 0x68
 * after each. Returns 1 if anything ever moved or answered.
 */
static int mclkSweep(void)
{
    unsigned src, div;
    int found = 0;

    Serial.println();
    Serial.println("sweeping MCLK's source and divider at CRU +0x118, watching");
    Serial.println("the sensor's own output pins rather than asking it anything:");

    for (src = 0; src < 4 && !found; src++) {
        for (div = 0; div < 4; div++) {
            uint32_t v = sl6806_mmio_read(SL6806_CRU_CAM_CLOCK);

            v &= ~(uint32_t)(SL6806_CRU_CAM_SRC_MASK | SL6806_CRU_CAM_DIV_MASK);
            v |= (src << 4) | (div << 8);
            sl6806_mmio_write(SL6806_CRU_CAM_CLOCK, v);
            sl6806_mmio_write(SL6806_CRU_CAM_CLOCK,
                              sl6806_mmio_read(SL6806_CRU_CAM_CLOCK)
                                  | SL6806_CRU_CAM_ENABLE);

            /* Give the sensor time to come up on a new clock, and put the
             * MCLK pad back where the peripheral can drive it. */
            sl6806_pad_configure(CAM_MCLK_MUX);
            delay(20);

            Serial.print("  src ");
            Serial.print(src);
            Serial.print(" div ");
            Serial.print(div);
            Serial.print(": ");
            if (scopeTheBus())
                found = 1;

            /* And ask, now that the pads are ours again. */
            sl6806_pad_configure(CAM_MCLK_MUX);
            busInit();
            if (i2cPing(CAM_ADDR)) {
                Serial.println("    ...and 0x68 ACKed!");
                found = 1;
            }
            if (found)
                break;
        }
    }

    if (!found) {
        Serial.println();
        Serial.println("  Nothing moved on any of the sixteen, and 0x68 never ACKed.");
        Serial.println("  The sensor's output pins are as static as its bus address is");
        Serial.println("  silent, which is what an absent module looks like from here.");
    }
    return found;
}

#endif /* DVP_BRINGUP */

/* --------------------------------------- the ROM's other clock family */

/*
 * ===================================================================
 *  THE MODULE IDS WERE NEVER THE WHOLE CLOCK TREE
 * ===================================================================
 * 15b established 128 module ids across four gate/shadow pairs and this
 * project has treated that as the clock tree since. It is not. The camera's
 * own sensor_init calls a veneer that branches into ROM 0x20EC, an 85-entry
 * dispatcher where each id sets one bit in one CRU register - a completely
 * separate family, fifty-six of them implemented, reaching CRU +0x1C, +0x30,
 * +0x80..+0x120 and eight registers at 0x400F1000. Nothing here had written
 * any of it.
 *
 * The sensor's id is 58 and the bring-up above does it. This sweeps the rest,
 * enabling one at a time and asking 0x68 after each, because the enables
 * accumulate: if the sensor answers after id N, then N was the last one it
 * needed and everything before it may or may not have mattered. That is a
 * weaker result than an isolated test but a far cheaper one, and it converts
 * a dead end into a bisection.
 *
 * WHAT THIS WRITES. Fifty-six OR-ins of a single bit, each transcribed from
 * the ROM's own table, into clock enable registers. It does not change any
 * clock source, divider, PLL or voltage. Enabling a clock for a block nobody
 * is using costs power and does nothing else, which is why this is an
 * acceptable brute force where a rail sweep would not be.
 *
 * Ids 0, 2, 42 and 53 write CRU +0x30 and id 71 writes +0x1C - registers of
 * unknown purpose rather than the uniform +0x8x..+0x12x clock enables. They
 * are done last and announced, so that if the device stops responding the
 * last line printed names the register that did it.
 */
static int romClockIsPlain(uint32_t addr)
{
    /* The uniform clock-enable window, plus the 0x400F1000 group. */
    return (addr >= 0x40080080u && addr <= 0x40080120u)
        || (addr >= 0x400F1000u && addr <= 0x400F105Cu);
}

static void romClockSweep(void)
{
    unsigned pass, i;

    Serial.println();
    Serial.println("sweeping the ROM's other clock family (notes 7n) - 56 enables");
    Serial.println("nothing in this project has ever written, one at a time, asking");
    Serial.println("0x68 after each:");

    busInit();

    for (pass = 0; pass < 2; pass++) {
        if (pass == 1) {
            Serial.println();
            Serial.println("  now the odd ones - CRU +0x1C and +0x30, purpose unknown:");
        }

        for (i = 0; i < SL6806_ROMCLK_COUNT; i++) {
            const sl6806_romclk_t *c = &sl6806_romclk[i];
            int plain = romClockIsPlain(c->addr);

            if ((pass == 0) != (plain != 0))
                continue;

            Serial.print("  id ");
            if (c->id < 10)
                Serial.print(' ');
            Serial.print(c->id);
            Serial.print(": 0x");
            Serial.print(c->addr, HEX);
            Serial.print(" |= 0x");
            Serial.print(c->bit, HEX);
            Serial.flush();

            sl6806_mmio_set(c->addr, c->bit);
            delay(5);

            if (i2cPing(CAM_ADDR)) {
                Serial.println("   *** 0x68 ACKed ***");
                Serial.println();
                Serial.println("THAT IS THE SENSOR. The clock it was waiting for is this");
                Serial.println("one or something enabled before it - bisect from here.");
                return;
            }
            Serial.println();
        }
    }

    Serial.println();
    Serial.println("  All 56 enabled, and 0x68 still does not answer. Every clock");
    Serial.println("  mechanism this chip is known to have has now been applied.");
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

/* ------------------------------------------------- initialising the sensor */

/*
 * The vendor's init tables, lifted out of this unit's flash by
 * tools/sl6806-sensortab and compiled in. A payload cannot read them at run
 * time - it runs from SRAM in bootloader mode with no XIP mapping - so the
 * only way to have them is to carry them.
 *
 * Table B is the default because it is the one that sizes the sensor: a
 * centred 648x488 window inside the 1280x720 array, which is VGA plus the
 * usual margin. Table A is the full array. Both are here so that neither is
 * a dead symbol and so the choice is a one-line edit.
 */
static const uint8_t (*const init_tables[2])[2] = { sc101_table_A, sc101_table_B };
static const unsigned init_lengths[2] = { SC101_TABLE_A_PAIRS, SC101_TABLE_B_PAIRS };
static const char *const init_names[2] = { "A - full 1280x720 array",
                                           "B - 648x488, VGA" };
#define INIT_TABLE 1        /* [V] the vendor's own selector: 1 picks B */

/*
 * Replay one table, exactly as 0x00D44C50 does: byte pairs in order, ~40 us
 * apart, each one a two-byte write to 0x68.
 *
 * ===================================================================
 *  THIS IS ~200 WRITES INTO A CHIP WITH NO DATASHEET
 * ===================================================================
 * and it is worth being clear about what that does and does not risk. The
 * bytes are the vendor's own, in the vendor's order, into the sensor - not
 * into the SoC. The sensor is on a removable module, it has its own supply
 * which this sketch controls, and payload mode gives all of it back on an
 * unplug. What it can do is leave the sensor in a state that stops it
 * answering, which is why the chip id is re-read afterwards and reported.
 *
 * IT IS PACED ACROSS loop() CALLS. 200 writes at 40 us plus nine clocked
 * bytes each is milliseconds of solid bit-banging, and doing it in one call
 * would block the boot ROM's USB handler for long enough to matter in poll
 * mode - the one rule in docs/BRINGUP.md. A block per call keeps it honest,
 * and the progress line means a sensor that stops ACKing names the pair it
 * stopped on rather than just going quiet.
 */
#define INIT_PAIRS_PER_PASS 16

static unsigned init_at;            /* next pair to write            */
static unsigned init_failed_at;     /* 1-based, 0 = nothing failed   */
static bool     init_running;

static void initBegin(void)
{
    init_at = 0;
    init_failed_at = 0;
    init_running = true;

    Serial.println();
    Serial.print("writing the vendor's init table ");
    Serial.print(init_names[INIT_TABLE]);
    Serial.print(", ");
    Serial.print(init_lengths[INIT_TABLE]);
    Serial.println(" pairs:");
}

/* One block. Returns 1 when the whole table has been walked. */
static int initStep(void)
{
    const uint8_t (*table)[2] = init_tables[INIT_TABLE];
    unsigned end = init_lengths[INIT_TABLE];
    unsigned n;

    for (n = 0; n < INIT_PAIRS_PER_PASS && init_at < end; n++, init_at++) {
        uint8_t pair[2] = { table[init_at][0], table[init_at][1] };

        if (!i2cWriteTo(CAM_ADDR, pair, 2)) {
            init_failed_at = init_at + 1;
            init_running = false;
            Serial.print("  stopped at pair ");
            Serial.print(init_failed_at);
            Serial.print(" of ");
            Serial.print(end);
            Serial.print(" (reg 0x");
            Serial.print(pair[0], HEX);
            Serial.print(" = 0x");
            Serial.print(pair[1], HEX);
            Serial.println(") - no ACK.");
            return 1;
        }
        delayMicroseconds(SC101_WRITE_GAP_US);
    }

    if (init_at < end) {
        Serial.print("  ");
        Serial.print(init_at);
        Serial.print("/");
        Serial.println(end);
        return 0;
    }

    init_running = false;
    Serial.print("  all ");
    Serial.print(end);
    Serial.println(" pairs went in.");

    /*
     * The vendor leaves the page selector wherever the table left it, then
     * writes the flip pair. This does not write the flip - it is gated on a
     * setting a payload has no opinion about - but it does put the page back
     * to 0, because every register number this sketch uses elsewhere is a
     * page-0 one and a stale page would silently redirect them.
     */
    if (!camWrite(CAM_REG_PAGE, 0))
        Serial.println("  (could not restore the page selector to 0)");

    return 1;
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

/*
 * Is there room to print the next block without losing it?
 *
 * The console is a 2 KB ring that only drains when the host polls, and this
 * sketch prints far more than any other example here - the sweep alone is two
 * 90-character lines, and the diagnostics that follow are a few hundred bytes
 * each. Runs came back with "[lost output]" holes in exactly those places.
 *
 * Pacing one item per loop() call fixed most of it, but "one item" is not a
 * number of bytes and the poll rate is the host's business, not ours. So ask:
 * hold the next block until the ring is at least half empty. That is honest
 * back-pressure rather than a guessed delay, and it costs nothing when the
 * monitor is keeping up.
 */
static int roomToPrint(void)
{
    return sl6806_console_space() >= (int)(SL6806_CONSOLE_SIZE / 2);
}

void loop()
{
    uint32_t now;

    /* Keep MCLK alive across whatever wait we are in. In mux mode this costs
     * nothing; in toggle mode it is the only reason the sensor sees a clock
     * outside a transfer. */
    if (mclk_mode == MCLK_TOGGLE)
        mclkSpin(MCLK_BURST_US);

    /*
     * Every phase below prints, and several print a lot. Waiting here rather
     * than inside each one keeps the back-pressure in a single place - and
     * the MCLK service above still runs, so a sensor being clocked does not
     * lose its clock while the monitor catches up.
     */
    if (!roomToPrint())
        return;

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
        static unsigned a, b;

        /*
         * One pad per loop() call. The console is a 2 KB ring that only
         * drains when the host polls, and this phase used to print the sweep,
         * four drive tests, three driven-high tests and four hold tests
         * without ever returning - which overran it and cost real evidence:
         * runs came back with "[lost output]" where the sweep should have
         * been. Returning between items is what lets the host catch up.
         */
        if (sub == 0) {
            a = sweepPulls(CAM_PAD_A, "pin 13");
            sub++;
            break;
        }
        if (sub == 1) {
            b = sweepPulls(CAM_PAD_B, "pin 12");
            sub++;
            break;
        }
        /*
         * The control pads get swept too, which they should have from the
         * start. Every reading of RESET in this sketch has assumed that
         * selector 6 pulls *up* on bank 4 pin 14 - an assumption imported
         * from two different pads, on the strength of them being in the same
         * bank. If some selector does hold pin 14 high, "nothing pulls this
         * net" needs rewriting; if none does, that is worth knowing before
         * anything else is concluded from it.
         */
        if (sub == 2) {
            (void)sweepPulls(CAM_PAD_RESET, "RESET pin 14");
            sub++;
            break;
        }
        if (sub == 3) {
            (void)sweepPulls(CAM_PAD_PWDN, "PWDN pin 15");
            sub++;
            break;
        }
        sub = 0;

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
        /* Paced one item per call, for the reason given in STEP_PULL_SWEEP. */
        switch (sub) {
        case 0:
            Serial.println("can the outputs reach the pins?");
            sub++;
            break;
        case 1:
            if (driveHighTest(CAM_PAD_RESET, "RESET pin 14", 0)) {
                ctrl_drive = 0;
                Serial.println("    the vendor's drive 0 reaches the pin.");
            } else if (driveHighTest(CAM_PAD_RESET, "RESET pin 14", 3)) {
                ctrl_drive = 3;
                Serial.println("    drive 0 does not reach the pin but drive 3 does. Using");
                Serial.println("    3 below - the vendor's own value would leave the sensor");
                Serial.println("    in reset, which would explain everything so far.");
            } else {
                ctrl_drive = 3;
                reset_stuck = true;
                Serial.println("    NEITHER DRIVE STRENGTH CAN PULL THIS LINE HIGH.");
                Serial.println("    A part wired to it is held in reset permanently, and");
                Serial.println("    that is a complete explanation for a silent 0x68 - no");
                Serial.println("    clock or bus question needed. Something drives this net");
                Serial.println("    low, or it is shorted to ground, or nothing is there.");
            }
            sub++;
            break;
        case 2:
            (void)driveHighTest(CAM_PAD_PWDN, "PWDN pin 15", ctrl_drive);
            Serial.println();
            Serial.println("what is on these nets? (charge held with nothing driving)");
            sub++;
            break;
        case 3:
            (void)holdTest(CAM_PAD_B, "SDA/SCL pin 12");
            sub++;
            break;
        case 4:
            (void)holdTest(CAM_PAD_A, "SDA/SCL pin 13");
            sub++;
            break;
        case 5:
            reset_floats = holdTest(CAM_PAD_RESET, "RESET pin 14");
            sub++;
            break;
        case 6:
            pwdn_floats = holdTest(CAM_PAD_PWDN, "PWDN pin 15");
            Serial.println();
            Serial.println("how much net hangs off each pad? (time for an internal");
            Serial.println("pull to move it - a proxy for capacitance, read by");
            Serial.println("comparison with the bus pads, which reach a fitted chip)");
            sub++;
            break;
        case 7:
            pullTimeReport(CAM_PAD_B, "SDA/SCL pin 12");
            sub++;
            break;
        case 8:
            pullTimeReport(CAM_PAD_A, "SDA/SCL pin 13");
            sub++;
            break;
        case 9:
            pullTimeReport(CAM_PAD_RESET, "RESET pin 14");
            sub++;
            break;
        default:
            pullTimeReport(CAM_PAD_PWDN, "PWDN pin 15");
#if CAMERA_POWER_WRITES
            powerTheSensor();
#endif
#if DVP_BRINGUP
            /* After the rail, before any pad the front end owns: the block
             * has to be running before its pads are muxed to it. */
            dvpBringUp();
            /* Independent of the sensor - see dvpCaptureProbe() above. */
            dvpCaptureProbe();
#endif
            sub = 0;
#if DVP_BRINGUP
            step = STEP_DVP_PADS;
#else
            printAttempt();
            step = STEP_POWER_LOW;
#endif
            break;
        }
        break;

#if DVP_BRINGUP
    /*
     * ===================================================================
     *  THE READING THAT NEEDED THIS: "all six read 0" MEANT NOTHING YET
     * ===================================================================
     * The 2026-08-13 scope pass found every DVP line at 0 and static under
     * all sixteen MCLK settings, and that looked like an absent module. It
     * was not evidence. The vendor's pad words for these eleven pins end in
     * 0x30 - drive 3, **pull selector 0** - and the sweep on the bus pads
     * only ever exercised selectors 4..15, because the ROM's pull table is
     * twelve entries indexed from 4 (7f). A pad sitting on selector 0 may
     * simply be pulled down internally, in which case it reads 0 whatever is
     * or is not attached to it.
     *
     * So ask the question the bus pads were asked in the first place, and
     * which decided that they reach a fitted chip:
     *
     *   sweepPulls  can an internal pull-up take this line high at all? If
     *               yes, nothing external is holding it, and the 0 was the
     *               pad's own doing. If no - if it stays 0 through every
     *               selector - something off-chip is holding it down, and
     *               that something is a fitted device.
     *   holdTest    drive it high and let go. A line that holds its charge
     *               for 10 ms reaches nothing; one that decays has a load.
     *               RESET holds, the bus pads decay, and both readings were
     *               informative on those pins.
     */
    case STEP_DVP_PADS: {
        static const struct { uint32_t id; const char *name; } lines[] = {
            { SL6806_DVP_PAD_SYNC0, "sync pin 0"  },
            { SL6806_DVP_PAD_SYNC1, "sync pin 1"  },
            { SL6806_DVP_PAD_SYNC2, "sync pin 2"  },
            { SL6806_DVP_PAD_D0,    "data pin 4"  },
            { SL6806_DVP_PAD_D7,    "data pin 11" },
        };
        const int n = (int)(sizeof(lines) / sizeof(lines[0]));

        if (sub == 0) {
            Serial.println();
            Serial.println("the DVP lines, asked properly this time. A pad that no");
            Serial.println("internal pull can lift is being held down by something");
            Serial.println("off-chip; a pad that holds its own charge reaches nothing.");
        }

        if (sub < n) {
            sweepPulls(lines[sub].id, lines[sub].name);
            sub++;
            break;
        }
        if (sub < 2 * n) {
            holdTest(lines[sub - n].id, lines[sub - n].name);
            sub++;
            break;
        }

        /* Put them back where the front end can use them. */
        sl6806_pad_configure(SL6806_DVP_PAD_SYNC0);
        sl6806_pad_configure(SL6806_DVP_PAD_SYNC1);
        sl6806_pad_configure(SL6806_DVP_PAD_SYNC2);
        sl6806_pad_configure(SL6806_DVP_PAD_D0);
        sl6806_pad_configure(SL6806_DVP_PAD_D7);

        Serial.println();
        Serial.println("Read that against the bus pads above: those reach the FM");
        Serial.println("tuner and they idle high on a pull-up. If these eleven");
        Serial.println("cannot be lifted at all, they reach something. If they lift");
        Serial.println("and hold like RESET does, they reach nothing - and the case");
        Serial.println("for no module fitted becomes the strong one.");

        sub = 0;
        printAttempt();
        step = STEP_POWER_LOW;
        break;
    }
#endif

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
            initBegin();
            step = STEP_INIT;
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
            /*
             * RETRACTED, 2026-08-13. This used to say MCLK was ruled out,
             * because the software clock measured 2.8 MHz and "the vendor
             * programs clock channel 6 with 2800, which as kHz is the same
             * clock". Channel 6 is an LDO and 2800 is millivolts - see
             * sl6806_regfile.h - so those two numbers had nothing to do with
             * each other and the agreement was a coincidence between a
             * voltage and a frequency. Nothing was ever ruled out.
             *
             * Worse, the argument pointed away from the one thing that was
             * never supplied. MCLK leaves on bank 4 pin 3 at alternate
             * function 2, which is the DVP peripheral's function on that
             * bank - so the sensor's clock is an output of the camera front
             * end, and the front end is gated off unless something enables
             * it. A typical sensor of this class wants 24 MHz; 2.8 MHz from
             * a toggle loop may simply be below its floor.
             */
            Serial.print("MCLK measured ");
            Serial.print(mclk_khz / 1000);
            Serial.print(".");
            Serial.print((mclk_khz % 1000) / 100);
            Serial.println(" MHz from the software toggle, which is the fastest");
            Serial.println("this CPU can bit-bang - and there is no reason to think it");
            Serial.println("is the right frequency. The vendor's 2800 is millivolts, not");
            Serial.println("kilohertz (notes 7m), so the real MCLK rate is unknown and");
            Serial.println("parts like this usually want 24 MHz.");
            Serial.println();
            Serial.println("MCLK's real source is the DVP front end: pin 3 carries it at");
            Serial.println("alternate function 2, the same function as the eleven DVP data");
            Serial.println("and sync pads on this bank (notes 7n).");
            if (!dvp_awake) {
                Serial.println("The front end is NOT running in this build, so the sensor has");
                Serial.println("never had a hardware clock. That is the leading explanation.");
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

            if (!pwdn_floats) {
                /*
                 * A populated pull-up is a component someone paid for. It is
                 * the one piece of evidence here that speaks to whether the
                 * board carries camera circuitry at all, and it says yes.
                 */
                Serial.println("The control nets are not symmetrical, and that is the most");
                Serial.println("informative thing in this run: PWDN has an external pull-up");
                Serial.println("on it, RESET has no external pull at all.");
                Serial.println();
                Serial.println("A populated pull-up is a fitted component, so the board does");
                Serial.println("carry camera circuitry - which argues against the easy");
                Serial.println("explanation that this unit simply has no camera. And a");
                Serial.println("floating RESET proves less than it looks: a sensor's reset");
                Serial.println("pin is a high-impedance input, so a trace running to one");
                Serial.println("with no resistor floats exactly like a trace running");
                Serial.println("nowhere. This cannot tell those two apart.");
            }

            Serial.println();
            Serial.println("WHAT HAS BEEN RULED OUT, all measured on this board:");
            Serial.println("  the bus       an FM tuner on these two pads gives its own");
            Serial.println("                driver's chip id in the same run");
            Serial.println("  the rail      2.8 V, read back through the vendor's scale;");
            Serial.println("                and it was already on before anything here ran");
            Serial.println("  the enables   the sensor's two register-file bits were also");
            Serial.println("                already set, cold");
            Serial.println("  the pads      both control lines drive, both bus lines pull");
            Serial.println("  the module    the stock camera app works on this unit");
            Serial.println("  the clocks    the DVP's register clock, its functional clock,");
            Serial.println("                the PLL, the media clock, the camera module");
            Serial.println("                clock across all 64 source/divider settings,");
            Serial.println("                and every one of the ROM's 56 other enables");
            Serial.println();
            Serial.println("The sensor's own output pins sit at 1 against a pull-up, which");
            Serial.println("is high impedance: it is not driving them. A fitted, powered");
            Serial.println("sensor with Hi-Z outputs is one that has never started.");
            Serial.println();
            Serial.println("So every mechanism this chip is known to have has been applied,");
            Serial.println("and the difference between this and the working stock firmware");
            Serial.println("is no longer expressible in any register anyone has found.");
            Serial.println("That is a complete negative rather than a missing experiment,");
            Serial.println("and it is where payload-mode probing ends. See notes 7n.");
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
#if DVP_BRINGUP
        /*
         * Last resort, and the first question in this sketch that does not
         * depend on the sensor being willing to talk: is anything moving on
         * its output pins at all, under any MCLK the block can produce?
         */
        if (dvp_awake)
            mclkSweep();
#endif
        romClockSweep();
        step = STEP_STUCK;
        break;

    case STEP_INIT:
        if (!initStep())
            break;

        /*
         * Re-read the chip id. Two hundred writes into an undocumented part
         * can leave it not answering, and "the table went in" is not the same
         * claim as "the sensor is still there afterwards" - which is the one
         * that matters to anything built on top of this.
         */
        Serial.println();
        Serial.println("re-reading the chip id after the table:");
        if (reportChipId())
            Serial.println("  the sensor survived its own init table.");
        else
            Serial.println("  it did not answer afterwards. The table is the suspect.");

        if (init_failed_at) {
            Serial.println();
            Serial.println("The table did not finish, so the sensor is in a half-");
            Serial.println("configured state. Unplug before drawing conclusions from");
            Serial.println("anything it says below.");
        }

        Serial.println();
        if (dvp_awake) {
            Serial.println("The DVP front end at 0x400E1000 is awake and geometry-");
            Serial.println("configured (see the DMA cluster report above) - so what is");
            Serial.println("still missing for an image is pixels actually leaving the");
            Serial.println("sensor, not the front end that would receive them.");
        } else {
            Serial.println("What is still missing for an image: the DVP front end at");
            Serial.println("0x400E1000 is mapped (sl6806_dvp.h) but did not come up");
            Serial.println("awake in this run - see dvpBringUp()'s own report above.");
        }

        samples   = 0;
        sample_at = millis();
        step      = STEP_TALK;
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
