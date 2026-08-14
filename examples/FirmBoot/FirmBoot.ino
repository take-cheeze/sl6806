/*
 * FirmBoot - start the vendor's application from a payload.
 *
 * ===================================================================
 *  READ THIS FIRST
 * ===================================================================
 * This hands the device to the stock firmware and does not come back. The
 * USB link goes with it - the application re-initialises USB as a card
 * reader - so everything this run has to say is said before it jumps, and
 * what happens afterwards is observed on the device and on the host's USB
 * bus, not in this console.
 *
 * **It writes nothing anywhere persistent.** The application is copied out of
 * the flash image already on the device into SRAM, and entered. A power cycle
 * puts you back in bootloader mode with nothing changed. That is the whole
 * difference between this and MODE=firmware, and it is why this exists first.
 *
 *     make SKETCH=examples/FirmBoot RUN_MODE=poll run
 *
 * ===================================================================
 *  WHY BOTHER
 * ===================================================================
 * To find out what bootloader mode withholds.
 *
 * The SD host takes a command and reports nothing, with seven hypotheses
 * closed by measurement (docs/SD.md): the gates are right, the pads are the
 * product's own, the datapath is clocked, the command register is live.
 * What is left is that a payload runs in bootloader mode and the code that
 * demonstrably reads cards - the bootloader's own sdupdate path - does not.
 * Measured this session: **in bootloader mode the mask ROM does not touch
 * the SD host at all**, and it does not touch the serial port either. Two
 * blocks, both off, both fine once a payload wakes them - and one of them
 * still will not run a command.
 *
 * So: does the application boot, and does the card mount?
 *
 *   - **The panel lights up with the P20 interface.** The application is
 *     running. That alone says the flash image is intact and startable from
 *     a payload, which nothing has established before.
 *   - **The host enumerates 301a:2801, mass storage, and the card mounts.**
 *     The socket, the card and the SD host all work under the vendor's
 *     firmware. The SD question then stops being "is the block broken" and
 *     becomes "what does bootloader mode withhold", which is a much better
 *     question because it has a finite answer.
 *   - **Nothing happens.** The application needs something the bootloader's
 *     hardware_init does that this does not.
 *
 * [M] 2026-08-14, the first run: **it booted.** The panel and the loss of the
 * upload endpoint both said so, which also confirms §7m's load address on
 * hardware. But the application's USB came up unstable - unstable enough that
 * `lsusb` could not be run against it - so the card question is still open,
 * and this sketch now runs the bootloader's clock sequence before handing
 * over. See FIRMBOOT_CLOCKS below.
 *
 * ===================================================================
 *  IF ITS USB IS UNSTABLE, TRY THE CABLE
 * ===================================================================
 * [M] It was, twice - once without the clock sequence and once with it. The
 * likeliest reason has nothing to do with what this sketch configures.
 *
 * **The application is re-initialising USB on a link that is already
 * enumerated.** In a normal boot the host has never seen this device: power
 * comes up, the bootloader runs, the application starts, and only then does
 * anything appear on the bus. Here the boot ROM has been enumerated as
 * `301a:2800` for the whole session, with an open connection and a host-side
 * device object, and the application reconfigures the controller underneath
 * all of that without a disconnect. A host presented with a device that stops
 * answering and starts answering differently, with no detach in between, does
 * exactly what you saw.
 *
 * **So unplug the USB cable after the jump and plug it back in.** This is a
 * player with a battery: the application keeps running, and the host gets a
 * clean enumeration of whatever the device now is. It costs nothing and needs
 * no code, and if `301a:2801` appears afterwards then the instability was the
 * handover and not the firmware.
 *
 * The alternative - detaching USB properly before jumping - needs the USB
 * controller's registers, and this project does not have them: 7l looked at
 * `0x40040000`, called it "most likely a USB controller", and left it there.
 *
 * ===================================================================
 *  WHAT IT CHECKS BEFORE JUMPING
 * ===================================================================
 * The load address is not in the header. The header names an entry, and the
 * load address is that entry less the 0x400-byte vector table - §7m's
 * finding, confirmed there by counting call targets. Getting it wrong copies
 * the image 0x400 bytes off and branches into the middle of a function, and
 * on a Cortex-M that is a hard fault with the USB link already gone: no
 * console, no message, just a device that stopped.
 *
 * So sl6806_firm_parse() derives the load address and then checks it against
 * the image: the segment's word 1 must be the entry the header named, and
 * word 0 must be a plausible SRAM stack pointer. Both hold for the shipped
 * image. 15 host tests cover the refusals.
 */

#include <Arduino.h>

extern "C" {
#include "sl6806_firm.h"
#include "sl6806_hwinit.h"
#include "sl6806_uart.h"
#include "sl6806_audio.h"
#include "sl6806_mmio.h"
}

/*
 * ===================================================================
 *  THE TWO EXTRA STEPS ARE SEPARATELY SELECTABLE, AND BOTH DEFAULT OFF
 * ===================================================================
 * [M] Three results so far:
 *
 *   neither step            boots, and the application's USB is unstable
 *   both steps              does not boot at all
 *
 * "Does not boot" is a worse default than "boots badly", so both are off
 * unless you ask for them. Turning them on one at a time is the experiment,
 * and it takes two runs:
 *
 *     make SKETCH=examples/FirmBoot RUN_MODE=poll run \
 *          EXTRA_FLAGS=-DFIRMBOOT_MODULES_OFF=1
 *
 *     make SKETCH=examples/FirmBoot RUN_MODE=poll run \
 *          EXTRA_FLAGS=-DFIRMBOOT_CLOCK_TREE=1
 *
 * Whichever of those fails to boot is the one that breaks it, and that is
 * worth more than either of them working: it names a step the bootloader
 * performs that a payload cannot survive, which is the shape of the whole
 * remaining question about bootloader mode.
 *
 * **FIRMBOOT_MODULES_OFF** calls ROM 0x3BFC, the first thing hardware_init
 * does: every module clock on the chip off, then a delay. The application
 * expects a chip in that state and a payload hands it one where the boot
 * ROM's USB is enumerated and talking, which is the likeliest reason for the
 * unstable enumeration. Note that the copy above already happened, so flash
 * going quiet no longer matters - that ordering cost one run.
 *
 * **FIRMBOOT_CLOCK_TREE** calls the ROM routines behind 0x008206D0: the
 * source selects, the rate, two ROM-clock disables, two flash-host writes and
 * the four dividers. Any of those could be what a payload cannot survive -
 * the dividers change the core clock this code is executing on.
 */
#ifndef FIRMBOOT_MODULES_OFF
#define FIRMBOOT_MODULES_OFF 0
#endif

/*
 * [M] And it only works in RUN_MODE=takeover. In poll mode loop() runs inside
 * the boot ROM's USB command handler, and switching off USB's module clock
 * there stops the controller in the middle of the transaction executing this
 * code - the device does nothing and prints nothing, which is what three runs
 * looked like. sl6806_hwinit.h has the detail.
 *
 *     make SKETCH=examples/FirmBoot RUN_MODE=takeover run \
 *          EXTRA_FLAGS="-DFIRMBOOT_MODULES_OFF=1 -DFIRMBOOT_CLOCK_TREE=1"
 *
 * There is no console in takeover mode, so nothing below prints. That is
 * acceptable here and only here: the observable was always the panel and the
 * host's USB bus, because the application takes the console regardless.
 *
 * [M] **Expect the upload to end with `unexpected status`, and the monitor to
 * report that the device is not answering.** Both are correct. In takeover
 * mode the payload seizes the CPU inside the ROM's run command, so that
 * command never returns a completion status - `smtlink_dump` says so - and
 * nothing afterwards services USB, so the monitor has nothing to talk to.
 * Neither message says anything about whether the jump worked.
 *
 * The only observables are the device and the bus:
 *
 *     the panel showing the P20 interface   -> the application is running
 *     lsusb -d 301a:2801                    -> and its USB came up
 *     lsusb -d 301a:2800                    -> it fell back to the boot ROM
 *     neither, panel dark                   -> it did not survive the jump
 */
#if FIRMBOOT_MODULES_OFF && defined(SL6806_RUN_MODE) && SL6806_RUN_MODE == 2
#error "FIRMBOOT_MODULES_OFF needs RUN_MODE=takeover; in poll mode loop() runs inside the USB handler this would switch off. See sl6806_hwinit.h."
#endif
#ifndef FIRMBOOT_CLOCK_TREE
#define FIRMBOOT_CLOCK_TREE 0
#endif

/*
 * ===================================================================
 *  DUMPING, AND WHY IT HAS TO GO OUT OF THE UART
 * ===================================================================
 * docs/AUDIO.md ends by asking for the audio block to be dumped **while the
 * stock firmware is really playing**, and then diffed against what this
 * framework produces. That cannot be done over USB: the application
 * re-initialises the controller, so the console and the upload endpoint are
 * gone the instant the jump happens, and any code of ours went with them.
 *
 * But there is a channel that survives, and this project already found it.
 * 26 and 27 identified 0x40091000 as a real UART on bank 1 pin 2, function 6,
 * at 1.5 Mbaud - it is where every `boot--->` line from the HLKJ bootloader
 * comes out. The application is built from the same tree and carries the same
 * printf: FIRM contains `-pwm1_event_callback`, `-brightness percent %d`,
 * `sdio(i):...` and the rest. **So a serial adapter on that pad sees the
 * product's own debug output, live, after the handover.**
 *
 * That makes the dump a two-part observation on one wire:
 *
 *   before the jump - this sketch prints the audio block and its clocks, so
 *                     the "ours" half of the diff is in the same transcript;
 *   after the jump  - whatever the application says for itself.
 *
 * WHAT YOU NEED: a 3.3 V USB-serial adapter, RX to bank 1 pin 2, ground to
 * ground, **1500000 baud**. Nothing else changes; if you have no adapter the
 * dump still goes to the USB console and only the second half is lost.
 *
 *     make SKETCH=examples/FirmBoot RUN_MODE=poll run
 *     # in another terminal, before the countdown ends:
 *     #   picocom -b 1500000 /dev/ttyUSB0
 *
 * Set FIRMBOOT_DUMP=0 to skip it. It only reads registers and configures one
 * pad, so it cannot affect whether the application boots - but the pad write
 * is a write, and this file's convention is that anything touching hardware
 * before the jump can be turned off.
 */
#ifndef FIRMBOOT_DUMP
#define FIRMBOOT_DUMP 1
#endif

/* Seconds of grace before the jump, so the console can be read and the run
 * abandoned by unplugging. */
#define COUNTDOWN 8

static sl6806_firm_t firm;
static int parsed;
static int left = COUNTDOWN;
static uint32_t last;

static void printHex(uint32_t v)
{
    static const char *hex = "0123456789ABCDEF";

    Serial.print("0x");
    for (int i = 7; i >= 0; i--)
        Serial.print(hex[(v >> (i * 4)) & 0xF]);
}

#if FIRMBOOT_DUMP
/* Both destinations, because the UART half is the only one that survives the
 * jump and the USB half is the only one that exists without an adapter. */
static void say(const char *s)
{
    Serial.print(s);
    sl6806_uart_puts(s);
}

static void sayReg(const char *name, uint32_t addr)
{
    static const char hex[] = "0123456789ABCDEF";
    char line[48];
    uint32_t v = sl6806_mmio_read(addr);
    unsigned i, n = 0;

    while (*name && n < 20u)
        line[n++] = *name++;
    while (n < 20u)
        line[n++] = ' ';
    for (i = 0; i < 8u; i++)
        line[n++] = hex[(v >> ((7u - i) * 4u)) & 0xFu];
    line[n++] = '\r';
    line[n++] = '\n';
    line[n] = 0;
    say(line);
}

/*
 * The "ours" half of the diff docs/AUDIO.md asks for. Read-only apart from
 * the UART pad, so it changes nothing the application then has to survive.
 */
static void dumpAudio(const char *when)
{
    say("\r\n--- audio block, ");
    say(when);
    say(" ---\r\n");
    sayReg("+0x000 ctrl",   SL6806_AUD_REG(0x000));
    sayReg("+0x008 DAC",    SL6806_AUD_DAC);
    sayReg("+0x07C src",    SL6806_AUD_REG(0x07C));
    sayReg("+0x080 src",    SL6806_AUD_REG(0x080));
    sayReg("+0x100 TXen",   SL6806_AUD_TX_ENABLE);
    sayReg("+0x104 TXtrig", SL6806_AUD_TX_TRIG);
    sayReg("+0x108 TXctrl", SL6806_AUD_TX_CTRL);
    sayReg("+0x10C TXaddr", SL6806_AUD_TX_ADDR);
    sayReg("+0x200 RXen",   SL6806_AUD_RX_ENABLE);
    sayReg("+0x208 RXctrl", SL6806_AUD_RX_CTRL);
    sayReg("+0x400 EQctrl", SL6806_AUD_EQ_CTRL);
    sayReg("+0x40C EQ?",    SL6806_AUD_REG(0x40C));
    say("--- clocks ---\r\n");
    sayReg("40080010 PLLsel",  SL6806_AUD_PLL_SEL);
    sayReg("40080014 PLLrat",  SL6806_AUD_PLL_RATIO);
    sayReg("40080094 bitclk",  SL6806_AUD_BITCLK_REG);
    sayReg("4009B040",         0x4009B040u);
    sayReg("4009B04C",         0x4009B04Cu);
    sayReg("4009B050",         0x4009B050u);
}
#endif

static const char *why(int err)
{
    switch (err) {
    case SL6806_FIRM_OK:          return "ok";
    case SL6806_FIRM_ERR_HDR:     return "not a FIRM header";
    case SL6806_FIRM_ERR_ENTRY:   return "the entry is not a Thumb SRAM address";
    case SL6806_FIRM_ERR_LENGTH:  return "the length is zero or absurd";
    case SL6806_FIRM_ERR_VECTORS: return "the vector table disagrees with the header";
    default:                      return "unknown";
    }
}

void setup()
{
    int err;

    Serial.begin(115200);
    Serial.println();
    Serial.println("=== SL6806: start the stock application ===");
#if FIRMBOOT_DUMP
    Serial.print("UART on bank 1 pin 2 @1500000: ");
    Serial.println(sl6806_uart_begin(SL6806_UART_BAUD)
                   ? "up - put an adapter there to see past the jump"
                   : "REFUSED - the dump will be USB-only and stop at the jump");
#endif
    Serial.println();
    Serial.println("This does not come back. USB goes away with it, and the");
    Serial.println("device becomes the product. Nothing is written to flash;");
    Serial.println("a power cycle undoes all of it.");
    Serial.println();

    err = sl6806_firm_read(&firm);
    parsed = (err == SL6806_FIRM_OK);

    Serial.println("--- the image in flash ---");
    Serial.print("  parse: ");
    Serial.print(err);
    Serial.print(" (");
    Serial.print(why(err));
    Serial.println(")");

    if (!parsed) {
        Serial.println();
        Serial.println("  Refusing to jump. Nothing was copied.");
        return;
    }

    Serial.print("  build stamp   ");
    printHex(firm.timestamp);
    Serial.println();
    Serial.print("  entry         ");
    printHex(firm.entry);
    Serial.println("   (Thumb)");
    Serial.print("  load address  ");
    printHex(firm.load);
    Serial.println("   (entry less the vector table)");
    Serial.print("  length        ");
    printHex(firm.length);
    Serial.println();
    Serial.print("  initial SP    ");
    printHex(firm.stack);
    Serial.println("   (from vector 0)");
    Serial.println();
    Serial.println("  The vector table agrees with the header, so the load");
    Serial.println("  address is right - see the sketch header for why that");
    Serial.println("  is the thing worth checking.");
    Serial.println();
    Serial.print("  PLL now      ");
    Serial.print(sl6806_pll_hz() / 1000000u);
    Serial.println(" MHz");
    Serial.print("  modules off  ");
    Serial.println(FIRMBOOT_MODULES_OFF ? "yes (ROM 0x3BFC)"
                                        : "no  (-DFIRMBOOT_MODULES_OFF=1)");
    Serial.print("  clock tree   ");
    Serial.println(FIRMBOOT_CLOCK_TREE ? "yes (0x008206D0's sequence)"
                                       : "no  (-DFIRMBOOT_CLOCK_TREE=1)");
    Serial.println();
    Serial.println("--- what to watch after the jump ---");
    Serial.println("  the panel: the P20 interface means it booted");
    Serial.println("  the host:  301a:2801 mass storage means the card works");
    Serial.println();
    Serial.println("  IF USB IS UNSTABLE: unplug the cable and plug it back");
    Serial.println("  in. The application keeps running on the battery, and");
    Serial.println("  the host gets a clean enumeration instead of a device");
    Serial.println("  that changed shape without detaching. See the header.");
    Serial.println();
    Serial.print("Jumping in ");
    Serial.print(COUNTDOWN);
    Serial.println(" seconds. Unplug now to abandon.");
    Serial.flush();

    last = millis();
}

void loop()
{
    if (!parsed)
        return;

    if (millis() - last < 1000u)
        return;
    last = millis();

    if (left > 0) {
        Serial.print("  ");
        Serial.println(left);
        Serial.flush();
        left--;
        return;
    }

    Serial.println();
    Serial.println("going.");
    Serial.flush();

    /* Give the host one more poll to collect that before USB disappears. */
    delay(50);

    /*
     * COPY FIRST. The copy reads XIP flash, and the next step switches off
     * every module clock including the flash controller's. Doing it the other
     * way round copies garbage out of an unclocked controller and branches
     * into it - measured 2026-08-14, and the device simply did nothing.
     */
    {
        int err = sl6806_firm_copy(&firm);

        if (err != SL6806_FIRM_OK) {
            Serial.print("copy refused: ");
            Serial.println(why(err));
            parsed = 0;
            return;
        }
    }

    /*
     * hardware_init's order, with flash no longer needed. Both take the
     * console with them - USB's module clock is among the ninety-six - which
     * is why they come after the flush and immediately before the handover.
     */
#if FIRMBOOT_DUMP
    /*
     * Last thing before the handover, so what is printed is the state the
     * application is actually given - not the state eight seconds earlier.
     */
    dumpAudio("as this payload leaves it");
    say("\r\n--- jumping. anything below this line is the application's"
        " own output ---\r\n");
#endif

#if FIRMBOOT_MODULES_OFF
    sl6806_hwinit_modules_off();
#endif
#if FIRMBOOT_CLOCK_TREE
    sl6806_hwinit_clocks();
#endif

    sl6806_firm_enter(&firm);

    /* Only reached if the entry refused, which parse should have prevented. */
    Serial.println("sl6806_firm_enter() refused - nothing was entered.");
    parsed = 0;
}
