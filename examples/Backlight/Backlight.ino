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
 * SWEPT - AND THE PANEL LIT UP DURING THE SWEEP.
 *
 * That is the first light anyone has got out of this board from a payload,
 * and it says the previous run's conclusion was wrong for a specific reason:
 * CTRL bit 28 is not a usable "counter stopped" flag. It stayed set the whole
 * time while the backlight was visibly blinking. Every "still busy" in that
 * log, and the "no clock source started the counter" at the end of it, were
 * measuring the wrong thing.
 *
 * Worse, the sweep threw the answer away. It wrote the pair register back to
 * zero at the end of each divider row, so whatever setting lit the panel was
 * extinguished a moment later - which is exactly what "blinks, then dark for
 * good" looks like.
 *
 * The sweep did pin the register's shape: it retains only 0x10F, so src is
 * [3:0] and the divider is a single bit at [8], not the eight-bit field
 * `div << 8` implied.
 *
 * ---------------------------------------------------------------------
 * SO THE DETECTOR IS NOW YOU, NOT A STATUS BIT.
 *
 * This holds ONE clock setting per host tick and prints it before applying
 * it, and it never writes the register back to zero. When the panel lights,
 * the value naming it is the line you just read.
 *
 * Type any character into the monitor (anything but a bare newline, which is
 * what --tick sends) and it FREEZES on the current setting and keeps it. So
 * the procedure is: watch, and the moment the backlight comes on, hit a key.
 * The log then says exactly which source and divider did it, and the panel
 * stays lit while you read it.
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

/*
 * Every setting the pair register can actually hold. The sweep measured the
 * writable mask as 0x10F, so that is src 0..15 against a one-bit divider -
 * 32 combinations, not the 96 the last run thought it was trying.
 */
#define NSRC   16
#define NDIV   2
#define NCOMBO (NSRC * NDIV)

static int  idx;
static bool frozen;
static bool white;

static uint32_t combo_value(int i)
{
    return SL6806_PWM_PAIR_VALUE(i % NSRC, (i / NSRC) ? 1u : 0u);
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
    uint32_t base = SL6806_PWM_CHAN(3);

    Serial.print("  pad 0x");
    Serial.print(SL6806_PWM_BL_PAD, HEX);
    Serial.print(" -> ");
    Serial.println(sl6806_pad_configure(SL6806_PWM_BL_PAD) == 0 ? "ok" : "REFUSED");

    sl6806_mmio_write(base + SL6806_PWM_CTRL, SL6806_PWM_CTRL_INIT);
    sl6806_mmio_write(base + SL6806_PWM_CTRL,
                      sl6806_mmio_read(base + SL6806_PWM_CTRL) | 0x3Fu);
    /* Full brightness while hunting - a dim backlight is easy to miss. */
    sl6806_pwm_set(3, SL6806_PWM_BL_PERIOD, SL6806_PWM_BL_DUTY(100));
    sl6806_pwm_enable(3, 1);
    sl6806_pwm_run(3, 1);

    show("ctrl", base + SL6806_PWM_CTRL);
    show("p/d ", base + SL6806_PWM_PERIOD_DUTY);
}

void setup()
{
    Serial.begin(115200);
    Serial.println();
    Serial.println("=== SL6806 backlight: hold each clock setting ===");

    Serial.print("module ");
    Serial.print(SL6806_PWM_MODULE_ID);
    Serial.print(" -> ack ");
    Serial.println(sl6806_module_enable(SL6806_PWM_MODULE_ID) ? "yes" : "no");
    configure();

    if (!Screen.begin(band, 240, 8))
        Serial.println("  no panel");

    Serial.println();
    Serial.println("Duty is 100%. One clock setting per tick, held, never");
    Serial.println("reset. WHEN THE PANEL LIGHTS, PRESS ANY KEY - that");
    Serial.println("freezes it here and the last line printed is the answer.");
    Serial.println();
}

void loop()
{
    int c;

    /* Keep the picture moving so a lit panel is unmistakable. */
    white = !white;
    Screen.fill(white ? SL6806_WHITE : SL6806_BLACK);
    Screen.display();

    c = Serial.read();
    if (c < 0)
        return;

    /* Anything but the tick's newline means "stop here". */
    if (c != '\n' && c != '\r' && !frozen) {
        frozen = true;
        Serial.println();
        Serial.print("*** FROZEN at src ");
        Serial.print((idx - 1 + NCOMBO) % NCOMBO % NSRC);
        Serial.print(", div ");
        Serial.print(((idx - 1 + NCOMBO) % NCOMBO) / NSRC);
        Serial.print("  (pair 0x");
        Serial.print(sl6806_mmio_read(SL6806_PWM_PAIR(3)), HEX);
        Serial.println(") - holding");
        return;
    }
    if (frozen)
        return;

    if (idx >= NCOMBO) {
        Serial.println();
        Serial.println("=== all 32 settings tried and held ===");
        Serial.println("If the panel lit at some point and you missed the");
        Serial.println("key, re-run: the order is deterministic.");
        frozen = true;
        return;
    }

    Serial.print("  [");
    Serial.print(idx);
    Serial.print("/");
    Serial.print(NCOMBO - 1);
    Serial.print("] src ");
    Serial.print(idx % NSRC);
    Serial.print(", div ");
    Serial.print(idx / NSRC);
    Serial.print("  -> pair 0x");
    Serial.print(combo_value(idx), HEX);
    Serial.print("   busy ");
    Serial.println((sl6806_mmio_read(SL6806_PWM_CHAN(3) + SL6806_PWM_CTRL)
                    & SL6806_PWM_CTRL_BUSY) ? "set" : "clear");

    sl6806_mmio_write(SL6806_PWM_PAIR(3), combo_value(idx));
    idx++;
}
