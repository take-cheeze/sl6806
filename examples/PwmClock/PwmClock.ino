/*
 * PwmClock - romclk 39 does NOT start the PWM counter. [M] 2026-08-14.
 *
 * Three sessions on a P20 Player. The middle one said it did; that was wrong,
 * and this header is what the measurements actually support:
 *
 *      0x400800F4  bit0  bit4   pair bit8   CTRL b28   commits   PANEL
 *   A   0x00000000   0     0        1           1        0/4     bright
 *   B   0x00000001   1     0        0           0        0/4     DARK
 *   C   0x00000001   1     0        1           0        0/4     bright
 *   D   0x00000011   1     1        0           1        0/4     DARK
 *   E   0x00000011   1     1        1           1        0/4     bright
 *
 *   the panel tracks PAIR BIT 8, exactly and only
 *   the counter runs in NO configuration
 *
 * `commits` is the witness that settled it: retiring CTRL bit 8 needs the
 * counter to reach a period boundary, and it is 0/4 everywhere - including
 * both rows where bit 28 claims the block is running. With the interrupt
 * enable confirmed armed on both of channel 3's bits (`irqen 0808`) the
 * status at +0x04 also stayed at zero. Nothing counts, nothing retires,
 * nothing fires.
 *
 * WHAT WENT WRONG IN THE MIDDLE SESSION, because the shape of the error is
 * worth more than the result:
 *
 *   - CTRL bit 28 clears when romclk 39 is enabled. That is real, and it is
 *     the only register response this clock produces. It was read as "the
 *     counter is running" when it means something nearer "the block has a
 *     clock".
 *   - The second witness, "any word in the eight-word block changed", turned
 *     out to be bit 28's own settling edge. Two witnesses, one observation.
 *   - "The backlight brightness changed" was the panel going bright/dark as
 *     the configurations alternated pair bit 8 on and off - A bright, B dark,
 *     C bright, D dark, E bright. A blinking panel, read as a dimming one.
 *
 * The fix in all three cases is the same and is the reason this sketch now
 * reports `commits`: prefer a witness that observes an EFFECT over one that
 * asks the block how it feels.
 *
 * THE STATIC WORK BEHIND IT. 0x00D99C34 is the PWM channel init
 * - the routine §14a lifted the register layout out of - and the only thing
 * in the whole image that calls it is the driver's configure op. It opens
 * with THREE clock steps, not one:
 *
 *     if (!module_is_enabled(68)) module_enable(68);
 *     clk_setsrc(39, use_high ? 16 : 10);
 *     romclk_enable(39);
 *
 * Both of the last two land on 0x400800F4. ROM 0x2338 (romclk 39) sets bit 0;
 * ROM 0x3174 (clk id 39, source 16) sets bit 4, and source 10 - the branch
 * the backlight actually takes - does nothing at all. Clock id 39 appears
 * nowhere else in the image.
 *
 * §18 concluded "the PWM does not ask for a clock of its own" after scanning
 * the SRAM blob and stage 1. These two calls are in XIP, outside that scan.
 *
 * SO: module 68 makes the registers WRITABLE. romclk 39 is what CLOCKS THE
 * COUNTER BEHIND THEM. That is the shape of every "configured and not
 * running" block in this tree, and it is the first time the missing half has
 * been found in the vendor's code rather than guessed at.
 *
 * WHAT THIS SKETCH DOES. Five configurations, each reported with three
 * independent witnesses, so that a negative is a real negative:
 *
 *   A  clock off, pair bit 8 set      - today's driver, the known-lit state
 *   B  clock on,  pair bit 8 clear    - the vendor's configuration
 *   C  clock on,  pair bit 8 set      - both
 *   D  clock on + src_high, bit 8 clear
 *   E  clock on + src_high, bit 8 set
 *
 * C is what the driver ships: the clock on because the vendor does it, and
 * pair bit 8 because that is the backlight.
 *
 * THE WITNESSES, and how each actually did:
 *
 *   1. sl6806_pwm_commit(). [M] The only one that is mechanically distinct
 *      from bit 28, and the only one whose answer can be trusted. 0/4 in all
 *      five configurations.
 *   2. The interrupt status at 0x40084004. [M] Zero throughout, with the
 *      enable confirmed armed on both of the channel's bits. Consistent with
 *      a counter that never reaches a boundary.
 *   3. Any word in the channel block changing between two reads. [M] Only
 *      ever CTRL bit 28 settling. Reported, and labelled not independent.
 *   4. The +0x08 readback that 0x00811EF6 reads as two halfwords. [M] Flat
 *      zero everywhere. It is not a counter.
 *
 *   And CTRL bit 28, printed beside the commit verdict precisely so the two
 *   can be seen disagreeing - which is what they do in B and C, and which is
 *   the whole result.
 *
 * Everything is written unconditionally and nothing is skipped on "already
 * set": uploading a payload does not reset the clock tree, so a warm chip
 * that returns early looks exactly like a negative result. §14a lost a run
 * to that.
 *
 *     make SKETCH=examples/PwmClock RUN_MODE=poll run
 *
 * AND LOOK AT THE PANEL. The console cannot tell you whether it dimmed.
 */

#include <Arduino.h>
#include "sl6806_pwm.h"
#include "sl6806_module.h"
#include "sl6806_mmio.h"
#include "sl6806_padctl.h"
#include "wiring_time.h"

#define CH 3

struct config {
    const char *name;
    int         clock;      /* romclk 39                    */
    int         src_high;   /* 0x400800F4 bit 4             */
    int         pair_bit8;  /* the empirical one            */
};

static const struct config configs[] = {
    { "A  clock off, pair8 on   (today's driver)", 0, 0, 1 },
    { "B  clock ON,  pair8 off  (the vendor's)",   1, 0, 0 },
    { "C  clock ON,  pair8 on",                    1, 0, 1 },
    { "D  clock ON+high, pair8 off",               1, 1, 0 },
    { "E  clock ON+high, pair8 on",                1, 1, 1 },
};
#define NCONFIG ((int)(sizeof configs / sizeof configs[0]))

/* ------------------------------------------------------------------ */
/* Everything below runs one step per loop() call, and never blocks.   */
/*                                                                     */
/* [!] THE FIRST VERSION OF THIS SKETCH DID IT ALL IN setup(), AND     */
/* WEDGED THE DEVICE. In RUN_MODE=poll the ROM's USB handler is        */
/* installed at the bottom of _start, AFTER sl6806_run_setup() - so    */
/* while setup() runs, the host cannot poll at all. A setup() that     */
/* takes longer than the host's timeout on the run command abandons    */
/* the transaction mid-flight, and the device then answers nothing     */
/* until it is unplugged. Five configurations with a duty sweep each   */
/* is tens of seconds, which is far past it.                           */
/*                                                                     */
/* startup_payload.c says this in as many words, and says the fix: do  */
/* it from loop(), one step per call, announcing each step and         */
/* returning so the host drains that line BEFORE the risky write. The  */
/* payload itself was fine both times - the backlight swept on the run */
/* that lost the console. It is the link that cannot wait.             */
/* ------------------------------------------------------------------ */

enum {
    ST_IDLE,        /* nothing scheduled                          */
    ST_ANNOUNCE,    /* print the configuration's name, then yield */
    ST_BRINGUP,     /* apply it, and take the first snapshot      */
    ST_SNAP_B,      /* second snapshot, and the verdict           */
    ST_SWEEP,       /* one duty step per call                     */
};

static int      state = ST_IDLE;
static int      cfg;            /* which configuration            */
static int      last_cfg;       /* the last one to run, inclusive */
static int      sweep_i;        /* 0..SWEEP_STEPS                 */
static uint32_t due;            /* millis() at which to act       */
static uint32_t snap_a[8];

/* Two passes of 0->100->0 in 5% steps. Long enough not to be mistaken for a
 * flicker, and paced by millis() rather than delay() so no single loop() call
 * blocks. In poll mode this only advances while the monitor is polling, which
 * is the point: the link stays alive throughout. */
#define SWEEP_LEG    20                  /* 5% steps, so 20 per direction */
#define SWEEP_STEPS  (4 * SWEEP_LEG)     /* up, down, up, down            */
#define SWEEP_MS     40

static unsigned sweep_duty(int i)
{
    int leg = i % SWEEP_LEG;             /* 0..19 within one direction */

    return (unsigned)(((i / SWEEP_LEG) & 1) ? (100 - leg * 5) : leg * 5);
}

static void snapshot(uint32_t *out)
{
    uint32_t base = SL6806_PWM_CHAN(CH);

    for (int i = 0; i < 8; i++)
        out[i] = sl6806_mmio_read(base + i * 4);
}

/* The whole bring-up, open-coded so each configuration can vary a step.
 * sl6806_backlight_begin() cannot be used here: it always does the same
 * thing, which is the thing being tested. */
static void bring_up(const struct config *c)
{
    uint32_t chan = SL6806_PWM_CHAN(CH);
    uint32_t pair = SL6806_PWM_PAIR(CH);

    sl6806_module_enable(SL6806_PWM_MODULE_ID);

    /* [!] Clear BOTH bits first, always. sl6806_pwm_clock_begin() only ORs,
     * so a configuration that wants bit 4 clear cannot get it back once D or
     * E has set it - and the third session's log shows exactly that: press
     * 4 then 2, and B runs with cru+F4 = 0x11 and is stopped, which reads as
     * B being unreliable rather than as the sketch leaking state. Every
     * configuration must be reachable from any other. */
    sl6806_mmio_clr(SL6806_PWM_CLK_REG,
                    SL6806_PWM_CLK_ENABLE | SL6806_PWM_CLK_SRC_HIGH);
    if (c->clock)
        sl6806_pwm_clock_begin(c->src_high);

    sl6806_pad_configure(SL6806_PWM_BL_PAD);

    sl6806_mmio_write(chan + SL6806_PWM_CTRL, SL6806_PWM_CTRL_INIT);
    sl6806_mmio_set(chan + SL6806_PWM_CTRL, 0x3Fu);

    if (c->pair_bit8)
        sl6806_mmio_set(pair, SL6806_PWM_PAIR_FORCE);
    else
        sl6806_mmio_clr(pair, SL6806_PWM_PAIR_FORCE);

    sl6806_pwm_set(CH, SL6806_PWM_BL_PERIOD, SL6806_PWM_BL_DUTY(50));
    sl6806_pwm_enable(CH, 1);
    sl6806_pwm_run(CH, 1);

    /* Arming lets +0x04 latch. It writes only the PWM's own enable register
     * and never the NVIC, so no handler is needed - but it is the one write
     * here that could plausibly raise an interrupt line, so it goes last and
     * the console line above it has already been drained by the host. */
    sl6806_pwm_irq_arm(CH);
    sl6806_pwm_irq_fired();
}

static void verdict(void)
{
    uint32_t b[8];
    bool moved = false, only_ctrl = true;
    uint32_t irq = sl6806_pwm_irq_fired();
    int commits = 0;

    snapshot(b);
    for (int i = 0; i < 8; i++)
        if (snap_a[i] != b[i]) {
            moved = true;
            if (i != 0)
                only_ctrl = false;
        }

    /* THE ONE INDEPENDENT WITNESS. Bit 28 is a state flag - it reports what
     * the block believes about its own clock. A commit retiring requires the
     * counter to REACH A PERIOD BOUNDARY, which is a different question and
     * the one actually being asked. Four in a row, because one could retire
     * on a single settling edge. */
    for (int i = 0; i < 4; i++)
        commits += sl6806_pwm_commit(CH);

    Serial.printf("     +08 %04x/%04x   irqen %04x   irq %03x   commits %d/4\n",
                  sl6806_pwm_count(CH, 0), sl6806_pwm_count(CH, 1),
                  (unsigned)sl6806_mmio_read(SL6806_PWM_IRQ_ENABLE),
                  (unsigned)irq, commits);
    Serial.printf("     CTRL %08x  P/D %08x  pair %08x  cru+F4 %08x\n",
                  (unsigned)b[0], (unsigned)b[1],
                  (unsigned)sl6806_mmio_read(SL6806_PWM_PAIR(CH)),
                  (unsigned)sl6806_mmio_read(SL6806_PWM_CLK_REG));

    /* Name what moved. The first run reported only that something did, and
     * "something in an eight-word block" turned out to be CTRL bit 28
     * settling - the same bit the verdict line already reports, so the two
     * were never independent. Say so out loud rather than count it twice. */
    if (moved) {
        Serial.print("     moved:");
        for (int i = 0; i < 8; i++)
            if (snap_a[i] != b[i])
                Serial.printf(" +%02x %08x->%08x", i * 4,
                              (unsigned)snap_a[i], (unsigned)b[i]);
        Serial.println(only_ctrl ? "   (CTRL only - not independent)" : "");
    }

    /* Print the two verdicts separately so a disagreement is visible rather
     * than silently resolved in favour of whichever is read first. That is
     * the mistake this whole investigation was built on. */
    Serial.printf("     verdict: commits say %s, CTRL bit 28 says %s\n",
                  commits ? "RUNNING" : "stopped",
                  (b[0] & SL6806_PWM_CTRL_STOPPED) ? "stopped" : "RUNNING");
}

static void start(int first, int last)
{
    cfg = first;
    last_cfg = last;
    state = ST_ANNOUNCE;
    due = millis();
}

void setup()
{
    Serial.begin(115200);
    Serial.println();
    Serial.println("=== SL6806 PWM clock: romclk 39 and the counter ===");
    Serial.printf("cru+F4 on entry: %08x   (bit0 = romclk 39, bit4 = dead src)\n",
                  (unsigned)sl6806_mmio_read(SL6806_PWM_CLK_REG));
    Serial.println("running A..E from loop(); 1..5 re-runs one, 0 re-runs all");
    /* And that is all setup() does. See the note above. */
}

void loop()
{
    int key = Serial.read();

    if (key == '0')
        start(0, NCONFIG - 1);
    else if (key >= '1' && key <= '0' + NCONFIG)
        start(key - '1', key - '1');

    if (state == ST_IDLE || (int32_t)(millis() - due) < 0)
        return;

    switch (state) {
    case ST_ANNOUNCE:
        Serial.println();
        Serial.printf("--- %s\n", configs[cfg].name);
        state = ST_BRINGUP;
        break;

    case ST_BRINGUP:
        bring_up(&configs[cfg]);
        snapshot(snap_a);
        due = millis() + 20;        /* let a running counter get somewhere */
        state = ST_SNAP_B;
        break;

    case ST_SNAP_B:
        verdict();
        Serial.println("     sweeping duty 0 -> 100 -> 0, twice; watch the panel");
        sweep_i = 0;
        state = ST_SWEEP;
        break;

    case ST_SWEEP:
        sl6806_backlight_set(sweep_duty(sweep_i));
        if (++sweep_i > SWEEP_STEPS) {
            sl6806_backlight_set(100);
            state = (cfg < last_cfg) ? (cfg++, ST_ANNOUNCE) : ST_IDLE;
            if (state == ST_IDLE)
                Serial.println("\ndone. Press 0 to repeat, 1..5 for one.");
        } else {
            due = millis() + SWEEP_MS;
        }
        break;

    default:
        state = ST_IDLE;
        break;
    }
}
