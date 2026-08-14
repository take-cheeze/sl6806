/*
 * PwmSrc - what feeds 0x400800F4, and are the fields PwmMode swept even real?
 *
 * TWO QUESTIONS, and the first one exists because of a gap in the second.
 *
 * 1. DID PwmMode ACTUALLY TEST ANYTHING? It swept MODE[3:1] and CTRL[19:16]
 *    against the commit witness, 168 rows, and came back empty with every row
 *    confirming its clock had settled. That is a clean negative ONLY IF those
 *    bits are writable. Nothing has ever read them back. If MODE[3:1] does not
 *    retain, the eight-mode sweep was one row run eight times, and the same
 *    for CTRL[19:16]. §14a's rule was "a write test is only a write test";
 *    this is its mirror, and it has not been applied.
 *
 * 2. IS THERE A CLOCK AT ALL BEHIND THE GATE? Everything measured so far is
 *    consistent with romclk 39 opening a gate onto a source that is not
 *    running: CTRL bit 28 clears, so the block sees a clock domain; nothing
 *    counts, no commit retires, no event fires. A gate with nothing behind it
 *    looks exactly like that.
 *
 *    [V] And the mask ROM has nothing more to offer on this clock. Its three
 *    families between them expose two operations on id 39 - romclk_enable
 *    sets 0x400800F4 bit 0 (ROM 0x2338) and clk_setsrc(39, 16) sets bit 4
 *    (ROM 0x3174). The divider setter, ROM 0x289C, dispatches id 39 to ROM
 *    0x2958, which is a bare `pop {r4, r5, r6, pc}` - a no-op. So if this
 *    register carries a divider or a second source select, NOTHING in the ROM,
 *    the bootloader or the application ever writes it, and it is sitting at
 *    its reset value. That is worth a census.
 *
 * WHAT THIS DOES, in three phases, one step per loop() call:
 *
 *   0. Census the PWM channel's own fields - MODE[3:1] and CTRL[19:16] -
 *      by writing them and reading back. Answers question 1 in four lines,
 *      and it is entirely safe: PWM registers only.
 *
 *   1. Bit-walk 0x400800F4. One bit per call, announced before it is written
 *      and restored immediately after, so an implemented-bit map falls out
 *      without ever holding an unknown CRU bit for longer than one step.
 *
 *   2. For every writable bit found other than bit 0, hold bit 0 plus that
 *      bit, wait for CTRL bit 28 to clear, and run the commit witness.
 *
 * ANNOUNCE BEFORE WRITING. Each CRU bit is printed before it is touched and
 * the call returns, so the host has drained that line before the write
 * happens. If a bit takes the device off the bus, the last line names it.
 * startup_payload.c asks for exactly this shape and examples/PadScope follows
 * it; this session has already lost one run to ignoring it.
 *
 *     make SKETCH=examples/PwmSrc RUN_MODE=poll run
 *
 * The panel stays dark throughout: the pair register's force bit is left
 * clear so the pad is not pinned and a hit is visible by eye.
 */

#include <Arduino.h>
#include "sl6806_pwm.h"
#include "sl6806_module.h"
#include "sl6806_mmio.h"
#include "sl6806_padctl.h"

#define CH 3

/* Bit 0 is the enable and is never walked - it is the thing being held. */
#define WALK_FIRST 1
#define WALK_LAST  31

enum { ST_IDLE, ST_CENSUS, ST_WALK_ANNOUNCE, ST_WALK_DO, ST_TRY, ST_TRY_WAIT };

static int      state = ST_IDLE;
static int      bit;              /* which CRU bit                    */
static uint32_t writable;         /* map built by phase 1             */
static int      try_bit;
static uint32_t settle_deadline;
static uint32_t due;

/* The baseline every phase-2 row starts from. */
static void baseline(void)
{
    uint32_t chan = SL6806_PWM_CHAN(CH);

    sl6806_module_enable(SL6806_PWM_MODULE_ID);
    sl6806_pad_configure(SL6806_PWM_BL_PAD);
    sl6806_mmio_clr(SL6806_PWM_PAIR(CH), SL6806_PWM_PAIR_FORCE);
    sl6806_mmio_write(chan + SL6806_PWM_CTRL, SL6806_PWM_CTRL_INIT);
    sl6806_mmio_write(chan + SL6806_PWM_CTRL, SL6806_PWM_CTRL_INIT | 0x3Fu);
    sl6806_pwm_set(CH, SL6806_PWM_BL_PERIOD, SL6806_PWM_BL_DUTY(50));
    sl6806_mmio_write(chan + SL6806_PWM_MODE, 0);
    sl6806_mmio_write(chan + SL6806_PWM_MODE, SL6806_PWM_MODE_ENABLE);
}

/*
 * Phase 0. Does a field retain what is written to it?
 *
 * Restores what it found, so running this on a configured block does not
 * disturb it.
 */
static void census_field(const char *name, uint32_t addr, uint32_t mask)
{
    uint32_t saved = sl6806_mmio_read(addr);
    uint32_t got;

    sl6806_mmio_write(addr, saved | mask);
    got = sl6806_mmio_read(addr) & mask;
    sl6806_mmio_write(addr, saved);

    Serial.printf("  %-14s wrote %08x  retained %08x  %s\n",
                  name, (unsigned)mask, (unsigned)got,
                  got == mask ? "fully writable"
                              : (got ? "PARTIAL" : "NOT WRITABLE"));
    if (got != mask)
        Serial.println("     ^ PwmMode's sweep of this field tested less than"
                       " it reported");
}

void setup()
{
    Serial.begin(115200);
    Serial.println();
    Serial.println("=== SL6806 PWM: is there a clock behind the gate? ===");
    Serial.printf("0x400800F4 on entry: %08x\n",
                  (unsigned)sl6806_mmio_read(SL6806_PWM_CLK_REG));
    Serial.println("'s' to start, 'q' to stop");
}

void loop()
{
    int key = Serial.read();
    uint32_t chan = SL6806_PWM_CHAN(CH);

    if (key == 's') {
        state = ST_CENSUS; bit = WALK_FIRST; writable = 0; due = millis();
    } else if (key == 'q') {
        state = ST_IDLE;
        Serial.println("stopped.");
    }

    if (state == ST_IDLE || (int32_t)(millis() - due) < 0)
        return;

    switch (state) {
    case ST_CENSUS:
        /* Phase 0. The block has to be clocked for its registers to answer,
         * so bring it up the way PwmMode did first. */
        sl6806_mmio_clr(SL6806_PWM_CLK_REG,
                        SL6806_PWM_CLK_ENABLE | SL6806_PWM_CLK_SRC_HIGH);
        sl6806_pwm_clock_begin(0);
        baseline();

        Serial.println("\nphase 0 - are the fields PwmMode swept writable?");
        census_field("MODE[3:1]", chan + SL6806_PWM_MODE, SL6806_PWM_MODE_MASK);
        census_field("CTRL[19:16]", chan + SL6806_PWM_CTRL, SL6806_PWM_CTRL_HI_MASK);
        census_field("PAIR", SL6806_PWM_PAIR(CH), 0xFFFFFFFFu);

        Serial.println("\nphase 1 - which bits of 0x400800F4 exist?");
        state = ST_WALK_ANNOUNCE;
        break;

    case ST_WALK_ANNOUNCE:
        if (bit > WALK_LAST) {
            Serial.printf("  writable map: %08x\n", (unsigned)writable);
            Serial.println("\nphase 2 - hold bit 0 plus each of them, and ask"
                           " the commit witness");
            try_bit = WALK_FIRST;
            state = ST_TRY;
            break;
        }
        /* Announced, then this call RETURNS - so the host has this line
         * before the write happens. */
        Serial.printf("  bit %2d ... ", bit);
        state = ST_WALK_DO;
        break;

    case ST_WALK_DO: {
        uint32_t saved = sl6806_mmio_read(SL6806_PWM_CLK_REG);
        uint32_t m = 1u << bit, got;

        sl6806_mmio_write(SL6806_PWM_CLK_REG, saved | m);
        got = sl6806_mmio_read(SL6806_PWM_CLK_REG) & m;
        sl6806_mmio_write(SL6806_PWM_CLK_REG, saved);

        if (got)
            writable |= m;
        Serial.println(got ? "writable" : "-");
        bit++;
        state = ST_WALK_ANNOUNCE;
        break;
    }

    case ST_TRY:
        while (try_bit <= WALK_LAST && !(writable & (1u << try_bit)))
            try_bit++;
        if (try_bit > WALK_LAST) {
            Serial.println("\ndone.");
            Serial.println("If nothing here starts it, the gate is open onto a");
            Serial.println("source that is not running, and the next place to");
            Serial.println("look is the vendor's clock init at 0x00806800 -");
            Serial.println("see docs/sl6806_re_notes.md 19, which called it a");
            Serial.println("reconfiguration rather than a bring-up.");
            state = ST_IDLE;
            break;
        }
        Serial.printf("  bit %2d + enable ... ", try_bit);
        sl6806_mmio_clr(SL6806_PWM_CLK_REG, 0xFFFFFFFFu);
        sl6806_mmio_set(SL6806_PWM_CLK_REG,
                        SL6806_PWM_CLK_ENABLE | (1u << try_bit));
        baseline();
        settle_deadline = millis() + 100;
        state = ST_TRY_WAIT;
        break;

    case ST_TRY_WAIT: {
        uint32_t c = sl6806_mmio_read(chan + SL6806_PWM_CTRL);
        bool settled = !(c & SL6806_PWM_CTRL_NOCLK);
        int commits = 0;

        if (!settled && (int32_t)(millis() - settle_deadline) < 0)
            return;

        for (int k = 0; k < 4; k++)
            commits += sl6806_pwm_commit(CH);

        Serial.printf("commits %d/4  CTRL %08x  clock %s%s\n",
                      commits,
                      (unsigned)sl6806_mmio_read(chan + SL6806_PWM_CTRL),
                      settled ? "settled" : "NEVER SETTLED",
                      commits ? "   >>> HIT" : "");
        try_bit++;
        state = ST_TRY;
        break;
    }

    default:
        state = ST_IDLE;
        break;
    }
}
