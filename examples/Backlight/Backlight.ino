/*
 * Backlight - wake the clock domain the PWM lives in, then drive it.
 *
 * FIRST RUN, 2026-08-07: every register read back zero, including the module
 * gate itself (modctl 0x0 -> 0x0). The gate bit was never the problem - the
 * write did not stick, so nothing downstream of it could.
 *
 * WHAT THAT TURNED OUT TO MEAN. Reading the same addresses from the host over
 * the vendor read command, in the same bootloader mode, gives the same
 * answer: 0x400E0000 and 0x400E2000 are zeros, while the CRU at 0x40080000
 * and the LCDC at 0x400D9000 read live values. So MMIO is fine and that whole
 * region is simply unclocked - not a payload problem, and not a wrong address.
 *
 * And the firmware says why. 0x00D9A7FC, the first thing the vendor's module
 * bring-up does, starts a PLL at 0x40080008 and spins on a lock bit before it
 * ever touches 0x400E0000. In bootloader mode that register reads 0x00000801
 * with the lock bit clear: the PLL is stopped, so the domain is dark.
 *
 * SO THIS SKETCH DOES THREE THINGS, one per host tick, and prints a readback
 * after every one:
 *
 *   1. Bring the PLL up the way 0x00D9A7FC does, and report whether it locks.
 *   2. If the module-gate register comes alive, find the PWM's bit by
 *      experiment - set each bit in turn and see whether 0x40084000 starts
 *      answering. Nobody knows which bit it is; the previous claim of bit 2
 *      was misread from a neighbouring module's teardown.
 *   3. Once a bit works, program channel 3 with the vendor's period and ramp
 *      the duty, flashing the panel so a backlight coming on is unmistakable.
 *
 * If step 1 does not lock, stop and report - everything after it is moot, and
 * the PLL config word is then the thing to doubt.
 *
 *     make SKETCH=examples/Backlight RUN_MODE=poll upload
 *     tools/sl6806-monitor --tick 3 build/Backlight.sym
 *
 * PACED BY THE HOST. Same lesson BacklightHunt records at length: neither
 * clock on this device can time a dwell, because the USB poll rate has been
 * measured anywhere from 0.3/s to 42/s. One step per byte from the host.
 *
 * RISK. This writes a PLL control register, which is a bigger thing than the
 * rest of the tree does - a clock tree can be misconfigured into stopping the
 * core. It is the vendor's own constant, and the lock poll is bounded so a
 * PLL that never comes up cannot hang the USB handler. Nothing writes flash,
 * so a replug always recovers. What this deliberately does NOT do is write
 * 0x4008011C, the clock-source select the vendor sets next: if that reparents
 * the core or USB onto the PLL it would end the session, and the ROM's USB
 * link is the only way to see anything here.
 */

#include <Arduino.h>
#include "sl6806_pwm.h"
#include "sl6806_lcdc.h"

static sl6806_color_t band[240 * 8];

/* 0 -> 100 -> 0, so a backlight that is on but dim is still obvious. */
static const uint8_t ramp[] = { 0, 20, 40, 60, 80, 100, 80, 60, 40, 20 };
#define NRAMP ((int)(sizeof ramp))

/* The channel the firmware opens for the backlight. */
#define BL_CHAN 3

enum { ST_PLL, ST_SWEEP, ST_DRIVE, ST_DONE };

static uint8_t state = ST_PLL;
static int     bit;            /* gate bit under test        */
static int     found = -1;     /* the bit that woke the PWM  */
static int     step;           /* index into ramp[]          */
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

/*
 * Does the PWM block answer? A register that only ever reads zero is
 * indistinguishable from one that is not there, so write a pattern and read
 * it back rather than trusting a bare read. Channel 3's period/duty is
 * harmless to scribble on: the channel is not running yet.
 */
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

    sl6806_mmio_write(SL6806_PLL_CTRL, SL6806_PLL_CONFIG);

    /* Bounded. The vendor spins forever; we cannot, because this runs inside
     * the boot ROM's USB handler and a spin here takes the device off the
     * bus until it is unplugged. A few hundred thousand reads is milliseconds
     * at any plausible core clock. */
    for (i = 0; i < 400000u; i++)
        if (sl6806_mmio_read(SL6806_PLL_CTRL) & SL6806_PLL_LOCK)
            break;

    if (!(sl6806_mmio_read(SL6806_PLL_CTRL) & SL6806_PLL_LOCK))
        return false;

    sl6806_mmio_write(SL6806_PLL_CTRL,
                      sl6806_mmio_read(SL6806_PLL_CTRL) | SL6806_PLL_OUT_ENABLE);
    return true;
}

static void configure(unsigned ch)
{
    uint32_t base = SL6806_PWM_CHAN(ch);

    /* The vendor writes the bare 0x40 first, then ORs the assembled flags. */
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
    Serial.println("=== SL6806 backlight: PLL, then gate, then PWM ===");

    show("pll   ", SL6806_PLL_CTRL);
    show("modctl", SL6806_MODCTL_BASE);
    show("pwm3  ", SL6806_PWM_CHAN(BL_CHAN) + SL6806_PWM_CTRL);
    Serial.println("(all three are expected to be dead before the PLL runs)");
    Serial.println();

    if (!Screen.begin(band, 240, 8))
        Serial.println("no panel - the flash below will do nothing");
    else
        Serial.println("lcd bus up; flashing white/black while we try");
    Serial.println();
    Serial.println("one step per host tick; run the monitor with --tick 3");
    Serial.println();
}

void loop()
{
    /* Flip every poll, not every tick, so the picture stays lively while we
     * wait on the host. */
    white = !white;
    Screen.fill(white ? SL6806_WHITE : SL6806_BLACK);
    Screen.display();

    if (state == ST_DONE)
        return;

    if (Serial.read() < 0)
        return;

    switch (state) {

    case ST_PLL:
        Serial.println("starting the PLL (0x40080008 <- 0xC0000C04)");
        if (!pll_start()) {
            show("pll   ", SL6806_PLL_CTRL);
            Serial.println("NO LOCK. Everything after this is moot - report");
            Serial.println("that readback; the config word is what to doubt.");
            state = ST_DONE;
            return;
        }
        show("pll   ", SL6806_PLL_CTRL);
        show("modctl", SL6806_MODCTL_BASE);
        Serial.println("PLL locked. Sweeping the module gate next.");
        Serial.println();
        bit = 0;
        state = ST_SWEEP;
        return;

    case ST_SWEEP: {
        uint32_t before = sl6806_mmio_read(SL6806_MODCTL_BASE);

        sl6806_mmio_write(SL6806_MODCTL_BASE, before | (1u << bit));
        delay(10);

        Serial.print("  gate bit ");
        Serial.print(bit);
        Serial.print(": modctl 0x");
        Serial.print(sl6806_mmio_read(SL6806_MODCTL_BASE), HEX);

        if (pwm_responds()) {
            found = bit;
            Serial.println("  <- PWM ANSWERS");
            Serial.println();
            step = 0;
            state = ST_DRIVE;
            configure(BL_CHAN);
            return;
        }
        Serial.println();

        if (++bit >= 32) {
            Serial.println();
            Serial.println("No bit woke 0x40084000.");
            if (sl6806_mmio_read(SL6806_MODCTL_BASE) == 0)
                Serial.println("modctl still reads 0 - the gate register is");
            else
                Serial.println("modctl took the writes, so the gate is live");
            Serial.println("and the PWM base or its reset is the next suspect.");
            state = ST_DONE;
        }
        return;
    }

    case ST_DRIVE:
        if (step >= NRAMP) {
            Serial.println();
            Serial.print("=== done; gate bit ");
            Serial.print(found);
            Serial.println(" is the one ===");
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
