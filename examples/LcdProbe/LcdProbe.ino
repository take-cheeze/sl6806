/*
 * LcdProbe - work out why the screen is dark.
 *
 * The LCD driver was written from a disassembly of the stock bootloader and
 * has never been checked against a panel. When it does not produce a picture
 * there are five or six different things that could be wrong, and they need
 * different fixes. This sketch tells them apart, and lets you bisect the
 * values that were inferred rather than read - from the monitor, without
 * rebuilding.
 *
 *     make SKETCH=examples/LcdProbe RUN_MODE=poll run
 *
 * Then press keys in the monitor. `h` prints the menu.
 *
 * ---------------------------------------------------------------------
 *  BEFORE YOU CHASE THE DRIVER: THE BACKLIGHT
 * ---------------------------------------------------------------------
 * The panel's backlight is NOT part of the LCD path. In the stock firmware
 * it is a PWM channel - the strings `/dev/pwm_ch3`, `brightness percent %d`
 * and a 48000 Hz period are all in the image - driven by a separate task,
 * and nothing in this framework turns it on. Which means:
 *
 *     a perfectly working driver still shows a black screen.
 *
 * So the first question is not "is the driver right" but "can I see anything
 * at all". Two ways to answer it without a PWM driver:
 *
 *   - Shine a bright light at the panel at an angle and look for the image.
 *     A reflective look at a transmissive LCD is dim but usually readable,
 *     and `w` (fill white) then `k` (fill black) is a big enough change to
 *     see. This is the cheapest decisive test there is.
 *   - Check whether the backlight is already on. If the device came out of
 *     the stock firmware rather than a cold boot, it may be.
 *
 * If the panel is visibly changing under a lamp, the driver works and the
 * only thing left is the backlight. That is a completely different job from
 * debugging QSPI.
 */

#include <Arduino.h>
#include "sl6806_lcdc.h"
#include "sl6806_padctl.h"
#include "sl6806_mmio.h"
#include "sl6806_cru.h"

/* A band, not a screen: 240x296 does not fit in the payload heap. */
static const int16_t BAND_W = 240;
static const int16_t BAND_H = 24;
static sl6806_color_t band[BAND_W * BAND_H];

/* Our own copy, so the knobs below can change one field and re-init. */
static sl6806_lcdc_config_t cfg;
static bool have_cfg;

static void hex32(const char *label, uint32_t v)
{
    Serial.print(label);
    Serial.print(" 0x");
    if (v < 0x10000000) Serial.print("0");
    if (v < 0x1000000)  Serial.print("0");
    if (v < 0x100000)   Serial.print("0");
    if (v < 0x10000)    Serial.print("0");
    if (v < 0x1000)     Serial.print("0");
    if (v < 0x100)      Serial.print("0");
    if (v < 0x10)       Serial.print("0");
    Serial.println(v, HEX);
}

static uint32_t lcdc(uint32_t off)
{
    return sl6806_mmio_read(SL6806_LCDC_BASE + off);
}

static void dumpRegisters()
{
    Serial.println();
    Serial.println("--- LCDC 0x400D9000 ---");
    hex32("  +00 CFG0   ", lcdc(SL6806_LCDC_CFG0));
    hex32("  +04 CFG1   ", lcdc(SL6806_LCDC_CFG1));
    hex32("  +08 CTRL   ", lcdc(SL6806_LCDC_CTRL));
    hex32("  +10 IE     ", lcdc(SL6806_LCDC_IE));
    hex32("  +14 STATUS ", lcdc(SL6806_LCDC_STATUS));
    hex32("  +20 XFER   ", lcdc(SL6806_LCDC_XFER));
    hex32("  +24 CMD0   ", lcdc(SL6806_LCDC_CMD0));
    hex32("  +28 LENGTH ", lcdc(SL6806_LCDC_LENGTH));
    hex32("  +2C CMD1   ", lcdc(SL6806_LCDC_CMD1));
    hex32("  +40 MAP0   ", lcdc(SL6806_LCDC_MAP0));
    hex32("  +44 MAP1   ", lcdc(SL6806_LCDC_MAP1));

    Serial.println("--- CRU 0x40080000 ---");
    hex32("  +64 GATE0  ", sl6806_mmio_read(SL6806_CRU_BASE + SL6806_CRU_GATE0));
    hex32("  +74 GATE1  ", sl6806_mmio_read(SL6806_CRU_BASE + SL6806_CRU_GATE1));
    hex32("  +10C MODCLK", sl6806_mmio_read(SL6806_CRU_BASE + SL6806_CRU_MODCLK));

    /* Bank 1 is where the P20's panel pads live. Word 0 covers pins 0..7,
     * word 1 pins 8..15, four bits each. */
    uint32_t b1 = sl6806_pad_bank_base[1];
    Serial.println("--- pad bank 1 (0x40081040) ---");
    hex32("  func 0-7   ", sl6806_mmio_read(b1 + 0x00));
    hex32("  func 8-15  ", sl6806_mmio_read(b1 + 0x04));
    hex32("  +10 input  ", sl6806_mmio_read(b1 + 0x10));
    hex32("  +14 drive  ", sl6806_mmio_read(b1 + 0x14));

    Serial.print("  pin 7 (reset) reads ");
    Serial.println(sl6806_pad_read(0x13800));
}

static void dumpStats(const char *what)
{
    const sl6806_lcdc_stats_t *s = sl6806_lcdc_stats();

    Serial.print(what);
    Serial.print(": ");
    Serial.print(s->transfers);
    Serial.print(" transfers, ");
    Serial.print(s->timeouts);
    Serial.print(" timeouts, polls min/last/max ");
    Serial.print(s->min_polls == 0xFFFFFFFFu ? 0 : s->min_polls);
    Serial.print("/");
    Serial.print(s->last_polls);
    Serial.print("/");
    Serial.print(s->max_polls);
    Serial.print(", busy waits ");
    Serial.println(s->busy_waits);

    if (s->transfers && s->max_polls == 0) {
        Serial.println("  !! every transfer completed on the first poll.");
        Serial.println("     Nothing is being clocked out - the done bit at");
        Serial.println("     +0x14 is stuck set, or +0x14 is not the status");
        Serial.println("     register. Suspect the module clock, not the panel.");
    }
    if (s->timeouts)
        Serial.println("  !! the controller is not answering at all.");
}

static void reinit(const char *what)
{
    Serial.print("re-initialising: ");
    Serial.println(what);

    sl6806_lcd_bus_register(NULL);
    if (sl6806_lcdc_begin(&cfg) != 0) {
        Serial.println("  begin() FAILED");
        return;
    }
    if (!Screen.begin(band, BAND_W, BAND_H)) {
        Serial.println("  Screen.begin() failed");
        return;
    }
    Serial.println("  ok");
    dumpStats("  after init");
}

static void fill(sl6806_color_t c, const char *name)
{
    uint32_t t0 = millis();

    sl6806_lcdc_stats_reset();
    Screen.fill(c);
    Screen.display();

    Serial.print("fill ");
    Serial.print(name);
    Serial.print(" in ");
    Serial.print(millis() - t0);
    Serial.println(" ms");
    dumpStats("  push");
}

/*
 * The test that says whether the controller drives the pins at all.
 *
 * Step 1 proves the pad and its input register work, by driving a data pad
 * as a plain GPIO and reading it back. Step 2 hands the pad back to the LCD
 * controller and samples the whole bank while a burst of transfers runs; a
 * line that appears as both a one and a zero is switching.
 *
 * If step 1 passes and step 2 shows no movement, the controller is accepting
 * transfers and reporting them complete without putting anything on the
 * wire, and the fault is in the transfer setup, not the panel.
 */
static void padTest()
{
    uint32_t b1 = sl6806_pad_bank_base[1];
    const uint32_t pin1 = SL6806_PAD_ID(1, 1, 0);   /* a data pad */
    uint32_t hi, lo;

    Serial.println();
    Serial.println("--- pad drive test, bank 1 pin 1 ---");

    sl6806_pad_set_func(pin1, SL6806_PAD_FUNC_OUTPUT);
    sl6806_pad_write(pin1, 1);
    hi = sl6806_mmio_read(b1 + 0x10);
    sl6806_pad_write(pin1, 0);
    lo = sl6806_mmio_read(b1 + 0x10);

    Serial.print("  driven high -> input 0x");
    Serial.print(hi, HEX);
    Serial.print(", low -> 0x");
    Serial.println(lo, HEX);

    if ((hi & 2u) && !(lo & 2u))
        Serial.println("  OK: the pad drives and the input register follows.");
    else
        Serial.println("  !! the pad did not follow. Either the bank base is"
                       " wrong or this pin is not what we think.");

    /* Hand it back and watch the whole bank during real traffic. */
    sl6806_pad_set_func(pin1, 2);
    sl6806_lcdc_watch(b1 + 0x10);
    for (int i = 0; i < 64; i++)
        sl6806_lcd_bus()->command(0x00, NULL, 0);   /* NOP: 4 frames each */
    sl6806_lcdc_watch(0);

    Serial.print("  during ");
    Serial.print(sl6806_lcdc_watch_samples);
    Serial.println(" samples of live traffic:");
    hex32("    ever high  ", sl6806_lcdc_watch_ones);
    hex32("    ever low   ", ~sl6806_lcdc_watch_zeros);

    uint32_t moved = sl6806_lcdc_watch_ones & sl6806_lcdc_watch_zeros & 0x17Eu;
    Serial.print("  LCD pads that moved: 0x");
    Serial.println(moved, HEX);
    if (moved) {
        Serial.println("  the controller IS driving the bus.");
    } else {
        Serial.println("  no LCD pad ever changed - but this is only half an");
        Serial.println("  answer. Most pad controllers switch the input buffer");
        Serial.println("  off for a pad in an alternate function, and such a");
        Serial.println("  pad reads 0 forever whatever the controller does.");
        Serial.println("  Press 'P' to find out which case this is.");
    }
}

/*
 * Does the input register see anything at all when a pad is in an alternate
 * function?
 *
 * This decides whether the pad test above means what it says. It reported
 * that no LCD pad ever changes during traffic - but on most pad controllers
 * the input buffer is switched off for a pad in an alternate function, in
 * which case that pad would read 0 forever no matter what the controller
 * does, and the test proves nothing.
 *
 * The discriminator: put a pad in function 2 with a pull-up and no driver.
 * If the input register can see the pad it reads 1; if the buffer is off it
 * reads 0. The pull selectors are an index into a mask ROM table whose
 * up/down meaning is not documented, so this finds the pull-up empirically
 * on a plain input first.
 */
static void inputBufferTest()
{
    uint32_t b1 = sl6806_pad_bank_base[1];
    int pullup = -1;

    Serial.println();
    Serial.println("--- can the input register see an alt-function pad? ---");
    Serial.println("  step 1: find a pull-up, on bank 1 pin 1 as a plain input");

    for (unsigned sel = 4; sel <= 15; sel++) {
        uint32_t id = SL6806_PAD_ID(1, 1, SL6806_PAD_FUNC_INPUT) | sel;

        sl6806_pad_configure(id);
        delay(1);
        int v = (sl6806_mmio_read(b1 + 0x10) >> 1) & 1;
        Serial.print("    pull ");
        Serial.print(sel);
        Serial.print(" -> ");
        Serial.println(v);
        if (v && pullup < 0)
            pullup = (int)sel;
    }

    if (pullup < 0) {
        Serial.println("  no selector pulled the pad high - inconclusive,");
        Serial.println("  the pin may be held low externally.");
        return;
    }

    Serial.print("  step 2: pull ");
    Serial.print(pullup);
    Serial.println(" holds it high. Now the same pull in function 2 (LCDC):");

    sl6806_pad_configure(SL6806_PAD_ID(1, 1, 2) | (unsigned)pullup);
    delay(1);
    int alt = (sl6806_mmio_read(b1 + 0x10) >> 1) & 1;
    Serial.print("    reads ");
    Serial.println(alt);

    if (alt) {
        Serial.println("  the input buffer WORKS in alt mode. So the pad test");
        Serial.println("  is trustworthy, and the controller really is not");
        Serial.println("  driving these pins.");
    } else {
        Serial.println("  the input buffer is OFF in alt mode. The pad test");
        Serial.println("  cannot see anything either way - disregard it. This");
        Serial.println("  needs a scope on the panel flex, not more software.");
    }

    /* Put it back the way the display driver wants it. */
    sl6806_pad_configure(0x00010930u);
    Serial.println("  pad restored to the LCD configuration");
}

/* Ask the panel who it is. If it answers, the bus works end to end. */
static void readTest()
{
    static const uint8_t opcodes[] = { 0x03, 0x0B, 0x02 };
    static const uint8_t cmds[]    = { 0x04, 0x09, 0x0A };  /* RDDID/RDDST/RDPWR */
    uint8_t buf[4];

    Serial.println();
    Serial.println("--- panel read-back ---");
    Serial.println("  (the read opcode is not recorded in the firmware, so");
    Serial.println("   this tries the QSPI-convention candidates)");

    for (unsigned o = 0; o < sizeof(opcodes); o++) {
        for (unsigned c = 0; c < sizeof(cmds); c++) {
            for (unsigned ab = 3; ab <= 4; ab++) {
                buf[0] = buf[1] = buf[2] = buf[3] = 0x5A;
                if (sl6806_lcdc_read(opcodes[o], cmds[c], ab, buf, 4) != 0) {
                    Serial.println("  read failed (timeout)");
                    return;
                }
                Serial.print("  op ");
                Serial.print(opcodes[o], HEX);
                Serial.print(" cmd ");
                Serial.print(cmds[c], HEX);
                Serial.print(" addr ");
                Serial.print(ab);
                Serial.print(" -> ");
                for (unsigned i = 0; i < 4; i++) {
                    Serial.print(buf[i], HEX);
                    Serial.print(" ");
                }
                Serial.println();
            }
        }
    }
    Serial.println("  all 00 or all FF means nobody answered.");
}

/* -------------------------------------------------- panel power / backlight */

/*
 * WHY THIS IS A SHORT LIST AND NOT A SWEEP
 *
 * The first version of this drove every pad in every bank. That froze the
 * device - one of those 192 pads owns something the USB link needs - and it
 * froze it in the worst possible way, because when USB dies the serial ring
 * is never polled and you do not even learn which pad did it. Do not do that.
 *
 * There is no need to guess. The stock firmware configures every pad it uses,
 * and the ids are immediates at its call sites, so the board's pad map can be
 * read straight out of the image (the scan is described in docs/LCD.md). Of
 * the ~80 pads it touches, only a handful are plain outputs, and those are the
 * only candidates for a power rail or a backlight enable.
 *
 * Driving one of these is not a shot in the dark: the stock firmware drives
 * exactly these pads, as outputs, itself.
 */
typedef struct {
    uint32_t id_on;
    uint32_t id_off;
    const char *what;
} power_pad_t;

static const power_pad_t power_pads[] = {
    /* Named by the firmware's own console handler at 0x00D0AEE0, which maps
     * the string "vcomo" to exactly these two ids. VCOM is an LCD panel
     * supply, so if this rail is off the controller will accept every command
     * over QSPI and still show nothing - which is the symptom. Best guess by
     * a distance. */
    { 0x0001A0D0u, 0x0001A090u, "vcomo  (bank 1 pin 20) - LCD VCOM rail" },

    /* Driven low and high in the cluster at 0x00D3C9DC-0x00D3CC3C, which is
     * the display's own suspend/resume pair. */
    { 0x000508C0u, 0x00050880u, "bank 5 pin 1  - toggled with display power" },

    /* Plain output, configured high, at 0x00D46432. */
    { 0x000300C0u, 0x00030080u, "bank 3 pin 0  - output, firmware sets high" },

    /* The two gpio_write targets in the power-sequencing routine at
     * 0x00D44AB8. */
    { 0x00047080u, 0x00047080u, "bank 4 pin 14 - power sequencing" },
    { 0x00047880u, 0x00047880u, "bank 4 pin 15 - power sequencing" },
};
#define NPOWER_PADS ((int)(sizeof(power_pads) / sizeof(power_pads[0])))

static int  padCursor = -1;
static bool padArmed;

static void announceCandidate()
{
    padCursor = (padCursor + 1) % NPOWER_PADS;
    padArmed = true;

    Serial.println();
    Serial.print("armed: ");
    Serial.println(power_pads[padCursor].what);
    Serial.print("  id 0x");
    Serial.print(power_pads[padCursor].id_on, HEX);
    Serial.println(" - press 'y' to drive it, 'l' for the next one");
    Serial.println("  (announced before driving on purpose: if the device");
    Serial.println("   freezes, this line has already reached you)");
}

static void driveArmed(int on)
{
    if (!padArmed) {
        Serial.println("nothing armed - press 'l' first");
        return;
    }
    const power_pad_t *pp = &power_pads[padCursor];
    uint32_t id = on ? pp->id_on : pp->id_off;

    Serial.print(on ? "driving ON : " : "driving OFF: ");
    Serial.println(pp->what);
    sl6806_pad_configure(id);
    if (pp->id_on == pp->id_off)          /* no distinct off id: set level */
        sl6806_pad_write(id, on);
    Serial.println("  done - look at the panel");
}

/* All of them at once. Still only the firmware's own output pads, so this is
 * the fast yes/no without the freeze the old all-pads version caused. */
static void powerOnAll(int on)
{
    Serial.println(on ? "all panel-power candidates ON" 
                      : "all panel-power candidates OFF");
    for (int i = 0; i < NPOWER_PADS; i++) {
        const power_pad_t *pp = &power_pads[i];
        uint32_t id = on ? pp->id_on : pp->id_off;

        Serial.print("  ");
        Serial.println(pp->what);
        sl6806_pad_configure(id);
        if (pp->id_on == pp->id_off)
            sl6806_pad_write(id, on);
    }
    Serial.println("  done - look at the panel");
}

/* ------------------------------------------------- brute force the guesses */

/*
 * Three bits in this driver were inferred, not read: bit 17 ("emit a frame"),
 * bit 18 ("hold chip select") and whether CTRL bit 4 matters. Polarity is
 * exactly the kind of thing that reading a disassembly gets backwards, and
 * getting either of the first two backwards produces a panel that ignores
 * everything - which is what we have.
 *
 * Software cannot watch the bus on this chip: a pad in an alternate function
 * has its input buffer switched off, as 'P' demonstrates. So the panel is the
 * only instrument available, and eight combinations is a short list.
 *
 * Each press advances one combination, re-initialises, and paints white so
 * there is something to see. Watch the glass, under a lamp.
 */
/*
 * Starts at 1, not 0. Combination 0 is the driver's default - the one that is
 * already failing - so beginning there wastes a press and reads like nothing
 * happened.
 */
static int quirkCombo;

static void nextQuirk()
{
    if (!have_cfg) {
        Serial.println("no config");
        return;
    }

    quirkCombo = (quirkCombo % 7) + 1;
    cfg.quirk_invert_frame = (quirkCombo & 1) ? 1 : 0;
    cfg.quirk_invert_cs    = (quirkCombo & 2) ? 1 : 0;
    cfg.quirk_ctrl_bit4    = (quirkCombo & 4) ? 1 : 0;

    Serial.println();
    Serial.print("combination ");
    Serial.print(quirkCombo);
    Serial.print("/7: frame bit ");
    Serial.print(cfg.quirk_invert_frame ? "INVERTED" : "as read");
    Serial.print(", CS bit ");
    Serial.print(cfg.quirk_invert_cs ? "INVERTED" : "as read");
    Serial.print(", CTRL bit 4 ");
    Serial.println(cfg.quirk_ctrl_bit4 ? "SET" : "left alone");

    sl6806_lcd_bus_register(NULL);
    if (sl6806_lcdc_begin(&cfg) != 0) {
        Serial.println("  begin() failed");
        return;
    }
    if (!Screen.begin(band, BAND_W, BAND_H)) {
        Serial.println("  Screen.begin() failed");
        return;
    }
    /* Flash rather than settle on one colour: a change is far easier to
     * catch under a lamp than a static shade. */
    for (int i = 0; i < 3; i++) {
        Screen.fill(SL6806_WHITE);
        Screen.display();
        delay(40);
        Screen.fill(SL6806_BLACK);
        Screen.display();
        delay(40);
    }
    Screen.fill(SL6806_WHITE);
    Screen.display();

    Serial.println("  init done, flashed white/black 3x, left white.");
    Serial.println("  'm' for the next combination.");
}

/*
 * Flash the panel continuously until a key is pressed.
 *
 * This exists so the lamp test can actually be done. The backlight is a PWM
 * channel nothing here drives, so in ordinary light a working display is
 * indistinguishable from a broken one - you have to look at the glass at an
 * angle under a bright lamp, which takes both hands. Pressing keys and
 * looking at the same time does not work.
 *
 * So: start this, put the monitor down, take the lamp in one hand and the
 * device in the other, and look. Full white against full black, twice a
 * second, is the largest change this panel can make.
 */
static bool flashing;
static uint32_t flashNext;
static bool flashWhite;

static void flashTick()
{
    if (!flashing || (int32_t)(millis() - flashNext) < 0)
        return;
    flashNext = millis() + 250;
    flashWhite = !flashWhite;
    Screen.fill(flashWhite ? SL6806_WHITE : SL6806_BLACK);
    Screen.display();
}

/*
 * Clear the status flags the vendor's ISR clears, and see.
 *
 * Our status register sits at 0x08020000 - bit 27 latched from the first
 * transfer onward and never cleared, because this driver polls and the flag
 * is cleared by an interrupt handler we do not run. The vendor is never in
 * that state.
 */
static void toggleStatusClear()
{
    if (!have_cfg) { Serial.println("no config"); return; }

    cfg.quirk_clear_status = !cfg.quirk_clear_status;
    Serial.println();
    Serial.print("status bits 27 and 22 now ");
    Serial.println(cfg.quirk_clear_status ? "CLEARED after each transfer"
                                          : "left alone (the default)");
    reinit(cfg.quirk_clear_status ? "with vendor status clearing"
                                  : "without status clearing");
    hex32("  STATUS now ", sl6806_mmio_read(SL6806_LCDC_BASE + SL6806_LCDC_STATUS));
    Screen.fill(SL6806_WHITE);
    Screen.display();
    Serial.println("  white pushed - press F for the lamp test");
}

/*
 * Continuous traffic, for someone with a scope or logic analyser.
 *
 * Everything software can see says the controller is working; the panel says
 * otherwise; and the pad input buffers are off in alternate-function mode, so
 * software cannot look at the wires. That leaves a probe on the panel flex,
 * and a probe needs a signal that is there long enough to find. This pushes
 * pixels back to back until a key is pressed.
 *
 * What to look for, on bank 1 pins 1-6 and 8: a clock burst, and chip select
 * framing it. If there is no clock, the controller is not driving these pads
 * and the pin mux is wrong. If there is a clock, the bus works and the panel
 * or its supply is the problem.
 */
static bool scoping;

static void scopeTick()
{
    if (!scoping)
        return;
    Screen.fill(SL6806_WHITE);
    Screen.display();
    Screen.fill(SL6806_BLACK);
    Screen.display();
}

static void menu()
{
    Serial.println();
    Serial.println("keys:");
    Serial.println("  w k r g b   fill white / black / red / green / blue");
    Serial.println("  d           dump registers");
    Serial.println("  t           transfer statistics");
    Serial.println("  x           pulse the panel reset line");
    Serial.println("  i           re-run the panel init sequence");
    Serial.println("  s           toggle pixel byte swap, then re-init");
    Serial.println("  c           module clock 0x910 <-> 0x310, re-init");
    Serial.println("  1           send one command by hand (SLPOUT)");
    Serial.println("  p           pad drive test: do the pins move at all?");
    Serial.println("  P           can the input register even see an LCD pad?");
    Serial.println("  q           read back from the panel");
    Serial.println("  m           next of the 8 inferred-bit combinations");
    Serial.println("  F           flash white/black forever (for the lamp test)");
    Serial.println("  e           clear status bits 27/22 as the vendor ISR does");
    Serial.println("  T           hammer the bus continuously (for a scope)");
    Serial.println();
    Serial.println("panel power (no PWM driver; these are the firmware's own");
    Serial.println("output pads, top candidate first):");
    Serial.println("  l           arm the next candidate and name it");
    Serial.println("  y / u       drive the armed candidate on / off");
    Serial.println("  A / Z       all candidates on / off");
    Serial.println("  h           this menu");
}

void setup()
{
    Serial.begin(115200);
    Serial.println();
    Serial.println("=== SL6806 LCD probe ===");
    Serial.println();
    Serial.println("NOTE: the backlight is a separate PWM channel and nothing");
    Serial.println("here turns it on. Shine a lamp at the panel while you");
    Serial.println("switch between 'w' and 'k' before blaming the driver.");
    Serial.println("Then try 'A': the panel's power rails may also be off.");
    Serial.println();

    /* Screen.begin() is what brings the controller up. */
    bool ok = Screen.begin(band, BAND_W, BAND_H);

    Serial.print("Screen.begin: ");
    Serial.println(ok ? "ok" : "FAILED");
    Serial.print("bus: ");
    Serial.println(sl6806_lcd_bus() ? sl6806_lcd_bus()->name : "none registered");
    Serial.print("timed out: ");
    Serial.println(sl6806_lcdc_timed_out ? "YES" : "no");

    const sl6806_lcdc_config_t *c = sl6806_lcdc_config();
    if (c) {
        cfg = *c;
        have_cfg = true;
        Serial.print("modclk 0x");
        Serial.print(cfg.modclk, HEX);
        Serial.print(", swap ");
        Serial.print(cfg.swap_pixel_bytes);
        Serial.print(", opcodes 0x");
        Serial.print(cfg.cmd_opcode, HEX);
        Serial.print("/0x");
        Serial.println(cfg.pixel_opcode, HEX);
    } else {
        Serial.println("no config: the controller never started");
    }

    Serial.print("delay cap: ");
    Serial.print(sl6806_block_limit());
    Serial.println(" ms (panel waits ignore it - see LcdBus.c)");

    dumpStats("bring-up + panel init");
    dumpRegisters();
    menu();
}

void loop()
{
    int ch = Serial.read();

    if (ch < 0) {
        flashTick();
        scopeTick();
        return;
    }

    if (flashing || scoping) {
        flashing = scoping = false;
        Serial.println("stopped");
        return;
    }

    switch (ch) {
    case 'w': fill(SL6806_WHITE, "white"); break;
    case 'k': fill(SL6806_BLACK, "black"); break;
    case 'r': fill(SL6806_RED,   "red");   break;
    case 'g': fill(SL6806_GREEN, "green"); break;
    case 'b': fill(SL6806_BLUE,  "blue");  break;

    case 'd': dumpRegisters(); break;
    case 't': dumpStats("since reset"); sl6806_lcdc_stats_reset(); break;

    case 'x': {
        const sl6806_lcd_bus_t *bus = sl6806_lcd_bus();
        if (!bus || !bus->reset) { Serial.println("no reset()"); break; }
        Serial.println("reset: high 10ms / low 20ms / high 120ms");
        bus->reset(1); delay(10);
        bus->reset(0); delay(20);
        bus->reset(1); delay(120);
        Serial.println("  done");
        break;
    }

    case 'i': {
        extern const sl6806_dcs_panel_t sl6806_p20_panel;
        sl6806_lcdc_stats_reset();
        Serial.print("init sequence: ");
        Serial.println(sl6806_dcs_begin(&sl6806_p20_panel) == 0 ? "ok" : "failed");
        dumpStats("  init");
        break;
    }

    case 's':
        if (!have_cfg) { Serial.println("no config"); break; }
        cfg.swap_pixel_bytes = !cfg.swap_pixel_bytes;
        reinit(cfg.swap_pixel_bytes ? "byte swap on" : "byte swap off");
        break;

    case 'c':
        if (!have_cfg) { Serial.println("no config"); break; }
        cfg.modclk = (cfg.modclk == 0x910u) ? 0x310u : 0x910u;
        reinit(cfg.modclk == 0x910u ? "module clock 0x910"
                                    : "module clock 0x310 (bootloader's)");
        break;

    case '1': {
        const sl6806_lcd_bus_t *bus = sl6806_lcd_bus();
        if (!bus) { Serial.println("no bus"); break; }
        sl6806_lcdc_stats_reset();
        bus->command(0x11, NULL, 0);   /* SLPOUT: harmless, 4 bytes */
        dumpStats("  one command (4 transfers expected)");
        break;
    }

    case 'l': announceCandidate(); break;
    case 'y': driveArmed(1); break;
    case 'u': driveArmed(0); break;
    case 'A': powerOnAll(1); break;
    case 'Z': powerOnAll(0); break;

    case 'p': padTest(); break;
    case 'P': inputBufferTest(); break;
    case 'm': nextQuirk(); break;
    case 'e': toggleStatusClear(); break;

    case 'T':
        Serial.println("hammering the bus until you press a key.");
        Serial.println("Scope bank 1 pins 1-6 and 8: look for a clock burst");
        Serial.println("and a chip select framing it. No clock means the mux");
        Serial.println("is wrong; a clock means the panel end is the problem.");
        scoping = true;
        break;

    case 'F':
        Serial.println("flashing white/black at 2 Hz until you press a key.");
        Serial.println("Put the monitor down. Hold a bright lamp at an angle");
        Serial.println("to the glass and look for the panel changing shade.");
        Serial.println("This is the test that says whether the driver works.");
        flashing = true;
        flashNext = millis();
        break;
    case 'q': readTest(); break;

    case 'h': menu(); break;
    default: break;
    }
}
