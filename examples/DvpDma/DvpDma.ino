/*
 * DvpDma - the +0x30..0x3C cluster's control register, watched over time.
 *
 * ===================================================================
 *  WHY THIS EXISTS SEPARATELY FROM CameraDemo
 * ===================================================================
 * CameraDemo triggers this cluster too, but it is a camera sketch: it ends
 * by sweeping all 56 of the ROM's other clock enables, and that sweep wedges
 * USB every time. One run per power cycle is no way to establish whether a
 * result reproduces, and the result below needed establishing.
 *
 * So this does the DVP bring-up, exercises the DMA cluster, and stops. It
 * touches no sensor, no I2C, no pad sweep and no clock it does not need, and
 * it repeats its measurement forever with a round counter, so the console
 * shows the same numbers coming back rather than one lucky reading.
 *
 * ===================================================================
 *  WHAT IT MEASURES, AND WHY THE BUFFER IS THE WRONG WITNESS
 * ===================================================================
 * sl6806_dvp.h mapped +0x30..0x3C by writing every bit of the live block and
 * reading back what stuck. Four registers, and the reading was: +0x38 a SRAM
 * pointer, +0x3C a length, +0x30 three control bits. All of it [I] except
 * the pointer - nothing had ever been run.
 *
 * The first hardware pass (2026-08-15) armed the descriptor and counted how
 * many bytes of the destination changed, the way AudioWall tests the audio
 * DMA. It counted zero, and that negative is worth very little here: the
 * sensor never starts, its outputs sit in Hi-Z, and a capture engine with no
 * pixel source has nothing to write even if it is in perfect health. The
 * buffer cannot distinguish a dead engine from a live one that was handed no
 * data.
 *
 * +0x30 itself can, and needs no source to do it. Write it, read it straight
 * back, then read it again after a delay with nothing else touching it:
 *
 *   unchanged      an inert latch - bits that store, and no logic behind them
 *   self-clears    hardware consumed the write. A latch cannot do this.
 *
 * That is the whole idea, and it is the same trick that found the audio
 * block's write-only TRIG bit after the register census had declared bit 0
 * unwritable.
 *
 * ===================================================================
 *  THE RESULT, AND IT IS NOT A LATCH
 * ===================================================================
 * [M] P20, 2026-08-15. All seven combinations, 107 rounds, across a power
 * cycle and two different destination buffers. Every round identical - not
 * one reading out of 749 differs:
 *
 *     write   reads back after 1 ms      what changed
 *     0x1  -> 0x0                        bit 0 cleared
 *     0x2  -> 0x2                        -
 *     0x8  -> 0x8                        -
 *     0x3  -> 0x0                        bits 0 and 1 cleared
 *     0x9  -> 0x9                        -
 *     0xA  -> 0xA                        -
 *     0xB  -> 0x9                        bit 1 cleared
 *
 * Bits that clear themselves are not storage, and which ones clear depends
 * on what the others were set to. Two rules cover all seven rows exactly:
 *
 *     bit 0 clears itself, UNLESS bit 3 is set - then it stays.
 *     bit 1 clears itself IF bit 0 is set - otherwise it stays.
 *
 * Every row follows from those two and nothing contradicts them. That reads
 * naturally as bit 0 being a one-shot "go" that hardware consumes, bit 3
 * holding it (a continuous or enable mode), and bit 1 a second command
 * consumed at the same moment the go is taken - but those names are [I] and
 * this sketch does not put them in the header. What is [M] is the two rules.
 *
 * The census could never have found this: it wrote bits and read them back
 * within the same breath, and every one of these values reads back correctly
 * at 0 ms. The change happens between 0 and 1 ms, so only a delayed second
 * read sees it.
 *
 * It does NOT establish that the block moves a byte, and this sketch says so
 * in its own output rather than letting the reader supply that conclusion.
 * What clears a bit might be a transfer completing, an engine refusing a
 * descriptor it does not like, or an interlock between two bits that has
 * nothing to do with data movement at all. Distinguishing those needs a
 * pixel source, and the sensor on this board has never started.
 *
 *     make SKETCH=examples/DvpDma RUN_MODE=poll run
 */

#include "sl6806_dvp.h"
#include "sl6806_mmio.h"
#include "sl6806_module.h"

/* The geometry write that proves the block is awake at all - any value the
 * register will hold does, and this is CameraDemo's. */
#define DVP_SIZE_WORD   0x02CF04FFu

#define POLL_LIMIT      100000u

/* Small on purpose. Nothing here waits for a transfer to finish; the buffer
 * is a control, and 256 bytes is enough to notice one being written. */
#define CAP_LEN         256u
static uint8_t cap_buf[CAP_LEN] __attribute__((aligned(4)));

/*
 * Every nonzero combination of +0x30's three writable bits. The census
 * (sl6806_dvp.h) says bits 0, 1 and 3 take writes and says nothing about
 * what any one of them does - so sweeping the combinations is the only way
 * to tell an interlock from a trigger, and the all-bits case that
 * sl6806_dvp_capture_start() uses is just one row of this table.
 */
static const uint32_t ctrl_combos[] = {
    0x1u, 0x2u, 0x8u, 0x3u, 0x9u, 0xAu, 0xBu,
};
#define N_COMBOS  (sizeof(ctrl_combos) / sizeof(ctrl_combos[0]))

static int dvp_awake;
static unsigned round_no;

enum { STEP_BRINGUP, STEP_WRITABLE, STEP_SWEEP, STEP_ROUND_END, STEP_DONE };
static int step = STEP_BRINGUP;
static unsigned combo;

static void printHex(uint32_t v)
{
    Serial.print("0x");
    Serial.print(v, HEX);
}

/*
 * The vendor's clock tree, as DvpProbe established it: the request/ack walk,
 * the PLL at +0x08, the media clock. Gates give a peripheral its registers;
 * they do not give it a clock, and this block needs both before +0x30 means
 * anything.
 */
static void clockTree(void)
{
    unsigned b, n;
    uint32_t v;

    for (b = 0; b < SL6806_REQ_ACK_COUNT; b++) {
        uint32_t ack = 1u << (b + SL6806_REQ_ACK_SHIFT);

        if (sl6806_mmio_read(SL6806_REQ_ACK) & ack)
            continue;
        sl6806_mmio_set(SL6806_REQ_ACK, 1u << b);
        for (n = POLL_LIMIT; n; n--)
            if (sl6806_mmio_read(SL6806_REQ_ACK) & ack)
                break;
    }

    v = sl6806_mmio_read(SL6806_CRU_PLL);
    if (!(v & SL6806_CRU_PLL_LOCKED)) {
        sl6806_mmio_write(SL6806_CRU_PLL, SL6806_CRU_PLL_VALUE);
        for (n = POLL_LIMIT; n; n--)
            if (sl6806_mmio_read(SL6806_CRU_PLL) & SL6806_CRU_PLL_LOCKED)
                break;
        if (n)
            sl6806_mmio_set(SL6806_CRU_PLL, SL6806_CRU_PLL_RELEASE);
    }
    Serial.print("  PLL ");
    printHex(sl6806_mmio_read(SL6806_CRU_PLL));
    Serial.println((sl6806_mmio_read(SL6806_CRU_PLL) & SL6806_CRU_PLL_LOCKED)
                       ? " (locked)" : " (NOT LOCKED)");

    delay(10);
    sl6806_mmio_write(SL6806_CRU_MEDIA_CLOCK, SL6806_CRU_MEDIA_VALUE);
    delay(10);

    v = sl6806_mmio_read(SL6806_CRU_CAM_CLOCK);
    v &= ~(uint32_t)(SL6806_CRU_CAM_SRC_MASK | SL6806_CRU_CAM_DIV_MASK);
    sl6806_mmio_write(SL6806_CRU_CAM_CLOCK, v);
    sl6806_mmio_write(SL6806_CRU_CAM_CLOCK,
                      sl6806_mmio_read(SL6806_CRU_CAM_CLOCK)
                          | SL6806_CRU_CAM_ENABLE);
}

static void bringUp(void)
{
    uint32_t v;

    Serial.println("bringing the front end up (no sensor, no I2C):");

    /* The sensor's own two clocks - kept because the block came up this way
     * when the result below was first measured, and dropping them would
     * change the conditions rather than simplify them. */
    sl6806_module_enable(SL6806_SENSOR_MODULE_ID);
    sl6806_mmio_set(SL6806_CRU_SENSOR_CLOCK, SL6806_CRU_SENSOR_CLOCK_EN);

    clockTree();

    if (!sl6806_module_enable(SL6806_DVP_MODULE_ID))
        Serial.println("  module id 46 did not acknowledge its gate.");

    if (sl6806_periph_enable(SL6806_DVP_PERIPH_ID)) {
        sl6806_periph_reset(SL6806_DVP_PERIPH_ID);
        Serial.print("  functional clock on, 0x400E0000 = ");
        printHex(sl6806_mmio_read(SL6806_PERIPH_ENABLE_REG));
        Serial.println();
    } else {
        Serial.println("  0x400E0000 would not hold bit 6 - registers but no");
        Serial.println("  logic clock. Everything below is then meaningless.");
    }

    sl6806_mmio_write(SL6806_DVP_BASE + SL6806_DVP_INSIZE, DVP_SIZE_WORD);
    v = sl6806_mmio_read(SL6806_DVP_BASE + SL6806_DVP_INSIZE);
    dvp_awake = (v == DVP_SIZE_WORD);

    Serial.print("  INSIZE reads back ");
    printHex(v);
    Serial.println(dvp_awake ? " - the block is awake." : " - NOT awake.");
    Serial.println();
}

/*
 * One combination: arm the descriptor with this trigger, then read +0x30
 * back at three separated times with nothing writing it in between. The
 * delays are what the first pass lacked - it read the buffer immediately and
 * concluded from that, the same mistake the audio contention test made
 * before it was sized to its window. 20 ms is inside poll mode's 50 ms
 * blocking cap and is a very long time in bus cycles.
 */
static void sweepOne(uint32_t bits)
{
    uint32_t c0, c1, c2, len_after;
    uint32_t want_addr;
    void *addr_after;
    unsigned i, changed = 0;

    memset(cap_buf, 0xAAu, sizeof(cap_buf));

    if (sl6806_dvp_capture_start_bits(cap_buf, CAP_LEN, bits) != 0) {
        Serial.println("  capture_start_bits() refused its own buffer - a bug");
        Serial.println("  in this sketch, not a reading.");
        return;
    }

    c0 = sl6806_dvp_capture_ctrl();
    delay(1);
    c1 = sl6806_dvp_capture_ctrl();
    delay(19);
    c2 = sl6806_dvp_capture_ctrl();

    addr_after = sl6806_dvp_capture_addr();
    len_after  = sl6806_dvp_capture_len();

    for (i = 0; i < sizeof(cap_buf); i++)
        if (cap_buf[i] != 0xAAu)
            changed++;

    Serial.print("  wrote ");
    printHex(bits);
    Serial.print(" -> ");
    printHex(c0);
    Serial.print(" @0ms, ");
    printHex(c1);
    Serial.print(" @1ms, ");
    printHex(c2);
    Serial.print(" @20ms | ADDR ");
    printHex((uint32_t)(uintptr_t)addr_after);
    Serial.print(" LEN ");
    printHex(len_after);
    Serial.print(" | ");
    Serial.print(changed);
    Serial.println(" bytes moved");

    if (c2 != bits) {
        Serial.print("      bits ");
        printHex(bits & ~c2);
        Serial.println(" cleared themselves - hardware wrote this register.");
    }

    want_addr = SL6806_DVP_SRAM_BASE
              | ((uint32_t)(uintptr_t)cap_buf & SL6806_DVP_DMA_ADDR_MASK);
    if ((uint32_t)(uintptr_t)addr_after != want_addr)
        Serial.println("      ADDR MOVED - a pointer that advances is an engine.");
    if (len_after != CAP_LEN)
        Serial.println("      LEN MOVED - a residual count, and real progress.");
    if (changed)
        Serial.println("      NONZERO. The buffer changed, which nothing in this"
                       " cluster had ever done.");

    sl6806_dvp_capture_stop();
}

void setup()
{
    Serial.begin(115200);
    Serial.println();
    Serial.println("=== SL6806 DVP DMA cluster: is +0x30 a latch? ===");
    Serial.println("Writes each combination of the three writable control bits");
    Serial.println("and reads the register back at 0, 1 and 20 ms. A value that");
    Serial.println("comes back changed was changed by hardware, which is a claim");
    Serial.println("the buffer cannot make: the sensor never starts on this");
    Serial.println("board, so a capture engine has no pixels to write and an");
    Serial.println("unchanged buffer proves nothing either way.");
    Serial.println();
}

void loop()
{
    switch (step) {

    case STEP_BRINGUP:
        bringUp();
        step = dvp_awake ? STEP_WRITABLE : STEP_DONE;
        break;

    case STEP_WRITABLE:
        Serial.print("descriptor registers hold a test pattern: ");
        Serial.println(sl6806_dvp_capture_writable() ? "yes" : "NO");
        Serial.print("+0x30 at rest, before any trigger: ");
        printHex(sl6806_dvp_capture_ctrl());
        Serial.println();
        combo = 0;
        step = STEP_SWEEP;
        break;

    case STEP_SWEEP:
        if (combo == 0) {
            Serial.println();
            Serial.print("round ");
            Serial.println(++round_no);
        }
        sweepOne(ctrl_combos[combo]);
        if (++combo >= N_COMBOS)
            step = STEP_ROUND_END;
        break;

    case STEP_ROUND_END:
        /* Straight back into another round. The point of this sketch is that
         * the numbers repeat, and a single round cannot show that. */
        combo = 0;
        step = STEP_SWEEP;
        break;

    case STEP_DONE:
        break;
    }
}
