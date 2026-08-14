/*
 * PwmMode - sweep the two register fields the vendor writes and this
 * framework never has, looking for the one that starts the counter.
 *
 * WHERE THIS PICKS UP. §32 established that romclk 39 is the PWM's clock out
 * of the vendor's own 0x00D99C34, enabled it, and measured the result: the
 * counter still does not run. Three sessions, five configurations of the
 * clock and the pair register's force bit, and sl6806_pwm_commit() reports
 * 0/4 in every one of them - a commit cannot retire without the counter
 * reaching a period boundary, so nothing is turning. The panel tracks the
 * pair register's bit 8 alone.
 *
 * WHAT IS LEFT. Two fields, and the case for them is that they are what the
 * vendor writes and we do not:
 *
 *   MODE [3:1], via 0x00811E9A:
 *       [chan+0x10] = (x << 16) | (mode << 1) | enable
 *   and it clears bit 0 first when [3:1] changes, which is the shape of a
 *   field you may not reconfigure while running - i.e. one that selects what
 *   the counter DOES. sl6806_pwm_enable() sets bit 0 and leaves mode at 0.
 *   If 0 is idle or hold-the-output, that accounts for every measurement so
 *   far on its own. This is the leading suspect.
 *
 *   CTRL [19:16], via 0x00811ED0: four single bits, sixteen values.
 *
 * Eight modes times sixteen control values is 128 combinations, and a third
 * phase loads +0x14/+0x18/+0x1C - three more registers the vendor writes and
 * we leave at zero, which next to period/duty look like further compares. A
 * compare of zero is a plausible way to get a counter that does not move.
 *
 * THE DETECTOR IS sl6806_pwm_commit(), NOT CTRL BIT 28.
 *
 * Bit 28 is why this section has been wrong twice. It responds to romclk 39
 * and to nothing else useful, it is not even a deterministic function of the
 * clock register - configuration D produced it both set and clear with an
 * identical 0x400800F4 - and the panel is DARK in one of the configurations
 * where it reads "running". A commit retiring requires a period boundary to
 * actually happen. That is the only question worth asking, so it is the only
 * one this sketch scores on. Bit 28 is printed for the record and ignored.
 *
 * THE PAIR FORCE BIT IS LEFT CLEAR throughout, so the pad is not held at a
 * level and a hit can be confirmed by eye. That means the panel is DARK for
 * the whole sweep, which is expected and is not the result.
 *
 * ON A HIT the sweep stops, prints the combination, and runs a duty ramp so
 * you can look at the panel. Press 'c' to carry on from there.
 *
 *     make SKETCH=examples/PwmMode RUN_MODE=poll run
 *
 * TWO loop() CALLS PER COMBINATION, and the split is load-bearing. The first
 * version of this sweep wrote `due = millis() + 20` and then ran the commits
 * immediately - `due` schedules the NEXT call, it does not pause this one -
 * so all 168 rows were measured microseconds after the clock was enabled,
 * inside its settling window. Every row came back with CTRL bit 28 still set,
 * and 168 clean-looking negatives were worth nothing. The wait is now a state
 * that polls until bit 28 clears, and a row whose clock never settles is
 * printed as `?? NOCLK` and counted as a hole rather than a negative.
 *
 * Nothing blocks: setup() only prints. examples/PwmClock's second session
 * wedged the link by doing its work in setup(), and startup_payload.c
 * explains why that is fatal in poll mode.
 */

#include <Arduino.h>
#include "sl6806_pwm.h"
#include "sl6806_module.h"
#include "sl6806_mmio.h"
#include "sl6806_padctl.h"

#define CH 3

/* Phase 3's loads for +0x14/+0x18/+0x1C. Zero is what this driver has always
 * written; the rest are guesses shaped like period and duty, on the theory
 * that a compare register sitting at zero is what stops the counter. */
static const uint32_t aux_sets[][3] = {
    { 0x00000000u, 0x00000000u, 0x00000000u },   /* the current state */
    { 0xFFFFFFFFu, 0x0000FFFFu, 0xFFFFFFFFu },   /* everything wide open */
    { (48000u << 16) | 24000u, 48000u, (48000u << 16) | 24000u },
    { (24000u << 16) | 48000u, 24000u, (24000u << 16) | 48000u },
    { 0x00010001u, 0x00000001u, 0x00010001u },
};
#define NAUX ((int)(sizeof aux_sets / sizeof aux_sets[0]))

/* 168 rows, 160 distinct: phase 3's aux == 0 repeats phase 1's ctrl_hi == 0
 * rows on purpose, as a control inside the second phase. If the two ever
 * disagree, something is leaking between combinations. */
#define NMODE 8
#define NCTRL 16
#define PHASE12_COMBOS (NMODE * NCTRL)
#define TOTAL          (PHASE12_COMBOS + NAUX * NMODE)

static int      idx;            /* 0..TOTAL-1, -1 when idle */
static int      hits, unsettled;
static bool     paused;
static uint32_t due;
static int      ramp_i = -1;

/* Per-row state, split across two loop() calls so the settle is a real wait
 * and not a comment. */
static bool     settling, settled;
static uint32_t settle_deadline;
static unsigned mode_now, ctrl_now;
static int      aux_now;

/*
 * Apply one combination. Everything is written from scratch every time -
 * CTRL and MODE are stored, not OR'd - because 0x00811ED0 and this sweep
 * both only ever set bits, and a leaked bit from the previous combination
 * would make a hit unattributable. §32 already lost a reading to exactly
 * that: the clock helper only ORs, so once bit 4 was set it stayed set.
 */
static void apply(int i, unsigned *mode_out, unsigned *ctrl_out, int *aux_out)
{
    uint32_t chan = SL6806_PWM_CHAN(CH);
    unsigned mode, ctrl_hi;
    int aux;

    if (i < PHASE12_COMBOS) {
        mode    = (unsigned)(i / NCTRL);
        ctrl_hi = (unsigned)(i % NCTRL);
        aux     = 0;
    } else {
        int j   = i - PHASE12_COMBOS;
        mode    = (unsigned)(j % NMODE);
        ctrl_hi = 0;
        aux     = j / NMODE;
    }

    /* The baseline, every time, so a warm chip is reconfigured rather than
     * skipped - uploading a payload does not reset the clock tree. */
    sl6806_module_enable(SL6806_PWM_MODULE_ID);
    sl6806_mmio_clr(SL6806_PWM_CLK_REG,
                    SL6806_PWM_CLK_ENABLE | SL6806_PWM_CLK_SRC_HIGH);
    sl6806_pwm_clock_begin(0);
    sl6806_pad_configure(SL6806_PWM_BL_PAD);

    /* The force bit stays clear: it drives the pad regardless of the counter
     * and would make every row look alike on the panel. */
    sl6806_mmio_clr(SL6806_PWM_PAIR(CH), SL6806_PWM_PAIR_FORCE);

    /*
     * The vendor's order, because order has mattered at every step of this
     * investigation and there is no reason to introduce a difference here.
     * 0x00D99C34 writes CTRL - a bare 0x40, then the control word OR'd on -
     * and only then does 0x00811E08 write +0x14, +0x18, MODE and +0x1C.
     *
     * [?] Where 0x00811ED0's CTRL[19:16] belongs in that order is unknown;
     * nothing calls it. It goes in with the rest of CTRL.
     */
    sl6806_mmio_write(chan + SL6806_PWM_CTRL, SL6806_PWM_CTRL_INIT);
    sl6806_mmio_write(chan + SL6806_PWM_CTRL,
                      SL6806_PWM_CTRL_INIT | 0x3Fu | SL6806_PWM_CTRL_HI(ctrl_hi));

    sl6806_mmio_write(chan + SL6806_PWM_REG14, aux_sets[aux][0]);
    sl6806_mmio_write(chan + SL6806_PWM_REG18, aux_sets[aux][1]);
    sl6806_mmio_write(chan + SL6806_PWM_REG1C, aux_sets[aux][2]);

    sl6806_pwm_set(CH, SL6806_PWM_BL_PERIOD, SL6806_PWM_BL_DUTY(50));

    /* MODE: bit 0 down first, as 0x00811E9A does when [3:1] changes, then the
     * field and the enable together. */
    sl6806_mmio_write(chan + SL6806_PWM_MODE, 0);
    sl6806_mmio_write(chan + SL6806_PWM_MODE,
                      SL6806_PWM_MODE_FIELD(mode) | SL6806_PWM_MODE_ENABLE);

    *mode_out = mode;
    *ctrl_out = ctrl_hi;
    *aux_out  = aux;
}

static void report(int i, unsigned mode, unsigned ctrl_hi, int aux,
                   int commits, uint32_t ctrl, bool ok)
{
    Serial.printf("%s %3d/%d mode %u ctrl[19:16] %x aux %d -> commits %d/4"
                  "  CTRL %08x  clock %s\n",
                  commits ? ">>> HIT" : (ok ? "       " : "?? NOCLK"),
                  i + 1, TOTAL, mode, ctrl_hi, aux, commits,
                  (unsigned)ctrl,
                  ok ? "settled" : "NEVER SETTLED - row is not evidence");
}

void setup()
{
    Serial.begin(115200);
    Serial.println();
    Serial.println("=== SL6806 PWM: sweeping MODE[3:1] and CTRL[19:16] ===");
    Serial.printf("%d combinations, detector is sl6806_pwm_commit()\n", TOTAL);
    Serial.println("the panel stays DARK throughout - the force bit is off");
    Serial.println("'s' start/restart, 'c' continue after a hit, 'q' stop");
}

void loop()
{
    int key = Serial.read();
    int commits = 0;
    uint32_t ctrl;

    if (key == 's') {
        idx = 0; hits = unsettled = 0; paused = false; ramp_i = -1;
        settling = false; due = millis();
        Serial.println("\nsweeping...");
    } else if (key == 'c') {
        paused = false; ramp_i = -1;
    } else if (key == 'q') {
        idx = -1; ramp_i = -1;
        Serial.println("stopped.");
    }

    if (idx < 0 || paused || (int32_t)(millis() - due) < 0)
        return;

    /* A hit's confirmation ramp, one duty step per call. */
    if (ramp_i >= 0) {
        sl6806_backlight_set((unsigned)((ramp_i % 20) * 5));
        if (++ramp_i > 80) {
            ramp_i = -1;
            paused = true;
            Serial.println("     ramp done. 'c' to continue the sweep.");
        } else {
            due = millis() + 40;
        }
        return;
    }

    if (idx >= TOTAL) {
        Serial.printf("\ndone. %d hit(s) in %d combinations, %d row(s) whose"
                      " clock never settled.\n", hits, TOTAL, unsettled);
        if (unsettled)
            Serial.println("Rows that never settled are holes, not negatives.");
        if (!hits && !unsettled) {
            Serial.println("Neither field starts it. The counter's problem is");
            Serial.println("not in MODE[3:1], CTRL[19:16] or +0x14/18/1C.");
        }
        idx = -1;
        return;
    }

    /*
     * TWO PHASES PER ROW, AND THE SECOND ONE IS THE POINT.
     *
     * [!] The first version of this sweep did `due = millis() + 20` and then
     * ran the commits immediately - `due` schedules the NEXT loop() call, it
     * does not pause this one. So all 168 rows were measured microseconds
     * after the clock was enabled, inside its settling window, and every row
     * came back with CTRL bit 28 still set. 168 clean-looking negatives, all
     * void. The comment claimed a wait the code did not perform.
     *
     * So the wait is now a state, and it is verified rather than assumed:
     * poll until bit 28 clears, and if it never does, say so on the row. A
     * row whose clock never settled is not evidence about MODE or CTRL.
     */
    if (settling) {
        uint32_t c = sl6806_mmio_read(SL6806_PWM_CHAN(CH) + SL6806_PWM_CTRL);

        if (!(c & SL6806_PWM_CTRL_NOCLK)) {
            settled = true;
        } else if ((int32_t)(millis() - settle_deadline) < 0) {
            return;                      /* keep waiting, without blocking */
        }
        settling = false;

        for (int k = 0; k < 4; k++)
            commits += sl6806_pwm_commit(CH);

        ctrl = sl6806_mmio_read(SL6806_PWM_CHAN(CH) + SL6806_PWM_CTRL);

        /* Print hits always; otherwise one line every sixteen, so the log
         * stays readable but a stall is still visible. A row that never
         * settled is printed too - it is a hole in the sweep, not a
         * negative. */
        if (commits || !settled || (idx % 16) == 0)
            report(idx, mode_now, ctrl_now, aux_now, commits, ctrl, settled);

        if (!settled)
            unsettled++;

        if (commits) {
            hits++;
            Serial.println("     ramping duty 0 -> 100 -> 0 twice; WATCH THE PANEL");
            ramp_i = 0;
        }

        idx++;
        return;
    }

    apply(idx, &mode_now, &ctrl_now, &aux_now);

    /* Wait for the clock to actually arrive. The fourth PwmClock session
     * measured bit 28 settling near the 20 ms mark and racing either side of
     * it, so give it well more than that and confirm rather than hope. */
    settling = true;
    settled  = false;
    settle_deadline = millis() + 100;
}
