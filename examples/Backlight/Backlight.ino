/*
 * Backlight - find the clock that wakes the PWM, then drive it.
 *
 * TWO RUNS SO FAR, both negative, and both narrowed it:
 *
 *   2026-08-07 a. Every register zero, including the module gate itself
 *     (modctl 0x0 -> 0x0). The write did not stick, so the gate bit was never
 *     the question. Reading the same addresses from the host in the same mode
 *     gave the same answer, while the CRU and the LCDC read live - so MMIO is
 *     fine and 0x400E**** is unclocked, not absent, and not payload-specific.
 *
 *   2026-08-07 b. Started the PLL as 0x00D9A7FC does. It locked
 *     (0x40080008: 0x801 -> 0xD0010C04, bit 28 set) and changed nothing:
 *     0x400E0000 still took no writes, on any of 32 gate bits.
 *
 * SO IT IS NOT THE PLL, and it is not the power domains either: 0x40000070
 * reads 0x03FF03FF, which is all ten of 0x00D9A7AC's request bits set and all
 * ten acknowledge bits back, so that handshake is already done before we
 * arrive.
 *
 *   2026-08-07 c. The sweep found it: CRU 0x40080068 / 0x40080078 bit 4.
 *     With that set the block answers - CTRL took 0x7F and read back
 *     0x1000007F (bit 28 is the busy flag 0x00811E74 polls), and period/duty
 *     took (48000 << 16) | 28800 exactly. Every register did what it was
 *     told, and the panel still did not light.
 *
 * WHICH LEFT THE PAD. A PWM running into a pin that is still muxed to GPIO
 * drives nothing. The vendor's configure op (0x00D45394) calls ROM 0x93C -
 * pad configure from a packed id - with the id its constructor put at
 * dev+0x48, and that id is 0x00010200: bank 1, pin 0, alternate function 4.
 * This sketch now does that first, and that is the one thing every previous
 * run was missing.
 *
 * It is also the thing BacklightHunt could not have found: it drove pads as
 * function 1, plain output, which is not what function 4 does, and its
 * candidate list skipped bank 1's low pins as the panel's bus - true of pins
 * 1-8, not of pin 0.
 *
 * THE SWEEP IS STILL HERE as a fallback, in case another board disagrees
 * about the gate. It runs inside one loop() call rather than one bit per
 * tick: a clock gate settles in cycles, there is nothing to watch, and 32
 * ticks would outlive the monitor's foreground limit.
 *
 * THE PROBE IS A WRITE AND A READ BACK, not a bare read: a register that
 * reads zero because it is dead and one that reads zero because it is idle
 * are the same thing from here, and the first two runs of this sketch were
 * both misled by exactly that. Channel 3's period/duty is the scratch - the
 * channel is not running, so scribbling on it costs nothing.
 *
 *     make SKETCH=examples/Backlight RUN_MODE=poll upload
 *     tools/sl6806-monitor --tick 3 build/Backlight.sym
 *
 * SAFETY. The sweep only ever ORs one extra bit into a gate register and then
 * puts the register back the way it found it - it never clears bit 3 or bit
 * 15, because the LCDC and whatever else is running are behind them. Turning
 * a clock on for a block whose reset is still asserted does nothing. Nothing
 * writes flash, so a replug always recovers.
 *
 * Still not written: 0x4008011C, the clock-source select the vendor sets
 * after the PLL locks. If it reparents the core or USB onto the PLL the
 * session ends and takes the log with it.
 */

#include <Arduino.h>
#include "sl6806_pwm.h"
#include "sl6806_cru.h"
#include "sl6806_padctl.h"
#include "sl6806_lcdc.h"

static sl6806_color_t band[240 * 8];

/* 0 -> 100 -> 0, so a backlight that is on but dim is still obvious. */
static const uint8_t ramp[] = { 0, 20, 40, 60, 80, 100, 80, 60, 40, 20 };
#define NRAMP ((int)(sizeof ramp))

#define BL_CHAN 3

/* The CRU gate words, as pairs. 0x64/0x74 are the documented ones; the other
 * two pairs are their neighbours, which hold structured non-zero values and
 * so are plausibly more of the same. */
typedef struct { uint16_t a, b; const char *what; } gatepair_t;
static const gatepair_t gates[] = {
    { 0x64, 0x74, "0x64/0x74 (the LCDC's own)" },
    { 0x60, 0x70, "0x60/0x70" },
    { 0x68, 0x78, "0x68/0x78" },
};
#define NGATES ((int)(sizeof gates / sizeof gates[0]))

enum { ST_PLL, ST_SWEEP, ST_DRIVE, ST_DONE };

static uint8_t state = ST_PLL;
static int     gi;             /* index into gates[]          */
static int     hit_reg = -1;   /* the CRU offset that worked  */
static int     hit_bit = -1;
static int     step;
static bool    white;

static void show(const char *what, uint32_t addr)
{
    Serial.print("  ");
    Serial.print(what);
    Serial.print(" @0x");
    Serial.print(addr, HEX);
    Serial.print(" = 0x");
    Serial.println(sl6806_mmio_read(addr), HEX);
}

/* Does the PWM answer? Write a pattern and read it back - a bare read cannot
 * tell a dead register from an idle one. */
static bool pwm_responds(void)
{
    uint32_t reg = SL6806_PWM_CHAN(BL_CHAN) + SL6806_PWM_PERIOD_DUTY;

    sl6806_mmio_write(reg, 0x12345678u);
    if (sl6806_mmio_read(reg) == 0)
        return false;
    sl6806_mmio_write(reg, 0);
    return true;
}

/* 0x00D9A7FC, minus the clock-source select. Returns true if it locked. */
static bool pll_start(void)
{
    uint32_t i;

    if (sl6806_mmio_read(SL6806_PLL_CTRL) & SL6806_PLL_LOCK)
        return true;                         /* already up */

    sl6806_mmio_write(SL6806_PLL_CTRL, SL6806_PLL_CONFIG);

    /* Bounded: this runs inside the boot ROM's USB handler, and a spin here
     * takes the device off the bus until it is unplugged. */
    for (i = 0; i < 400000u; i++)
        if (sl6806_mmio_read(SL6806_PLL_CTRL) & SL6806_PLL_LOCK)
            break;

    if (!(sl6806_mmio_read(SL6806_PLL_CTRL) & SL6806_PLL_LOCK))
        return false;

    sl6806_mmio_write(SL6806_PLL_CTRL,
                      sl6806_mmio_read(SL6806_PLL_CTRL) | SL6806_PLL_OUT_ENABLE);
    return true;
}

/*
 * Try every bit of one CRU gate pair. Returns the bit that woke the PWM, or
 * -1. Restores both registers before returning, whatever happens.
 */
static int sweep_pair(const gatepair_t *g)
{
    uint32_t ra = SL6806_CRU_BASE + g->a;
    uint32_t rb = SL6806_CRU_BASE + g->b;
    uint32_t base_a = sl6806_mmio_read(ra);
    uint32_t base_b = sl6806_mmio_read(rb);
    int bit, found = -1;

    Serial.print("  ");
    Serial.print(g->what);
    Serial.print(" = 0x");
    Serial.print(base_a, HEX);
    Serial.print(" / 0x");
    Serial.println(base_b, HEX);

    for (bit = 0; bit < 32; bit++) {
        if (base_a & (1u << bit))
            continue;                        /* already on; leave it alone */

        sl6806_mmio_write(ra, base_a | (1u << bit));
        sl6806_mmio_write(rb, base_b | (1u << bit));
        delayMicroseconds(200);

        if (pwm_responds()) {
            found = bit;
            break;
        }

        sl6806_mmio_write(ra, base_a);
        sl6806_mmio_write(rb, base_b);
    }

    if (found < 0) {
        sl6806_mmio_write(ra, base_a);
        sl6806_mmio_write(rb, base_b);
        Serial.println("    no bit woke the PWM");
    } else {
        Serial.print("    bit ");
        Serial.print(found);
        Serial.println(" WOKE THE PWM - left on");
    }
    /* Whatever happened, say what the neighbouring blocks look like now. */
    show("modctl", SL6806_MODCTL_BASE);
    return found;
}

static void configure(unsigned ch)
{
    uint32_t base = SL6806_PWM_CHAN(ch);
    int rc;

    /*
     * Mux the pin first. A PWM that runs into a pad still selected for GPIO
     * drives nothing, which is the most likely reason the previous run
     * programmed every register correctly and changed nothing on the panel.
     */
    rc = sl6806_pad_configure(SL6806_PWM_BL_PAD);
    Serial.print("  pad 0x");
    Serial.print(SL6806_PWM_BL_PAD, HEX);
    Serial.print(" (bank ");
    Serial.print(SL6806_PAD_BANK(SL6806_PWM_BL_PAD));
    Serial.print(" pin ");
    Serial.print(SL6806_PAD_PIN(SL6806_PWM_BL_PAD));
    Serial.print(" fn ");
    Serial.print(SL6806_PAD_FUNC(SL6806_PWM_BL_PAD));
    Serial.print(") -> ");
    Serial.println(rc == 0 ? "ok" : "REFUSED");

    sl6806_mmio_write(base + SL6806_PWM_CTRL, SL6806_PWM_CTRL_INIT);
    sl6806_mmio_write(base + SL6806_PWM_CTRL,
                      sl6806_mmio_read(base + SL6806_PWM_CTRL) | 0x3Fu);

    sl6806_pwm_set(ch, SL6806_PWM_BL_PERIOD, SL6806_PWM_BL_DUTY(60));
    sl6806_pwm_enable(ch, 1);
    sl6806_pwm_run(ch, 1);

    show("ctrl", base + SL6806_PWM_CTRL);
    show("p/d ", base + SL6806_PWM_PERIOD_DUTY);
    show("mode", base + SL6806_PWM_MODE);
}

void setup()
{
    Serial.begin(115200);
    Serial.println();
    Serial.println("=== SL6806 backlight: hunting the PWM's clock ===");

    show("pll   ", SL6806_PLL_CTRL);
    show("domain", 0x40000070u);
    show("modctl", SL6806_MODCTL_BASE);
    show("pwm3  ", SL6806_PWM_CHAN(BL_CHAN) + SL6806_PWM_CTRL);
    Serial.println("(domain should read 0x3FF03FF - requests and acks both in)");
    Serial.println();

    if (!Screen.begin(band, 240, 8))
        Serial.println("no panel - the flash below will do nothing");
    else
        Serial.println("lcd bus up; flashing white/black while we try");
    Serial.println();
    Serial.println("one stage per host tick; run the monitor with --tick 3");
    Serial.println();
}

void loop()
{
    white = !white;
    Screen.fill(white ? SL6806_WHITE : SL6806_BLACK);
    Screen.display();

    if (state == ST_DONE)
        return;

    if (Serial.read() < 0)
        return;

    switch (state) {

    case ST_PLL:
        Serial.println("PLL:");
        if (!pll_start()) {
            show("pll   ", SL6806_PLL_CTRL);
            Serial.println("  NO LOCK - doubt the config word before anything else");
            state = ST_DONE;
            return;
        }
        show("pll   ", SL6806_PLL_CTRL);

        /* The gate is known now - measured, not guessed. Set it directly and
         * only fall back to the sweep if this board disagrees. */
        sl6806_mmio_write(SL6806_CRU_BASE + SL6806_PWM_CRU_GATE0,
                          sl6806_mmio_read(SL6806_CRU_BASE + SL6806_PWM_CRU_GATE0)
                          | SL6806_PWM_CRU_GATE_BIT);
        sl6806_mmio_write(SL6806_CRU_BASE + SL6806_PWM_CRU_GATE1,
                          sl6806_mmio_read(SL6806_CRU_BASE + SL6806_PWM_CRU_GATE1)
                          | SL6806_PWM_CRU_GATE_BIT);
        delayMicroseconds(200);
        show("gate0 ", SL6806_CRU_BASE + SL6806_PWM_CRU_GATE0);

        if (pwm_responds()) {
            Serial.println("  PWM answers.");
            Serial.println();
            state = ST_DRIVE;
            step = 0;
            configure(BL_CHAN);
            return;
        }
        Serial.println("  PWM still dead - falling back to the full sweep.");
        Serial.println();
        gi = 0;
        state = ST_SWEEP;
        return;

    case ST_SWEEP: {
        int bit = sweep_pair(&gates[gi]);

        if (bit >= 0) {
            hit_reg = gates[gi].a;
            hit_bit = bit;
            Serial.println();
            step = 0;
            state = ST_DRIVE;
            configure(BL_CHAN);
            return;
        }

        if (++gi >= NGATES) {
            Serial.println();
            Serial.println("=== no CRU gate woke 0x40084000 ===");
            Serial.println("The gates are not it, or the PWM needs its reset");
            Serial.println("released too, or 0x40084000 is not where it lives.");
            Serial.println("Report the three baselines above - they are the");
            Serial.println("part of this run that is worth something.");
            state = ST_DONE;
        }
        return;
    }

    case ST_DRIVE:
        if (step >= NRAMP) {
            Serial.println();
            Serial.print("=== done");
            if (hit_bit >= 0) {
                Serial.print("; CRU 0x");
                Serial.print(hit_reg, HEX);
                Serial.print(" bit ");
                Serial.print(hit_bit);
                Serial.print(" is the PWM's gate");
            }
            Serial.println(" ===");
            state = ST_DONE;
            return;
        }
        sl6806_pwm_set(BL_CHAN, SL6806_PWM_BL_PERIOD,
                       SL6806_PWM_BL_DUTY(ramp[step]));
        Serial.print("  duty ");
        Serial.print(ramp[step]);
        Serial.println("%");
        step++;
        return;
    }
}
