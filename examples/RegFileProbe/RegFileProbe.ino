/*
 * RegFileProbe - read the SL6806's indexed register file, now that it is found.
 *
 * ===================================================================
 *  THE HUNT IS OVER. THIS SKETCH CONFIRMS RATHER THAN SEARCHES.
 * ===================================================================
 * The file is not an MMIO window at all, which is why six of them were
 * scanned here and none of them was it. The vendor's two accessors - write at
 * SRAM 0x00804E44, read at 0x00804EAC - drive a four-register mailbox at
 * 0x400F7000+0x100, and the byte they put in the low half of the command word
 * differs by exactly one between the two directions. That is an 8-bit I2C
 * address and its R/W bit: the register file is a chip on a serial bus, at
 * device address 0x30. See cores/sl6806/sl6806_regfile.h and notes 7l.
 *
 * WHAT THIS SKETCH DOES, in three phases and without a single write:
 *
 *   1. Dump all 131 registers the firmware is known to use, 0x00..0x82.
 *   2. Sample eight of them repeatedly, and say which hold still. This is
 *      the phase that exists because of a specific mistake: 7l's wrong
 *      identification rested entirely on one reading of one register that
 *      turned out to change on every read. A register file that is really a
 *      register file should be mostly stable.
 *   3. Decode the five LDO channels and print their voltages in millivolts.
 *      This is the real test, and it is a prediction rather than an
 *      observation: the vendor's setters encode 1700..3300 mV in 50 mV steps
 *      in five bits, so if this is the right block, the decoded rails come
 *      out at recognisable supply voltages - and if it is not, they come out
 *      as noise. Getting 2.8 V and 3.3 V and 1.8 V from a byte that had no
 *      reason to produce them is worth more than any amount of "looks live".
 *
 * NOT ONE WRITE, and that is a deliberate limit rather than caution for its
 * own sake. Channels 2..6 are five power rails, only one of which is known to
 * be the camera's. The others may well be the core, the SRAM this payload is
 * running from, or the USB PHY carrying the console. examples/CameraDemo does
 * the writes, and only the camera's.
 *
 *     make SKETCH=examples/RegFileProbe RUN_MODE=poll run
 *
 * RUN_MODE=poll is not optional advice - see the note below.
 *
 * ===================================================================
 *  WHAT THE OLD SCAN FOUND, so nobody re-runs it
 * ===================================================================
 * This sketch used to read six candidate MMIO bases looking for the file.
 * Measured on hardware, byte-wide:
 *
 *   0x40080000  clock and reset unit, the control - varied, plausible
 *   0x40036000  reads all-zero
 *   0x4010E000  reads all-zero
 *   0x40040000  VARIED AND LIVE - a real peripheral, but not this one
 *   0x40030000  reads all-zero
 *   0x40020000  reads all-zero
 *
 * Nothing faulted. 0x40040000 was briefly identified as the file and then
 * retracted; it is most likely a USB controller. All-zero means gated off at
 * least as plausibly as absent.
 *
 * Payload mode never writes flash, so an unplug undoes anything here.
 */

#include <Arduino.h>
#include "sl6806_console.h"
#include "sl6806_regfile.h"

/* [V] The index range the firmware's 140 call sites cover. */
#define REG_FIRST      0x00
#define REG_LAST       0x82
#define REG_COUNT      (REG_LAST - REG_FIRST + 1)

/* Registers per printed line, and lines per loop() call. */
#define PER_LINE       16
#define LINES_PER_PASS 2

/* Phase 2: how many times to re-read, and which registers. */
#define SAMPLES        8

/*
 * The eight worth watching. The first six are the busiest indices across the
 * firmware; the last two are the camera's own two registers, which should be
 * rock steady on a unit whose camera is off.
 */
static const uint8_t watch[] = {
    0x03, 0x47, 0x1B, 0x13, 0x30, 0x0F, 0x16, 0x2C,
};
#define NWATCH ((int)(sizeof(watch) / sizeof(watch[0])))

/*
 * ===================================================================
 *  BUILD THIS WITH RUN_MODE=poll
 * ===================================================================
 * In the default run mode loop() is driven by the boot ROM's idle callback,
 * which on this unit has been measured as low as a fraction of a call per
 * second. A probe made of small paced steps then looks exactly like a device
 * that has faulted: output stops after the banner and the monitor gives up.
 * In poll mode loop() runs at the USB poll rate and this finishes in seconds.
 */

enum {
    STEP_DUMP,
    STEP_STABLE,
    STEP_RAILS,
    STEP_VERDICT,
    STEP_IDLE,
};

static uint8_t  step = STEP_DUMP;
static unsigned idx = REG_FIRST;

/* Tallies for the verdict. */
static unsigned n_read;        /* completed transfers                  */
static unsigned n_timeout;     /* mailbox never cleared START          */
static unsigned n_zero;
static unsigned n_ff;

static int      sample_round;

static void printHex2(int v)
{
    if (v < 0x10)
        Serial.print('0');
    Serial.print(v, HEX);
}

/*
 * Is there room to print the next block without losing it? The console is a
 * 2 KB ring that only drains when the host polls; holding a block until the
 * ring is half empty is honest back-pressure rather than a guessed delay.
 */
static int roomToPrint(void)
{
    return sl6806_console_space() >= (int)(SL6806_CONSOLE_SIZE / 2);
}

static void railLine(unsigned ch)
{
    int on = sl6806_reg_read(SL6806_RF_LDO_ENABLE);
    int mv = sl6806_ldo_get_mv(ch);

    Serial.print("  channel ");
    Serial.print(ch);
    Serial.print("  ");

    if (on < 0)
        Serial.print("enable=? ");
    else {
        Serial.print((on & (1 << (ch - 2))) ? "ON " : "off");
        Serial.print(' ');
    }

    if (ch == 4) {
        /* The vendor keeps this one's setting in RAM rather than decoding
         * the register, so there is nothing honest to print. */
        int raw = sl6806_reg_read(SL6806_RF_LDO4_MV);
        Serial.print(" (a six-entry menu, raw 0x");
        if (raw >= 0)
            printHex2(raw);
        else
            Serial.print("??");
        Serial.println(")");
        return;
    }

    if (mv < 0)
        Serial.println(" read failed");
    else {
        Serial.print(' ');
        Serial.print(mv);
        Serial.println(" mV");
    }
}

void setup()
{
    Serial.begin(115200);
    Serial.println();
    Serial.println("=== SL6806 indexed register file ===");
    Serial.println("Mailbox at 0x400F7000+0x100; the file is a chip at I2C 0x30.");
    Serial.println("Accessors transcribed from SRAM 0x00804E44 / 0x00804EAC.");
    Serial.println("This sketch only reads. Channels 2..6 are power rails and");
    Serial.println("four of the five are not known to be safe to touch.");
    Serial.println();
    Serial.println("index  00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F");
}

void loop()
{
    int lines;

    if (!roomToPrint())
        return;

    switch (step) {

    case STEP_DUMP:
        for (lines = 0; lines < LINES_PER_PASS && idx <= REG_LAST; lines++) {
            unsigned col;

            Serial.print(" 0x");
            printHex2((int)idx);
            Serial.print("  ");

            for (col = 0; col < PER_LINE && idx <= REG_LAST; col++, idx++) {
                int v = sl6806_reg_read(idx);

                if (v < 0) {
                    Serial.print("-- ");
                    n_timeout++;
                    continue;
                }
                n_read++;
                if (v == 0x00)
                    n_zero++;
                if (v == 0xFF)
                    n_ff++;
                printHex2(v);
                Serial.print(' ');
            }
            Serial.println();
        }
        if (idx > REG_LAST) {
            step = STEP_STABLE;
            sample_round = 0;
            Serial.println();
            Serial.println("do these hold still? (7l's whole mistake was assuming so)");
        }
        break;

    case STEP_STABLE: {
        int i;

        /* One full sweep of the watch list per call, so a moving register
         * shows up as a changing column rather than as drift over time. */
        Serial.print("  ");
        for (i = 0; i < NWATCH; i++) {
            int v = sl6806_reg_read(watch[i]);

            Serial.print("0x");
            printHex2(watch[i]);
            Serial.print('=');
            if (v < 0)
                Serial.print("--");
            else
                printHex2(v);
            Serial.print(' ');
        }
        Serial.println();

        if (++sample_round >= SAMPLES) {
            step = STEP_RAILS;
            Serial.println();
            Serial.println("the LDO channels, decoded with the vendor's own scales:");
        }
        break;
    }

    case STEP_RAILS: {
        unsigned ch;

        for (ch = 2; ch <= 6; ch++)
            railLine(ch);

        Serial.println();
        Serial.println("Channel 6 at 2800 mV is the camera's - notes 7h, 0x00D44B1C.");
        step = STEP_VERDICT;
        break;
    }

    case STEP_VERDICT:
        Serial.println();
        Serial.print("read ");
        Serial.print(n_read);
        Serial.print(" of ");
        Serial.print(REG_COUNT);
        Serial.print(", timed out ");
        Serial.print(n_timeout);
        Serial.print(", zero ");
        Serial.print(n_zero);
        Serial.print(", 0xFF ");
        Serial.println(n_ff);

        if (n_timeout == REG_COUNT) {
            Serial.println("VERDICT: the mailbox never completed a transfer. Either");
            Serial.println("0x400F7000 is gated off in payload mode, or the block is");
            Serial.println("not this. Nothing above means anything.");
        } else if (n_read && n_zero + n_ff == n_read) {
            Serial.println("VERDICT: transfers complete but every byte is 0x00 or");
            Serial.println("0xFF, which is what an absent slave looks like. The");
            Serial.println("mailbox is alive; the chip behind it may not be.");
        } else if (n_read) {
            Serial.println("VERDICT: the file reads. Check the rail voltages above");
            Serial.println("against real supplies before believing any of it - that");
            Serial.println("is the part a wrong block cannot fake.");
        }
        step = STEP_IDLE;
        break;

    case STEP_IDLE:
    default:
        break;
    }
}
