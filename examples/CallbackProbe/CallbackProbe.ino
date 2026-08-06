/*
 * CallbackProbe - find out which ROM USB callbacks your unit actually calls.
 *
 * WHY
 * A payload registers a table of callbacks with the boot ROM. Only two slots
 * have ever had established meaning: scsi_cb, which is verified because the
 * console depends on it, and cb2, which was *inferred* to be a periodic idle
 * hook because the stock payload installs a do-nothing function there and the
 * ROM stays healthy. That inference is how loop() gets driven in the default
 * build - and on at least one real unit it is wrong: the sketch prints
 * setup()'s output and then never ticks.
 *
 * This sketch settles it. It installs a distinct counter in every slot and
 * prints how often each one fired. Whatever the ROM calls, you will see
 * counting up.
 *
 *   make SKETCH=examples/CallbackProbe RUN_MODE=poll run
 *
 * RUN_MODE=poll is not optional here. It drives loop() from scsi_cb, which is
 * the one callback known to fire - if the probe relied on cb2 and cb2 is the
 * thing under test, a silent result would be ambiguous.
 *
 * READING THE OUTPUT
 * Every line is one slot and its call count. `polls` is scsi_cb and is your
 * baseline: it goes up once per monitor poll, so it proves the counting works.
 *
 *   - A slot climbing with polls, or faster: the ROM calls it. If it is cb0-cb5
 *     or cb8, that is a better hook than polling and worth reporting - the
 *     default RUN_MODE=hook could use it instead.
 *   - Every slot except polls stuck at 0: this ROM revision calls none of
 *     them, and RUN_MODE=poll is the only way to drive loop().
 *
 * Either answer is worth having. The second is what one unit showed.
 */

#include <Arduino.h>
#include <sl6806_rom.h>   /* the callback table this probe fills in */

static volatile uint32_t hits[9];   /* cb0..cb5, scsi, spare, cb8 */
static uint32_t polls;

/* One counter per slot. They must be separate functions so the ROM calling
 * any single one is distinguishable. */
static int cb0(void) { hits[0]++; return 0; }
static int cb1(void) { hits[1]++; return 0; }
static int cb2(void) { hits[2]++; return 0; }
static int cb3(void) { hits[3]++; return 0; }
static int cb4(void) { hits[4]++; return 0; }
static int cb5(void) { hits[5]++; return 0; }
static int cb8(void) { hits[8]++; return 0; }

/*
 * Called by the core just before the table is handed to the ROM. scsi_cb is
 * already set to the console handler and must stay that way, or there is no
 * way to report anything.
 */
extern "C" void sl6806_usb_userfn_init(sl6806_usb_userfn_t *fn)
{
    fn->cb0 = cb0;
    fn->cb1 = cb1;
    fn->cb2 = cb2;
    fn->cb3 = cb3;
    fn->cb4 = cb4;
    fn->cb5 = cb5;
    fn->cb8 = cb8;
}

void setup()
{
    Serial.begin(115200);
    Serial.println();
    Serial.println("=== SL6806 ROM callback probe ===");
    Serial.println("counters for every slot in sl6806_usb_userfn_t.");
    Serial.println("'polls' is scsi_cb - the known-good baseline.");
    Serial.println();
}

void loop()
{
    static const char *kNames[] = {
        "cb0", "cb1", "cb2", "cb3", "cb4", "cb5", "-", "-", "cb8"
    };

    polls++;

    /* One line per report, so the monitor stays readable. Print only every
     * few polls: loop() runs inside the USB handler in poll mode, and a long
     * write would stall the transaction it is running in. */
    if (polls % 8)
        return;

    Serial.print("polls=");
    Serial.print(polls);
    for (unsigned i = 0; i < sizeof(hits) / sizeof(hits[0]); i++) {
        if (kNames[i][0] == '-')
            continue;
        Serial.print("  ");
        Serial.print(kNames[i]);
        Serial.print("=");
        Serial.print(hits[i]);
    }
    Serial.println();

    if (polls == 8) {
        Serial.println();
        Serial.println("If every slot stays 0 while polls climbs, this ROM");
        Serial.println("calls none of them and RUN_MODE=poll is the only way");
        Serial.println("to drive loop(). If one climbs, say which - it is a");
        Serial.println("better hook than polling.");
        Serial.println();
    }
}
