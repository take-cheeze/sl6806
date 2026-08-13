/*
 * DvpProbe - hunt the functional clock enable at 0x400E0000.
 *
 * ===================================================================
 *  WHAT THE FIRST VERSION OF THIS SKETCH FOUND, AND WHAT IT MISSED
 * ===================================================================
 * Run on a P20 on 2026-08-13. `0x400E0000` bit 6 would not stick, and mask
 * ROM module id 46 made the DVP block at 0x400E1000 take and hold writes. So
 * the block came up awake, accepted a full geometry configuration, reported
 * CTRL 0x812 - and the sensor on the other end of it stayed silent, with
 * every DVP line flat under sixteen different MCLK settings.
 *
 * The camera works under the stock firmware on that same unit. So the module
 * is fitted, and every "is anything there" question is answered: yes. What is
 * missing is something the vendor does and this does not.
 *
 * ===================================================================
 *  THE PATTERN, AND IT IS NOT NEW
 * ===================================================================
 * The backlight got here first. 14a and 15: module id 68 makes the PWM's
 * registers writable, and its counter still does not run. Registers and
 * function are two different enables on this chip, and the module id only
 * buys the first.
 *
 * Read the camera the same way and it fits exactly. Module 46 bought the DVP
 * its register file; a block whose registers answer but whose logic is not
 * clocked would accept every geometry write, report them back, and never
 * drive MCLK - which is precisely what was measured. **The functional clock is
 * what 0x400E0000 carries**, and that register is dead from a payload.
 *
 * ===================================================================
 *  THE QUESTION THIS ASKS: WHY IS 0x400E0000 DEAD?
 * ===================================================================
 * A register that will not hold a bit is usually not a dead register - it is
 * a register in a block that is itself gated off. Nothing has ever tested
 * that, because every sweep here has used 0x400E0000 as a *mechanism* and
 * none has used it as a *witness*.
 *
 * So: walk the 128 mask ROM module ids and after each one ask whether
 * 0x400E0000 will hold bit 6. That is the same read-back discipline that
 * found the ADC (id 84, 15b) and the DVP's own register clock (id 46), turned
 * on the register that both the camera and the backlight are waiting behind.
 *
 * If some id makes it writable, two peripherals come unstuck at once. If none
 * does, that is a complete negative over every mechanism this chip is known
 * to have, and the answer is somewhere outside them.
 *
 * IT ALSO RETRIES THE OBVIOUS THING FIRST. The earlier run tested
 * 0x400E0000 *before* module 46 was enabled and never went back to it
 * afterwards. Enabling the block's register clock may be exactly what makes
 * its enable register answer, and that costs one line to check.
 *
 * ===================================================================
 *  WHAT IT WRITES
 * ===================================================================
 * The clock tree (request/ack at 0x40000070, the PLL at 0x40080008, the media
 * module clock at 0x4008011C) - all transcriptions, all the vendor's own
 * values. Module id 46, then each module id in turn during the walk. Bit 6 of
 * 0x400E0000, restored after every attempt. One DVP size register, restored.
 *
 * No power rail, no sensor. Payload mode never writes flash.
 *
 *     make SKETCH=examples/DvpProbe RUN_MODE=poll run
 */

#include <Arduino.h>
#include "sl6806_console.h"
#include "sl6806_dvp.h"
#include "sl6806_mmio.h"
#include "sl6806_module.h"
#include "sl6806_padctl.h"

/*
 * ===================================================================
 *  WHERE THIS STANDS, AND THE TWO THINGS LEFT TO TRY
 * ===================================================================
 * Measured with everything above in place - rail, register clock, functional
 * clock, PLL, media clock, camera module clock, geometry - and the DVP lines
 * sampled against the vendor's own pull-up: **all six read 1 and never move.**
 *
 * That is a real reading, unlike the two before it. A line held at 1 by a
 * pull-up is a line nothing is driving, so the sensor's outputs are in
 * high-impedance. The module is fitted and works under the stock firmware, so
 * a sensor with Hi-Z outputs is a sensor in power-down or one whose output
 * drivers are gated behind an internal clock it has not got.
 *
 * Two candidates remain, and they are cheap:
 *
 *   1. THE DIVIDER IS FOUR BITS AND ONLY FOUR VALUES WERE TRIED. CRU +0x118
 *      carries [11:8] as a divider - sixteen values - and the sweep so far
 *      covered 0..3 because that is what fits a source field. Divider 0 may
 *      well mean "stopped": it is the value the vendor passes, but the
 *      vendor's argument came out of a variable it had just tested for zero,
 *      which is exactly the kind of reading that has been wrong twice here.
 *
 *   2. MCLK MAY ONLY RUN WHILE THE BLOCK IS CAPTURING. The four registers the
 *      map turned up - +0x30, +0x34, +0x38, +0x3C - are an output path, and
 *      +0x38 rests at the SRAM base with [19:2] writable, which is a DMA
 *      destination. Nothing has ever told this block to start. If its sensor
 *      clock is gated on the capture engine running, then configuring
 *      geometry and walking away is precisely how to get a silent sensor.
 *
 * Both are tested by the same witness: do the sensor's own output pins move?
 */

/* The vendor's pull-up selector, and the pad word that selects input with it.
 * Bits [5:4] are drive, [3:0] the pull selector; see sl6806_padctl.h. */
#define PULL_VENDOR     8u
#define PAD_INPUT_PULLUP(id) \
    (((uint32_t)(id) & 0x000FF800u) | (3u << 4) | (PULL_VENDOR & 0x0Fu))

#define POLL_LIMIT      200000u
#define IDS_PER_PASS    4

/* A legal INSIZE no configuration would produce by accident. */
#define PROBE_VALUE     0x02A70159u

enum {
    STEP_DUMP,
    STEP_CLOCKTREE,
    STEP_MODULE46,
    STEP_RETRY,
    STEP_WALK,
    STEP_MAP,
    STEP_DIVIDER,
    STEP_CAPTURE,
    STEP_VERDICT,
    STEP_IDLE,
};

/* How many samples per line when watching for movement. */
#define SCOPE_SAMPLES   4000u

/*
 * A destination for the capture attempt: a static buffer in this payload's
 * own BSS. Using real memory the sketch owns means that if the block does
 * start writing, it writes somewhere harmless and the result is visible.
 * 4 KB is far less than a frame - the point is whether anything moves at
 * all, not to catch an image.
 */
static uint32_t capture_buf[1024];

/*
 * ===================================================================
 *  MAPPING THE BLOCK, BECAUSE THE KNOWN REGISTERS ARE NOT ENOUGH
 * ===================================================================
 * With registers, function clock, PLL, media clock, camera module clock and a
 * full geometry configuration all in place, the sensor still gets no MCLK.
 * Every register written so far came from two vendor routines - configure at
 * 0x00D9817C and open at 0x00D98224 - and between them they touch nine
 * offsets. A camera front end that produces a sensor clock has a divider for
 * it somewhere, and it is not in those nine.
 *
 * So map the block: write to every word from +0x00 to +0xFC, see which bits
 * stick, and restore. A register nobody has written that turns out to be
 * writable is where the missing control lives, and this is the only way to
 * find it without a datasheet.
 *
 * The risk is real but small and worth naming: writing ones into an unmapped
 * control register of a live block can start something. This block has no
 * destination address programmed and no sensor streaming into it, so there is
 * nothing for it to move. Every word is restored immediately, and the whole
 * thing is undone by an unplug.
 */
#define MAP_LAST        0xFCu
#define MAP_PER_PASS    8u

static uint8_t  step = STEP_DUMP;
static unsigned walk_id;
static unsigned map_off;
static unsigned map_found;       /* writable words outside the known nine */
static int      regs_awake;      /* the DVP holds writes                 */
static int      enable_awake;    /* 0x400E0000 holds bit 6               */
static int      won_id = -1;
static int      moved;           /* any DVP line ever toggled */

static int roomToPrint(void)
{
    return sl6806_console_space() >= (int)(SL6806_CONSOLE_SIZE / 2);
}

static void printHex8(uint32_t v)
{
    unsigned i;

    Serial.print("0x");
    for (i = 8; i-- > 0;)
        Serial.print((int)((v >> (i * 4)) & 0xF), HEX);
}

/*
 * The witness for both remaining experiments: are the sensor's own output
 * pins doing anything? Sampled against the vendor's pull-up, so
 *
 *     1, static   nothing drives it - the pull-up wins
 *     0, static   something off-chip holds it low
 *     toggling    the sensor is alive and clocked
 *
 * Returns the number of transitions seen, and prints a level per line.
 */
static unsigned scopeLines(void)
{
    static const uint32_t lines[] = {
        SL6806_DVP_PAD_SYNC0, SL6806_DVP_PAD_SYNC1, SL6806_DVP_PAD_SYNC2,
        SL6806_DVP_PAD_D0,    SL6806_DVP_PAD_D1,    SL6806_DVP_PAD_D7,
    };
    unsigned i, total = 0;

    for (i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        unsigned n, changes = 0;
        int last;

        sl6806_pad_configure(PAD_INPUT_PULLUP(lines[i]));
        delayMicroseconds(500);
        last = sl6806_pad_read(lines[i]);

        for (n = 0; n < SCOPE_SAMPLES; n++) {
            int now = sl6806_pad_read(lines[i]);

            if (now != last) {
                changes++;
                last = now;
            }
        }
        total += changes;
        Serial.print(sl6806_pad_read(lines[i]) ? "1" : "0");
        if (changes) {
            Serial.print("(");
            Serial.print(changes);
            Serial.print(") ");
        } else {
            Serial.print(" ");
        }
    }

    /* Put them back on the peripheral. */
    for (i = 0; i < sizeof(lines) / sizeof(lines[0]); i++)
        sl6806_pad_configure(lines[i]);

    Serial.println(total ? " <- MOVING" : " static");
    return total;
}

/* Witness 1: does the DVP block hold a written size? */
static int blockTakesWrites(void)
{
    uint32_t addr = SL6806_DVP_BASE + SL6806_DVP_INSIZE;
    uint32_t saved = sl6806_mmio_read(addr);
    uint32_t back;

    sl6806_mmio_write(addr, PROBE_VALUE);
    back = sl6806_mmio_read(addr);
    sl6806_mmio_write(addr, saved);
    return back == PROBE_VALUE;
}

/*
 * Witness 2, and the one this sketch is now about: does the peripheral enable
 * register hold the camera's bit? Restored either way - leaving a functional
 * clock on for a block nobody has configured is not something to do by
 * accident.
 */
static int enableTakesWrites(void)
{
    uint32_t saved = sl6806_mmio_read(SL6806_DVP_PERIPH_ENABLE);
    uint32_t bit = 1u << SL6806_DVP_PERIPH_ID;
    uint32_t back;

    sl6806_mmio_write(SL6806_DVP_PERIPH_ENABLE, saved | bit);
    back = sl6806_mmio_read(SL6806_DVP_PERIPH_ENABLE);
    sl6806_mmio_write(SL6806_DVP_PERIPH_ENABLE, saved);
    return (back & bit) != 0;
}

/* [V] 0x00D9A7FC and 0x00D9A7AC - see sl6806_dvp.h. */
static void clockTree(void)
{
    unsigned b, n;
    uint32_t v;

    Serial.print("  request/ack 0x40000070 ");
    printHex8(sl6806_mmio_read(SL6806_REQ_ACK));

    for (b = 0; b < SL6806_REQ_ACK_COUNT; b++) {
        uint32_t ack = 1u << (b + SL6806_REQ_ACK_SHIFT);

        if (sl6806_mmio_read(SL6806_REQ_ACK) & ack)
            continue;
        sl6806_mmio_set(SL6806_REQ_ACK, 1u << b);
        for (n = POLL_LIMIT; n; n--)
            if (sl6806_mmio_read(SL6806_REQ_ACK) & ack)
                break;
    }
    Serial.print(" -> ");
    printHex8(sl6806_mmio_read(SL6806_REQ_ACK));
    Serial.println();

    v = sl6806_mmio_read(SL6806_CRU_PLL);
    Serial.print("  PLL 0x40080008 ");
    printHex8(v);

    if (!(v & SL6806_CRU_PLL_LOCKED)) {
        sl6806_mmio_write(SL6806_CRU_PLL, SL6806_CRU_PLL_VALUE);
        for (n = POLL_LIMIT; n; n--)
            if (sl6806_mmio_read(SL6806_CRU_PLL) & SL6806_CRU_PLL_LOCKED)
                break;
        if (n)
            sl6806_mmio_set(SL6806_CRU_PLL, SL6806_CRU_PLL_RELEASE);
    }
    Serial.print(" -> ");
    printHex8(sl6806_mmio_read(SL6806_CRU_PLL));
    Serial.println((sl6806_mmio_read(SL6806_CRU_PLL) & SL6806_CRU_PLL_LOCKED)
                       ? "  locked" : "  NOT LOCKED");

    delay(10);
    sl6806_mmio_write(SL6806_CRU_MEDIA_CLOCK, SL6806_CRU_MEDIA_VALUE);
    delay(10);

    /* [V] 0x00D44688, source 0 divider 0 - the camera's own module clock. */
    v = sl6806_mmio_read(SL6806_CRU_CAM_CLOCK);
    v &= ~(uint32_t)(SL6806_CRU_CAM_SRC_MASK | SL6806_CRU_CAM_DIV_MASK);
    sl6806_mmio_write(SL6806_CRU_CAM_CLOCK, v);
    sl6806_mmio_write(SL6806_CRU_CAM_CLOCK,
                      sl6806_mmio_read(SL6806_CRU_CAM_CLOCK)
                          | SL6806_CRU_CAM_ENABLE);

    Serial.print("  media 0x4008011C ");
    printHex8(sl6806_mmio_read(SL6806_CRU_MEDIA_CLOCK));
    Serial.print(", camera 0x40080118 ");
    printHex8(sl6806_mmio_read(SL6806_CRU_CAM_CLOCK));
    Serial.println();
}

void setup()
{
    Serial.begin(115200);
    Serial.println();
    Serial.println("=== SL6806 camera front end: the functional clock ===");
    Serial.println("The module is fitted and works under the stock firmware, and");
    Serial.println("module id 46 already makes the DVP's registers writable - yet");
    Serial.println("it drives no MCLK. That is the backlight's problem exactly:");
    Serial.println("registers and function are two different enables, and the");
    Serial.println("second one lives in 0x400E0000, which will not hold a bit.");
    Serial.println();
    Serial.println("So this asks what gates 0x400E0000 itself.");
    Serial.println();
}

void loop()
{
    if (!roomToPrint())
        return;

    switch (step) {

    case STEP_DUMP:
        Serial.println("as found:");
        Serial.print("  0x400E0000 = ");
        printHex8(sl6806_mmio_read(SL6806_DVP_PERIPH_ENABLE));
        Serial.print("   0x400E0008 = ");
        printHex8(sl6806_mmio_read(SL6806_DVP_PERIPH_RESET));
        Serial.println();
        Serial.print("  DVP holds writes: ");
        Serial.print(blockTakesWrites() ? "yes" : "no");
        Serial.print("   0x400E0000 holds bit 6: ");
        Serial.println(enableTakesWrites() ? "yes" : "no");
        Serial.println();
        step = STEP_CLOCKTREE;
        break;

    case STEP_CLOCKTREE:
        Serial.println("1. the vendor's clock tree:");
        clockTree();
        Serial.print("  -> 0x400E0000 holds bit 6: ");
        Serial.println((enable_awake = enableTakesWrites()) ? "YES" : "no");
        step = STEP_MODULE46;
        break;

    case STEP_MODULE46:
        Serial.println();
        Serial.println("2. + module id 46, the DVP's register clock:");
        if (!sl6806_module_enable(SL6806_DVP_MODULE_ID))
            Serial.println("  id 46 did not acknowledge its gate.");
        regs_awake = blockTakesWrites();
        Serial.print("  DVP holds writes: ");
        Serial.println(regs_awake ? "yes - as before" : "no");
        step = STEP_RETRY;
        break;

    case STEP_RETRY:
        /*
         * The line the last run never got to. If a block's own register clock
         * is what its enable register was waiting for, this is where it shows
         * up, and it costs one test.
         */
        Serial.println();
        Serial.println("3. and now, with the block's registers alive, retry the");
        Serial.println("   peripheral enable that would not stick before:");
        enable_awake = enableTakesWrites();
        Serial.print("  0x400E0000 holds bit 6: ");
        Serial.println(enable_awake ? "YES - that was it" : "still no");

        if (enable_awake) {
            uint32_t bit = 1u << SL6806_DVP_PERIPH_ID;

            /* Leave the camera in the state CameraDemo wants to find, then
             * map the block while it is fully alive. */
            sl6806_mmio_set(SL6806_PERIPH_ENABLE_REG, bit);
            delay(10);
            sl6806_mmio_clr(SL6806_PERIPH_RESET_REG, bit);
            delay(10);
            sl6806_mmio_set(SL6806_PERIPH_RESET_REG, bit);
            delay(10);

            Serial.println();
            Serial.println("4. the block is fully enabled and still drives no MCLK, so");
            Serial.println("   map it: which words take writes? The nine offsets the");
            Serial.println("   vendor's two routines touch are not all of it, and a");
            Serial.println("   sensor-clock divider has to live somewhere.");
            Serial.println();
            Serial.println("   off   as found   after writing ones   writable bits");
            map_off = 0;
            step = STEP_MAP;
            break;
        }

        Serial.println();
        Serial.println("4. walking the 128 module ids, with 0x400E0000 itself as");
        Serial.println("   the witness this time - which no sweep here has done:");
        walk_id = 0;
        step = STEP_WALK;
        break;

    case STEP_WALK: {
        unsigned n;

        for (n = 0; n < IDS_PER_PASS && walk_id < 128; n++, walk_id++) {
            if (!sl6806_module_enable(walk_id))
                continue;
            if (!enableTakesWrites())
                continue;

            enable_awake = 1;
            won_id = (int)walk_id;
            Serial.print("   id ");
            Serial.print(walk_id);
            Serial.println(" MADE 0x400E0000 WRITABLE.");
            step = STEP_VERDICT;
            return;
        }

        if (walk_id >= 128) {
            Serial.println("   ...128 ids, and it still will not hold a bit.");
            step = STEP_VERDICT;
        } else if ((walk_id & 0x1F) == 0) {
            Serial.print("   ");
            Serial.print(walk_id);
            Serial.println("/128");
        }
        break;
    }

    case STEP_MAP: {
        unsigned n;

        /* The offsets the vendor's configure and open routines write, so a
         * writable word that is NOT one of these is the interesting kind. */
        static const uint8_t known[] = { 0x00, 0x04, 0x08, 0x10, 0x14,
                                         0x18, 0x1C, 0x20, 0x24, 0x40 };

        for (n = 0; n < MAP_PER_PASS && map_off <= MAP_LAST; n++, map_off += 4) {
            uint32_t addr = SL6806_DVP_BASE + map_off;
            uint32_t saved = sl6806_mmio_read(addr);
            uint32_t back, stuck;
            unsigned k;
            int is_known = 0;

            sl6806_mmio_write(addr, 0xFFFFFFFFu);
            back = sl6806_mmio_read(addr);
            sl6806_mmio_write(addr, saved);

            stuck = back & ~saved;          /* bits that went 0 -> 1 */
            if (!stuck && back == saved)
                continue;                   /* read-only or absent   */

            for (k = 0; k < sizeof(known); k++)
                if (known[k] == map_off)
                    is_known = 1;

            Serial.print("   +0x");
            if (map_off < 0x10)
                Serial.print('0');
            Serial.print(map_off, HEX);
            Serial.print("  ");
            printHex8(saved);
            Serial.print("   ");
            printHex8(back);
            Serial.print("   ");
            printHex8(stuck);
            Serial.println(is_known ? "  (known)" : "  <- NOT in the vendor's nine");
            if (!is_known)
                map_found++;
        }

        if (map_off > MAP_LAST) {
            Serial.println();
            Serial.println("5. the divider is four bits, and only four values have ever");
            Serial.println("   been tried. Sixteen dividers x four sources, watching the");
            Serial.println("   sensor's own pins against a pull-up:");
            map_off = 0;                    /* reused as the sweep index */
            step = STEP_DIVIDER;
        }
        break;
    }

    case STEP_DIVIDER: {
        unsigned src = map_off >> 4;
        unsigned div = map_off & 0x0F;
        uint32_t v;

        v = sl6806_mmio_read(SL6806_CRU_CAM_CLOCK);
        v &= ~(uint32_t)(SL6806_CRU_CAM_SRC_MASK | SL6806_CRU_CAM_DIV_MASK);
        v |= (src << 4) | (div << 8);
        sl6806_mmio_write(SL6806_CRU_CAM_CLOCK, v);
        sl6806_mmio_write(SL6806_CRU_CAM_CLOCK,
                          sl6806_mmio_read(SL6806_CRU_CAM_CLOCK)
                              | SL6806_CRU_CAM_ENABLE);
        sl6806_pad_configure(SL6806_DVP_PAD_MCLK);
        delay(20);

        Serial.print("   src ");
        Serial.print(src);
        Serial.print(" div ");
        if (div < 10)
            Serial.print(' ');
        Serial.print(div);
        Serial.print(": ");
        if (scopeLines())
            moved = 1;

        if (++map_off >= 64 || moved) {
            /* Back to the vendor's own setting before anything else. */
            v = sl6806_mmio_read(SL6806_CRU_CAM_CLOCK);
            v &= ~(uint32_t)(SL6806_CRU_CAM_SRC_MASK | SL6806_CRU_CAM_DIV_MASK);
            sl6806_mmio_write(SL6806_CRU_CAM_CLOCK, v | SL6806_CRU_CAM_ENABLE);
            Serial.println();
            Serial.println("6. and the other candidate: MCLK may only run while the");
            Serial.println("   block is capturing. Point the DMA at a buffer this");
            Serial.println("   payload owns and try the control bits:");
            map_off = 0;
            step = STEP_CAPTURE;
        }
        break;
    }

    case STEP_CAPTURE: {
        /* Bits 0, 1 and 3 are the only ones +0x30 accepts. Try each alone
         * and then together - four attempts, and the last is all of them. */
        static const uint32_t ctrl[] = { 0x1, 0x2, 0x8, 0xB };
        uint32_t base = (uint32_t)(uintptr_t)capture_buf;

        if (map_off == 0) {
            sl6806_mmio_write(SL6806_DVP_BASE + SL6806_DVP_DMA_ADDR, base);
            sl6806_mmio_write(SL6806_DVP_BASE + SL6806_DVP_DMA_LEN24,
                              sizeof(capture_buf));
            sl6806_mmio_write(SL6806_DVP_BASE + SL6806_DVP_DMA_LEN20,
                              sizeof(capture_buf));
            Serial.print("   buffer at ");
            printHex8(base);
            Serial.print(", +0x38 reads back ");
            printHex8(sl6806_mmio_read(SL6806_DVP_BASE + SL6806_DVP_DMA_ADDR));
            Serial.println();
        }

        sl6806_mmio_write(SL6806_DVP_BASE + SL6806_DVP_DMA_CTRL, ctrl[map_off]);
        delay(20);

        Serial.print("   +0x30 = 0x");
        Serial.print(ctrl[map_off], HEX);
        Serial.print(": ");
        if (scopeLines())
            moved = 1;

        if (++map_off >= (unsigned)(sizeof(ctrl) / sizeof(ctrl[0])) || moved) {
            sl6806_mmio_write(SL6806_DVP_BASE + SL6806_DVP_DMA_CTRL, 0);
            step = STEP_VERDICT;
        }
        break;
    }

    case STEP_VERDICT:
        Serial.println();
        if (enable_awake) {
            Serial.println("=== 0x400E0000 IS REACHABLE ===");
            if (won_id >= 0) {
                Serial.print("What did it: module id ");
                Serial.println(won_id);
            }

            Serial.print("0x400E0000 = ");
            printHex8(sl6806_mmio_read(SL6806_DVP_PERIPH_ENABLE));
            Serial.print("   0x400E0008 = ");
            printHex8(sl6806_mmio_read(SL6806_DVP_PERIPH_RESET));
            Serial.println();
            Serial.println();

            if (map_found) {
                Serial.print(map_found);
                Serial.println(" writable words sit outside the ten the vendor's");
                Serial.println("own routines touch. Those are the candidates for");
                Serial.println("whatever divides MCLK, and no disassembly in this");
                Serial.println("project has ever named them.");
            } else {
                Serial.println("Every writable word in the block is one the vendor");
                Serial.println("already writes. There is no undiscovered control");
                Serial.println("here, so MCLK is owned by something else entirely.");
            }
            Serial.println();
            Serial.println("This is the wall the backlight has been behind since 14a.");
            Serial.println("If it is the functional clock, the PWM counter should run");
            Serial.println("now too - a second, independent confirmation from a");
            Serial.println("different peripheral, worth more than the camera itself.");
            Serial.println("examples/Backlight is the way to take it.");
        } else {
            Serial.println("=== NEGATIVE, OVER EVERY MECHANISM ===");
            Serial.println("0x400E0000 will not hold a bit after the clock tree, after");
            Serial.println("the DVP's own register clock, or after any of the 128 module");
            Serial.println("ids. Every enable this chip is known to have has been tried.");
            Serial.println();
            Serial.println("That is worth reporting as a result rather than a failure.");
            Serial.println("It says the register is gated by something outside the CRU,");
            Serial.println("the mask ROM's module space and 0x400F1000 - most likely");
            Serial.println("privileged to code the boot ROM has not handed control away");
            Serial.println("from, which would put both the camera and the backlight's");
            Serial.println("dimming out of a payload's reach for a structural reason");
            Serial.println("rather than an undiscovered one.");
        }
        step = STEP_IDLE;
        break;

    case STEP_IDLE:
    default:
        break;
    }
}
