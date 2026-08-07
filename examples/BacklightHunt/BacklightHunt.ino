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
static const cand_t cands[] = {
    { 0x00017618u, "bank 1 pin 14  alt 12  (pair with pin 15)" },
    { 0x00017E18u, "bank 1 pin 15  alt 12  (pair with pin 14)" },
    { 0x000512B0u, "bank 5 pin 2   alt 5   (lone function)" },
    { 0x00016F08u, "bank 1 pin 13  alt 14" },
    { 0x00021705u, "bank 2 pin 2   alt 14" },
    { 0x000157B0u, "bank 1 pin 10  off in firmware" },
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

#define HOLD_MS  2500u
#define FLASH_MS  400u

static int cursor = -1;
static bool announced;
static uint32_t holdUntil, flashNext;
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
    uint32_t now = millis();

    /* Keep the panel changing, so a backlight coming on is unmissable and
     * tells us whether the picture is right at the same time. */
    if ((int32_t)(now - flashNext) >= 0) {
        flashNext = now + FLASH_MS;
        white = !white;
        Screen.fill(white ? SL6806_WHITE : SL6806_BLACK);
        Screen.display();
    }

    if ((int32_t)(now - holdUntil) < 0)
        return;                 /* still holding the current candidate */

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

    holdUntil = now + HOLD_MS;
    announced = false;
}
