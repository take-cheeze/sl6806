/*
 * TouchDemo - report touches, with coordinates, from the P20's touch panel.
 *
 * The controller is a Hynitron CST816 family part on TWI bus 1 at address
 * 0x15, decoded register for register in docs/sl6806_re_notes.md §7h. The
 * coordinates live behind a 7-byte I2C read at register 0x00, and the SL6806's
 * TWI controller has never been located - the stock firmware reaches it only
 * through mask-ROM routines that a payload cannot call.
 *
 * ===================================================================
 *  SO THIS SKETCH DOES NOT USE THE TWI CONTROLLER. IT BIT-BANGS.
 * ===================================================================
 * Every line the chip needs is a pad whose id is known, and the pad
 * controller works (cores/sl6806/sl6806_padctl.c, read out of the mask ROM):
 *
 *     RESET  0x00018000   bank 1 pin 16, output
 *     INT    0x00016800   bank 1 pin 13, input, ACTIVE LOW
 *     SDA    0x00017618   bank 1 pin 14, vendor alt function 12
 *     SCL    0x00017E18   bank 1 pin 15, vendor alt function 12
 *
 * Take those last two away from function 12, drive them as GPIO, and I2C is
 * just software. That is the whole trick: a missing peripheral base costs
 * speed, not access. The vendor runs this bus at 200 kHz; this runs it at
 * roughly 100 kHz, which is nowhere near a bottleneck for seven bytes per
 * touch.
 *
 * OPEN DRAIN ON A PAD CONTROLLER THAT HAS NO OPEN-DRAIN MODE. I2C needs both
 * lines released-high and driven-low, never driven-high. Here each pad is
 * configured once as an input with a pull-up and a 0 latched in its output
 * register; after that, flipping the function nibble to 1 (output) drives the
 * line low and flipping it back to 0 (input) releases it to the pull-up.
 * sl6806_pad_set_func() changes exactly that nibble and nothing else, so the
 * drive strength and pull set up front survive every edge.
 *
 * WHICH PAD IS SDA. §7h lists the pair in the order SDA, SCL because that is
 * the order the firmware configures them in, which is an assumption and not a
 * reading. So the sketch does not trust it: it probes with one assignment,
 * and if nothing answers it swaps them and probes again. Whichever ACKs is
 * reported, and that settles the question on hardware.
 *
 * WHAT IS VERIFIED HERE AND WHAT IS NOT. The pad ids, the chip's register
 * numbers and the coordinate decode below are all [V] - out of this unit's
 * own firmware. The bus timing is ordinary I2C and owes nothing to the
 * SL6806. What has never been run before this sketch is the combination, so
 * treat a silent bus as "the assignment or the pull-ups are wrong" rather
 * than as a verdict on §7h: an unanswered address is exactly what a swapped
 * SDA/SCL looks like, which is why the swap is automatic.
 *
 *     make SKETCH=examples/TouchDemo run
 *     make SKETCH=examples/TouchDemo RUN_MODE=poll run   (if loop() never ticks)
 *
 * Payload mode never writes flash, so an unplug undoes anything here.
 */

#include <Arduino.h>
#include "sl6806_padctl.h"

/* ------------------------------------------------------------- the pads */

#define TP_RESET_PAD  0x000180F0u   /* [V] the vendor's word: output, drive 3 */

/*
 * INT. The vendor uses 0x00016F08 - alternate function 14, pull selector 8 -
 * because it wants an interrupt. This sketch polls, so it takes function 0
 * (plain input) and keeps the same pull-up. examples/PadScope measured
 * function 0 as one that leaves the input buffer alive, which is what makes
 * the pad readable at all.
 */
#define TP_INT_PAD    0x00016808u

/* The two bus pads, as the firmware configures them: [V] §7h. Only the bank
 * and pin fields are used below - the function nibble is this sketch's to
 * drive. */
#define TP_PAD_A      0x00017618u   /* bank 1 pin 14 */
#define TP_PAD_B      0x00017E18u   /* bank 1 pin 15 */

/* Bank and pin of a pad id, with every configuration field cleared. */
#define PAD_WHICH(id) ((uint32_t)(id) & 0x000FF800u)

/*
 * A bus pad at rest: input (function 0), drive 1 and pull selector 8, both
 * the vendor's. Pull 8 is the selector examples/PadScope measured as the one
 * that holds a floating input high.
 */
#define PAD_RELEASED(id)  (PAD_WHICH(id) | (1u << 4) | 8u)

/* ---------------------------------------------------------- the CST816 */

#define TP_ADDR       0x15u   /* [V] 7-bit; §7h                            */
#define TP_REG_DATA   0x00u   /* [V] 7 bytes from here carry a touch       */
#define TP_REG_CHIPID 0xA7u   /* [V] one byte                              */
#define TP_REG_FWVER  0xA9u   /* [V] two bytes                             */

/* [V] The vendor clamps y to this before using it (0x00D3FFD2). Reported
 * rather than applied - a coordinate off the end is worth seeing raw. */
#define TP_Y_CLAMP    286

/* --------------------------------------------------------- bus timing */

/* Half a bit, microseconds. ~100 kHz before call overhead, which the chip is
 * happy with - the vendor drives it at 200 kHz. Slower is always safe. */
#define I2C_HALF_US   5

/* How long to wait for a slave that is stretching the clock. Generous: this
 * is an error path, and in poll mode it still has to return promptly. */
#define I2C_STRETCH_US 2000

/* ---------------------------------------------------------- the sketch */

#define WINDOW_MS      20      /* how long each loop() call watches INT     */
#define HEARTBEAT_MS   5000    /* proof of life while nothing happens       */
#define LATCHED_MS     3000    /* INT low this long is not a finger         */
#define DRAG_MS        100     /* how often to re-read a finger still down  */

enum {
    STEP_LINE,        /* does INT read at all, before anything is driven */
    STEP_RESET_LOW,   /* drive RESET low                                 */
    STEP_RESET_HIGH,  /* release it after 10 ms                          */
    STEP_SETTLE,      /* wait the vendor's 50 ms                         */
    STEP_PROBE,       /* find the chip on the bus                        */
    STEP_WATCH        /* the demo proper                                 */
};

static uint8_t  step = STEP_LINE;
static uint32_t step_at;

static uint32_t pad_sda = TP_PAD_A;   /* swapped by the probe if needed */
static uint32_t pad_scl = TP_PAD_B;
static bool     bus_ok;               /* the chip answered its address  */

static bool     active;               /* INT is being pulled low        */
static uint32_t active_since;
static uint32_t touches;
static bool     latched_reported;
static uint32_t heartbeat_at;
static uint32_t drag_at;              /* last re-read of a finger still down */

/* ------------------------------------------------------- the I2C lines */

static inline void sda_low(void)     { sl6806_pad_set_func(pad_sda, SL6806_PAD_FUNC_OUTPUT); }
static inline void sda_release(void) { sl6806_pad_set_func(pad_sda, SL6806_PAD_FUNC_INPUT); }
static inline void scl_low(void)     { sl6806_pad_set_func(pad_scl, SL6806_PAD_FUNC_OUTPUT); }
static inline int  sda_read(void)    { return sl6806_pad_read(pad_sda); }

/* Release SCL and wait for it to actually rise: a slave is allowed to hold it
 * down to buy itself time, and a bit-banged master that ignores that reads
 * the previous bit twice. Returns 0 if it never rose. */
static int scl_release(void)
{
    uint32_t waited = 0;

    sl6806_pad_set_func(pad_scl, SL6806_PAD_FUNC_INPUT);

    while (sl6806_pad_read(pad_scl) == 0) {
        if (waited >= I2C_STRETCH_US)
            return 0;
        delayMicroseconds(1);
        waited++;
    }
    return 1;
}

static void half(void) { delayMicroseconds(I2C_HALF_US); }

/* Both pads to their resting state, with a 0 waiting in the output register
 * so that "output" means "low" for the rest of the run. */
static void busInit(void)
{
    sl6806_pad_configure(PAD_RELEASED(pad_sda));
    sl6806_pad_configure(PAD_RELEASED(pad_scl));
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

/*
 * Read `n` bytes from register `reg`, the way the vendor driver does: address
 * for writing, the register number, a repeated START, then address for
 * reading. Returns 1 on success.
 */
static int tpRead(uint8_t reg, uint8_t *buf, int n)
{
    int i;

    i2cStart();
    if (!i2cWrite((uint8_t)(TP_ADDR << 1)))      goto fail;
    if (!i2cWrite(reg))                          goto fail;

    i2cStart();                                  /* repeated START */
    if (!i2cWrite((uint8_t)((TP_ADDR << 1) | 1))) goto fail;

    for (i = 0; i < n; i++)
        buf[i] = i2cRead(i != n - 1);

    i2cStop();
    return 1;

fail:
    i2cStop();
    return 0;
}

/* Does anything answer address 0x15 with the assignment currently in force? */
static int tpPresent(uint8_t *chip_id)
{
    return tpRead(TP_REG_CHIPID, chip_id, 1);
}

/* ------------------------------------------------------------ INT watch */

/*
 * Sample INT for `ms`, returning how many samples read low and writing the
 * total to *total. Sampling rather than reading once is the point: a report
 * pulse is far shorter than the gap between two loop() calls.
 */
static uint32_t sampleLow(uint32_t ms, uint32_t *total)
{
    uint32_t start, n = 0, low = 0;

    /* Never spin on a clock that is not running. If neither counter advances
     * micros() is frozen, the window below would never end, and in poll mode
     * an endless loop inside the ROM's USB handler takes the device off the
     * bus until it is unplugged - the same reasoning that makes delay()
     * return immediately in wiring_time.c. */
    if (!sl6806_tick_mask()) {
        *total = 1;
        return digitalRead(PIN_TP_INT) == LOW ? 1u : 0u;
    }

    start = micros();
    while (micros() - start < ms * 1000UL) {
        n++;
        if (digitalRead(PIN_TP_INT) == LOW)
            low++;
    }

    *total = n;
    return low;
}

/* Read one touch and print it. Returns 1 if the chip answered. */
static int reportTouch(void)
{
    uint8_t b[7];
    unsigned x, y, event, points;

    if (!tpRead(TP_REG_DATA, b, 7))
        return 0;

    /* [V] The vendor's own decode, 0x00D3FF24: buf[n] is register n. */
    points = b[2] & 0x0Fu;
    event  = (unsigned)(b[3] >> 4);
    x      = (unsigned)(((b[3] & 0x0Fu) << 8) | b[4]);
    y      = (unsigned)(((b[5] & 0x0Fu) << 8) | b[6]);

    Serial.print("  (");
    Serial.print(x);
    Serial.print(", ");
    Serial.print(y);
    Serial.print(")  ");

    switch (event) {
    case 0:  Serial.print("down");    break;
    case 4:  Serial.print("up");      break;
    case 8:  Serial.print("contact"); break;
    default: Serial.print("event ");  Serial.print(event); break;
    }

    Serial.print(", ");
    Serial.print(points);
    Serial.print(points == 1 ? " point" : " points");

    /* The vendor clamps here rather than trusting the chip; say so instead of
     * hiding it, because a y past the end is evidence about the panel. */
    if (y > TP_Y_CLAMP)
        Serial.print("  [y past the vendor's clamp of 286]");

    Serial.println();
    return 1;
}

/* ------------------------------------------------------------------------ */

void setup()
{
    Serial.begin(115200);
    Serial.println();
    Serial.println("=== SL6806 touch panel demo ===");
    Serial.println("CST816 family, TWI 1 addr 0x15 - notes 7h.");
    Serial.println("I2C is bit-banged on the two TWI1 pads, so this needs no");
    Serial.println("TWI controller: coordinates come straight off the chip.");
    Serial.println();

    /* The pad controller is only wired into digitalRead()/digitalWrite() when
     * something installs it. The display bring-up does; this sketch has no
     * display, so it does it itself. */
    sl6806_gpio_vendor_register(sl6806_padctl_vendor());
    sl6806_pad_configure(TP_RESET_PAD);
    sl6806_pad_configure(TP_INT_PAD);
    busInit();

    if (!sl6806_gpio_available())
        Serial.println("GPIO reports unavailable - nothing below will mean anything.");

    step_at = millis();
}

void loop()
{
    uint32_t total, low, now;

    switch (step) {

    case STEP_LINE: {
        /* Say so before sampling anything. Everything else this sketch prints
         * comes after a 20 ms sampling window, so without this line a loop()
         * that never runs and a loop() that wedges in its first window look
         * identical on the monitor - and they have opposite fixes. */
        static bool announced;
        if (!announced) {
            announced = true;
            Serial.println("loop() is running.");
            break;
        }

        /* Before touching RESET: does the pad read, and what does it idle at?
         * With the pull-up applied and the controller not reporting, this
         * should be solidly high. It is the control for everything after. */
        low = sampleLow(WINDOW_MS, &total);

        Serial.print("INT before reset: ");
        Serial.print(low);
        Serial.print("/");
        Serial.print(total);
        Serial.println(" samples low");

        if (low == 0) {
            Serial.println("  idles high, as a pulled-up interrupt line should.");
        } else if (low == total) {
            Serial.println("  low the whole time. Either the controller is already");
            Serial.println("  holding it - an unread event latches INT - or the pad");
            Serial.println("  is not this line. The reset below distinguishes them.");
        } else {
            Serial.println("  flickering with nothing touching it. Suspect the pad id");
            Serial.println("  before you suspect the panel.");
        }

        Serial.println();
        Serial.println("resetting the controller (low 10ms, high 50ms) ...");
        step = STEP_RESET_LOW;
        step_at = millis();
        break;
    }

    case STEP_RESET_LOW:
        /* The vendor sequence at 0x00D401E6, which it runs immediately before
         * reading the chip-id register. */
        digitalWrite(PIN_TP_RESET, LOW);
        step = STEP_RESET_HIGH;
        step_at = millis();
        break;

    case STEP_RESET_HIGH:
        if (millis() - step_at < 10)
            break;
        digitalWrite(PIN_TP_RESET, HIGH);
        step = STEP_SETTLE;
        step_at = millis();
        break;

    case STEP_SETTLE:
        /* Paced with millis() rather than delay() so poll mode has nothing to
         * clamp: the whole 60 ms is spread over several loop() calls, none of
         * which blocks. */
        if (millis() - step_at < 50)
            break;
        step = STEP_PROBE;
        break;

    case STEP_PROBE: {
        uint8_t id = 0, ver[2] = { 0, 0 };

        busRecover();
        bus_ok = tpPresent(&id);

        if (!bus_ok) {
            /* An unanswered address is what a swapped SDA/SCL looks like, and
             * §7h's order is an assumption. Try the other way round. */
            Serial.println("no answer on SDA=1/14 SCL=1/15; swapping the pair ...");
            pad_sda = TP_PAD_B;
            pad_scl = TP_PAD_A;
            busInit();
            busRecover();
            bus_ok = tpPresent(&id);
        }

        if (bus_ok) {
            Serial.print("chip id 0x");
            Serial.print(id, HEX);
            if (tpRead(TP_REG_FWVER, ver, 2)) {
                Serial.print(", fw version 0x");
                Serial.print(ver[0], HEX);
                Serial.print(ver[1], HEX);
            }
            Serial.print("  (SDA = bank 1 pin ");
            Serial.print(SL6806_PAD_PIN(pad_sda));
            Serial.print(", SCL = pin ");
            Serial.print(SL6806_PAD_PIN(pad_scl));
            Serial.println(")");
            Serial.println();
            Serial.println("touch the screen.");
        } else {
            Serial.println("nothing answers 0x15 either way round.");
            Serial.println("  Falling back to watching INT, which still reports that");
            Serial.println("  a touch happened - just not where. Worth checking:");
            Serial.println("  the pads (notes 7h), and whether this bus has pull-ups");
            Serial.println("  strong enough for the internal ones to be doing the");
            Serial.println("  work here.");
        }
        Serial.println();

        active = false;
        active_since = millis();
        heartbeat_at = millis();
        step = STEP_WATCH;
        break;
    }

    case STEP_WATCH: {
        bool read_this_pass = false;

        low = sampleLow(WINDOW_MS, &total);
        now = millis();

        if (low && !active) {
            active = true;
            active_since = now;
            latched_reported = false;
            touches++;

            Serial.print("touch ");
            Serial.print(touches);
            Serial.print(" at ");
            Serial.print(now);
            Serial.println(" ms");

            /* The read is also what releases INT: the controller holds the
             * line down until its event is collected. So this is not only how
             * the coordinates arrive, it is why the line goes back up. */
            if (bus_ok) {
                read_this_pass = true;
                drag_at = now;
                if (!reportTouch()) {
                    bus_ok = false;
                    Serial.println("  the bus stopped answering; back to INT only.");
                }
            }
        } else if (!low && active) {
            active = false;
            Serial.print("        released after ");
            Serial.print(now - active_since);
            Serial.println(" ms");
        }

        /* While a finger stays down the chip keeps reporting, so keep reading:
         * this is what turns a press into a drag. Throttled, because loop()
         * runs far faster than a finger moves and an unthrottled drag buries
         * everything else in the monitor. */
        if (active && bus_ok && low && !read_this_pass &&
            now - drag_at >= DRAG_MS) {
            drag_at = now;
            (void)reportTouch();
        }

        /* Without the bus, an unread event latches INT low forever and the
         * first touch is the only edge there will ever be. Say so once rather
         * than leaving it looking like a stuck pin. */
        if (active && !bus_ok && !latched_reported &&
            now - active_since > LATCHED_MS) {
            latched_reported = true;
            Serial.println("        INT has stayed low for seconds. If your finger is");
            Serial.println("        off the glass, the controller has latched the event");
            Serial.println("        and is waiting to be read - which is exactly what");
            Serial.println("        the I2C read above would have done. Expected, not a");
            Serial.println("        fault.");
        }

        if (now - heartbeat_at >= HEARTBEAT_MS) {
            heartbeat_at = now;
            Serial.print("[");
            Serial.print(now / 1000);
            Serial.print("s] ");
            Serial.print(touches);
            Serial.print(" touches, INT ");
            Serial.println(active ? "low" : "high");
        }
        break;
    }
    }
}
