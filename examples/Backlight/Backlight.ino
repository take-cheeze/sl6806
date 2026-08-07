/*
 * Backlight - find the clock that wakes the PWM, then drive it.
 *
 * FOUR RUNS SO FAR, each negative and each narrowing it:
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
 *   2026-08-07 d. Gate set, pad muxed to bank 1 pin 0 function 4, every
 *     register correct - CTRL 0x1000007F, period/duty 0xBB807080, mode 1 -
 *     and still no light. Reading the block back from the host afterwards
 *     showed why it cannot be a register-contents problem: the channel is
 *     programmed exactly right and CTRL bit 28, the busy flag the vendor
 *     spins on before every write, is stuck set. A counter with no clock.
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
 * WHAT CHANGED, AND WHY IT IS WORTH ANOTHER RUN. The keys were stuck on the
 * same wall and came off it, so the method that freed them is now pointed
 * here. The mask ROM's module_clock_enable (0x00001C5C, transcribed in
 * sl6806_module.c) says three things every sweep in this file got wrong:
 *
 *   - there are 128 module ids across FOUR register pairs, and the fourth is
 *     at 0x400F1000, not in the CRU at all - so a quarter of the space was
 *     never tried;
 *   - 0x60/0x64/0x68 are gates and 0x70/0x74/0x78 are their shadows, not two
 *     independent banks;
 *   - the shadow is written first, then the gate, then you wait for the gate
 *     to read the bit back. Writing both at once does nothing, which is
 *     precisely how the ADC's correct bit was recorded as dead.
 *
 * THE MODULE WALK IS DONE AND IT WAS A CLEAN NEGATIVE. All 128 ids, in the
 * ROM's order, with the acknowledgement waited for: the counter stayed
 * stopped. That is a real result, unlike the sweeps before it, and it means
 * the counter does not want a second module clock.
 *
 * SO WHAT IS LEFT IS THE ONE REGISTER NOBODY WRITES. Each channel *pair* has
 * a register at 0x40084010 + (ch >> 1) * 4, and 0x00811EC0 writes it as
 * `src | (div << 8)` - the shape of a counter clock select. Nothing in flash
 * or in the SRAM blob ever writes it; it reads 0 on a cold chip and stays 0.
 * A counter with no source is exactly what "every register correct, nothing
 * moves" looks like.
 *
 * SWEPT, AND IT WAS NOT THE ANSWER EITHER. Sixteen sources against six
 * dividers: the counter stayed stopped. The sweep did pin the register's
 * shape - it only retains 0x10F, so src is [3:0] and the divider is one bit
 * at [8], not the eight-bit field `div << 8` implied.
 *
 * ---------------------------------------------------------------------
 * THIS SKETCH HAS RUN OUT OF THINGS TO TRY, AND THAT IS THE POINT OF IT.
 *
 * The gate, the pad, the register contents, all 128 module ids in the ROM's
 * own order, every gate-shaped CRU register, every module clock, and the
 * counter's clock select have each been covered by measurement. The PWM is
 * correctly programmed and its counter does not move. Whatever starts it is
 * not in the address space a payload can reach.
 *
 * So do not add another register guess here. The next move is ground truth:
 * read the CRU and this block while the stock firmware runs, and diff it
 * against the cold state in 13f. That needs a way past read_mem being refused
 * in card-reader mode (7d.1), which is a project rather than a run.
 *
 * What this sketch is still good for is confirming the map on another board,
 * and as the place the sweep lives if someone finds a new register class to
 * point it at.
 * ---------------------------------------------------------------------
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
 * NOW WRITTEN, after run d showed why it had to be: 0x4008011C. The vendor
 * sets it to 0x31 right after the PLL locks and to 0x30 on the way down. One
 * bit apart means bit 0 is an enable, not the clock-source reparent it was
 * first taken for - and a plain enable cannot take the core or USB with it,
 * which is what the earlier caution was about.
 */

#include <Arduino.h>
#include "sl6806_pwm.h"
#include "sl6806_module.h"
#include "sl6806_padctl.h"
#include "sl6806_lcdc.h"

static sl6806_color_t band[240 * 8];

static const uint8_t ramp[] = { 0, 20, 40, 60, 80, 100, 80, 60, 40, 20 };
#define NRAMP ((int)(sizeof ramp))

#define BL_CHAN 3

/* Dividers worth trying: none, a couple of small ones, and a big one. */
static const uint8_t divs[] = { 0, 1, 3, 7, 15, 63 };
#define NDIVS ((int)(sizeof divs / sizeof divs[0]))

enum { ST_SWEEP, ST_DRIVE, ST_DONE };

static uint8_t state = ST_SWEEP;
static int     di;                 /* index into divs[] */
static int     found_src = -1, found_div = -1;
static int     step;
static bool    white;

static bool busy(void)
{
    return (sl6806_mmio_read(SL6806_PWM_CHAN(BL_CHAN) + SL6806_PWM_CTRL)
            & SL6806_PWM_CTRL_BUSY) != 0;
}

static void show(const char *what, uint32_t addr)
{
    Serial.print("  ");
    Serial.print(what);
    Serial.print(" = 0x");
    Serial.println(sl6806_mmio_read(addr), HEX);
}

static void configure(void)
{
    uint32_t base = SL6806_PWM_CHAN(BL_CHAN);

    Serial.print("  pad 0x");
    Serial.print(SL6806_PWM_BL_PAD, HEX);
    Serial.print(" (bank 1 pin 0 fn 4) -> ");
    Serial.println(sl6806_pad_configure(SL6806_PWM_BL_PAD) == 0 ? "ok" : "REFUSED");

    sl6806_mmio_write(base + SL6806_PWM_CTRL, SL6806_PWM_CTRL_INIT);
    sl6806_mmio_write(base + SL6806_PWM_CTRL,
                      sl6806_mmio_read(base + SL6806_PWM_CTRL) | 0x3Fu);
    sl6806_pwm_set(BL_CHAN, SL6806_PWM_BL_PERIOD, SL6806_PWM_BL_DUTY(60));
    sl6806_pwm_enable(BL_CHAN, 1);
    sl6806_pwm_run(BL_CHAN, 1);

    show("ctrl", base + SL6806_PWM_CTRL);
    show("p/d ", base + SL6806_PWM_PERIOD_DUTY);
    show("pair", SL6806_PWM_PAIR(BL_CHAN));
}

/* One divider's worth of sources per loop() call. */
static int sweep_step(void)
{
    uint32_t reg = SL6806_PWM_PAIR(BL_CHAN);
    unsigned src;

    if (di >= NDIVS)
        return -1;

    Serial.print("  div ");
    Serial.print(divs[di]);
    Serial.print(", src 0..15: ");

    for (src = 0; src < 16; src++) {
        sl6806_mmio_write(reg, SL6806_PWM_PAIR_VALUE(src, divs[di]));
        delayMicroseconds(300);
        if (!busy()) {
            Serial.print("src ");
            Serial.print(src);
            Serial.println(" CLEARED BUSY - the counter runs");
            found_src = (int)src;
            found_div = divs[di];
            di = NDIVS;
            return 1;
        }
    }
    Serial.print("still busy (pair reads 0x");
    Serial.print(sl6806_mmio_read(reg), HEX);
    Serial.println(")");
    sl6806_mmio_write(reg, 0);
    di++;
    return 0;
}

void setup()
{
    Serial.begin(115200);
    Serial.println();
    Serial.println("=== SL6806 backlight: the counter's clock source ===");

    Serial.print("module ");
    Serial.print(SL6806_PWM_MODULE_ID);
    Serial.print(" -> ack ");
    Serial.println(sl6806_module_enable(SL6806_PWM_MODULE_ID) ? "yes" : "no");

    configure();
    Serial.print("  busy: ");
    Serial.println(busy() ? "STUCK (counter stopped)" : "clear (counter runs)");

    if (!busy()) {
        Serial.println("Already running - skipping the sweep.");
        state = ST_DRIVE;
        di = NDIVS;
    } else {
        Serial.println("Sweeping the pair clock register 0x40084014.");
    }

    if (!Screen.begin(band, 240, 8))
        Serial.println("no panel - the flash below will do nothing");
    Serial.println();
}

void loop()
{
    white = !white;
    Screen.fill(white ? SL6806_WHITE : SL6806_BLACK);
    Screen.display();

    if (state == ST_DONE)
        return;

    if (state == ST_SWEEP) {
        int r = sweep_step();

        if (r == 1) {
            state = ST_DRIVE;
            step = 0;
        } else if (r == -1) {
            Serial.println();
            Serial.println("=== no clock source started the counter ===");
            Serial.println("Module ids exhausted, gate correct, pad correct,");
            Serial.println("registers correct, clock select swept. The next");
            Serial.println("move is ground truth, not another guess.");
            state = ST_DONE;
        }
        return;
    }

    if (Serial.read() < 0)
        return;

    if (step >= NRAMP) {
        Serial.println();
        if (found_src >= 0) {
            Serial.print("=== counter runs with src ");
            Serial.print(found_src);
            Serial.print(", div ");
            Serial.print(found_div);
            Serial.println(" ===");
        } else {
            Serial.println("=== done ===");
        }
        state = ST_DONE;
        return;
    }
    sl6806_pwm_set(BL_CHAN, SL6806_PWM_BL_PERIOD, SL6806_PWM_BL_DUTY(ramp[step]));
    Serial.print("  duty ");
    Serial.print(ramp[step]);
    Serial.print("%  busy ");
    Serial.println(busy() ? "stuck" : "clear");
    step++;
}
