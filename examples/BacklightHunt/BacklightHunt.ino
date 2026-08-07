/*
 * BacklightHunt - find the pin that lights the panel.
 *
 * The stock firmware lights this screen brightly, so the panel, its supply
 * and its backlight all work. This framework never turns the backlight on:
 * the vendor drives it from a PWM channel (/dev/pwm_ch3, 48 kHz, 60%) whose
 * driver is SRAM-resident and in no dump, and unlike the LCD writers there is
 * no bootloader copy to disassemble.
 *
 * That does not matter much, because a backlight almost never needs PWM to be
 * *on*. It sits behind an enable pin, and PWM only dims it - so driving that
 * pin high is full brightness, and the GPIO driver is complete.
 *
 * WHICH PIN. Not by sweeping: driving all 192 pads wedges the device, because
 * something USB needs is among them. tools/sl6806-padscan lists every pad the
 * stock firmware configures, and a backlight enable has to be one of the pads
 * it puts on an alternate function on its own. That is the list below - a
 * dozen candidates, not two hundred.
 *
 * HOW TO USE IT. Upload, then watch the screen. Each candidate is announced,
 * then driven high for two and a half seconds, then put back. When the panel
 * lights, the last pad named in the log is the answer.
 *
 * While hunting, the display is driven with a slow white/black flash. So if
 * the backlight comes on and the picture flashes, both halves work and the
 * LCD driver has been correct all along. If it comes on and the panel is a
 * uniform shade, the backlight is found and the driver still has a bug.
 * Either outcome is worth more than everything software-only has produced.
 *
 * Pads deliberately NOT touched: bank 1 pins 1-8 (the panel's own bus), bank
 * 1 pin 17 (configured as an input by the firmware, so something external
 * drives it and driving it back would be contention), and bank 4's sixteen
 * function-2 pads (a bus, most likely storage).
 */

#include <Arduino.h>
#include "sl6806_padctl.h"
#include "sl6806_lcdc.h"

static sl6806_color_t band[240 * 8];

typedef struct {
    uint32_t id;          /* the id the firmware uses, for restoring */
    const char *what;
} cand_t;

/*
 * Every pad the stock firmware configures that could plausibly gate a
 * backlight: the lone alternate-function pads, plus the plain outputs that
 * have already been tried without success, kept so one pass covers
 * everything.
 */
/*
 * ELIMINATED on hardware, held high for ~19 s each and watched, twice:
 *   0x00017618  bank 1 pin 14  alt 12
 *   0x00017E18  bank 1 pin 15  alt 12
 *   0x000512B0  bank 5 pin 2   alt 5
 *   0x00016F08  bank 1 pin 13  alt 14
 *   0x00021705  bank 2 pin 2   alt 14
 *   0x000157B0  bank 1 pin 10  off in firmware
 * Left out of the list so a cycle over what remains fits in one monitor run.
 */
static const cand_t cands[] = {
    { 0x00030DBBu, "bank 3 pin 1   alt 11" },
    { 0x000315BBu, "bank 3 pin 2   alt 11" },
    { 0x00031DBBu, "bank 3 pin 3   alt 11" },
    { 0x000325B8u, "bank 3 pin 4   alt 11" },
    { 0x00032DABu, "bank 3 pin 5   alt 11" },
    { 0x000335B0u, "bank 3 pin 6   alt 11" },
    { 0x0001A0D0u, "bank 1 pin 20  vcomo    (already tried)" },
    { 0x000300C0u, "bank 3 pin 0   output   (already tried)" },
    { 0x000508C0u, "bank 5 pin 1   output   (already tried)" },
    { 0x00047080u, "bank 4 pin 14  output   (already tried)" },
    { 0x00047880u, "bank 4 pin 15  output   (already tried)" },
};
#define NCANDS ((int)(sizeof(cands) / sizeof(cands[0])))

/*
 * COUNT POLLS, NOT MILLISECONDS.
 *
 * millis() is useless for pacing here and two attempts were wasted before
 * that was obvious. loop() is driven by USB polls; smtlink_dump spawns a
 * process per poll, so it runs about once a second; and the 24-bit SysTick
 * wraps every 262 ms with wraps only accumulated on read, so most of the
 * elapsed time is simply lost. A nominal 2500 ms hold measured 18 s of wall
 * clock, and dropping it to 800 ms changed nothing, because the limit was
 * never the clock.
 *
 * Holding for a fixed number of loop() calls is predictable: at roughly one
 * poll per second, HOLD_POLLS of 3 gives about three seconds per pad and puts
 * a whole 17-pad cycle inside a single one-minute monitor run - which matters,
 * because a monitor that outlives the foreground limit gets backgrounded, and
 * a second one started alongside it wedges the device off the bus.
 */
/*
 * PACED BY THE HOST, NOT BY THIS DEVICE.
 *
 * Neither clock here can time a candidate. millis() loses SysTick wraps
 * between polls, and counting polls fails too because the poll rate is not a
 * constant: measured at roughly 0.3 per second after a long session, and 42
 * per second right after a cold boot - a factor of a hundred, which turned a
 * three-second dwell into either 9 seconds or 70 milliseconds.
 *
 * So this advances only when a byte arrives on Serial, and the host sends one
 * every few seconds (sl6806-monitor --tick). The dwell is then exact and the
 * log still names the pad, whatever the link is doing.
 */

static int cursor = -1;
static bool announced;
static bool white;

static void release(int i)
{
    if (i >= 0)
        sl6806_pad_configure(cands[i].id);   /* back to the firmware's setting */
}

void setup()
{
    Serial.begin(115200);
    Serial.println();
    Serial.println("=== SL6806 backlight hunt ===");
    Serial.println("Watch the screen. Each pad is named, then driven high for");
    Serial.println("2.5 s, then put back. When the panel lights, the pad named");
    Serial.println("just before is the one.");
    Serial.println();

    if (!Screen.begin(band, 240, 8))
        Serial.println("WARNING: no panel - the flash below will do nothing");
    else
        Serial.println("lcd bus up; flashing white/black while we hunt");
    Serial.println();
}

void loop()
{
    /* Flip every poll, so a backlight coming on is unmissable and says at the
     * same time whether the picture is right. */
    white = !white;
    Screen.fill(white ? SL6806_WHITE : SL6806_BLACK);
    Screen.display();

    /* Advance only when the host says so. */
    if (Serial.read() < 0)
        return;

    if (!announced) {
        int next = (cursor + 1) % NCANDS;

        Serial.print("[");
        Serial.print(next);
        Serial.print("/");
        Serial.print(NCANDS - 1);
        Serial.print("] driving ");
        Serial.println(cands[next].what);
        announced = true;
        return;                 /* let the host drain that line before we act */
    }

    release(cursor);
    cursor = (cursor + 1) % NCANDS;

    /* Function 1 is output; keep the pad's own drive and pull settings. */
    sl6806_pad_configure((cands[cursor].id & ~0x00000780u) | (1u << 7) | (1u << 6));

    announced = false;
}
