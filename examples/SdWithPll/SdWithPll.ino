/*
 * SdWithPll - the last software hypothesis: the SD host wants the PLL.
 *
 * ===================================================================
 *  WHY THIS IS THE ONE LEFT
 * ===================================================================
 * Seven hypotheses about the SD host are closed by measurement
 * (docs/SD.md): the gates, the clock into the block, every writable CTRL
 * bit, whether the block is gated, whether its core runs, the pad set, and a
 * storage mux. What survives is that a payload runs in **bootloader mode**,
 * and two things measured this session say that state is genuinely different
 * rather than merely suspicious:
 *
 *   - the mask ROM does not touch the SD host there - cold registers read
 *     zero and writes are dropped until a payload gates it;
 *   - it does not touch the serial port there either.
 *
 * And **nothing has ever started the PLL**. The bootloader's hardware_init
 * sets it to 384 MHz before it touches any peripheral (notes 25); a payload
 * has never done it. If the SD command state machine is fed from a domain
 * that PLL drives, a payload is asking it to shift 48 bits out with nothing
 * to shift them on - which is exactly the measured behaviour: the command
 * register takes the word, the busy bit drops inside 300 ns, and the status
 * register never says anything.
 *
 *     make SKETCH=examples/SdWithPll RUN_MODE=poll run
 *
 * ===================================================================
 *  IT MAY TAKE THE CONSOLE WITH IT
 * ===================================================================
 * Nothing establishes where the boot ROM clocks USB from. Changing the PLL
 * while USB is enumerated may end the link, so this prints everything it can
 * before touching it, flushes, and then reports whether it still has a
 * console afterwards - which is itself a measurement nobody has taken.
 *
 * Recoverable by unplugging. Nothing here writes flash.
 */

#include <Arduino.h>

extern "C" {
#include "sl6806_hwinit.h"
#include "sl6806_sd.h"
#include "sl6806_mmio.h"
}

static int phase;
static bool done;

static void printHex(uint32_t v)
{
    static const char *hex = "0123456789ABCDEF";

    Serial.print("0x");
    for (int i = 7; i >= 0; i--)
        Serial.print(hex[(v >> (i * 4)) & 0xF]);
}

/* Bring the controller up and send CMD0. Returns 1 if it completed. */
static int cmd0(uint32_t *sta_out)
{
    unsigned n;

    if (sl6806_sd_controller_begin(0u, SL6806_SD_CLKCR_IDENT) != SL6806_SD_OK) {
        *sta_out = 0;
        return 0;
    }
    sl6806_mmio_write(SL6806_SD_BASE + SL6806_SD_STA, SL6806_SD_STA_CLEAR_ALL);
    sl6806_mmio_write(SL6806_SD_BASE + SL6806_SD_ARG, 0);
    sl6806_mmio_write(SL6806_SD_BASE + SL6806_SD_CMD, 0x8000A400u);

    for (n = 0; n < 20000u; n++)
        if (sl6806_mmio_read(SL6806_SD_BASE + SL6806_SD_STA)
            & SL6806_SD_STA_CMDDONE)
            break;

    *sta_out = sl6806_mmio_read(SL6806_SD_BASE + SL6806_SD_STA);
    return (*sta_out & SL6806_SD_STA_CMDDONE) ? 1 : 0;
}

void setup()
{
    Serial.begin(115200);
    Serial.println();
    Serial.println("=== SL6806: the SD host, with the PLL running ===");
    Serial.flush();
    phase = 0;
    done = false;
}

void loop()
{
    uint32_t sta;

    if (done)
        return;

    switch (phase) {

    case 0:
        Serial.println();
        Serial.println("--- before ---");
        Serial.print("  PLL status ");
        printHex(sl6806_mmio_read(SL6806_PLL_STATUS));
        Serial.print("   decoded ");
        Serial.print(sl6806_pll_hz() / 1000000u);
        Serial.println(" MHz");
        Serial.print("  CMD0: ");
        Serial.println(cmd0(&sta) ? "COMPLETED" : "no");
        Serial.print("  STA ");
        printHex(sta);
        Serial.println();
        Serial.println();
        Serial.println("  Setting the PLL to 384 MHz now. If the console stops");
        Serial.println("  here, USB was clocked from what just changed - which");
        Serial.println("  is worth knowing. Unplug and power-cycle.");
        Serial.flush();
        delay(50);
        phase++;
        return;

    case 1: {
        int ok = sl6806_pll_set_384();

        Serial.println();
        Serial.println("--- after ---");
        Serial.println("  the console survived the PLL change.");
        Serial.print("  PLL status ");
        printHex(sl6806_mmio_read(SL6806_PLL_STATUS));
        Serial.print("   decoded ");
        Serial.print(sl6806_pll_hz() / 1000000u);
        Serial.print(" MHz   ");
        Serial.println(ok ? "as asked" : "NOT the 384 MHz asked for");
        Serial.flush();
        phase++;
        return;
    }

    default:
        Serial.print("  CMD0 with the PLL up: ");
        Serial.println(cmd0(&sta) ? "*** COMPLETED ***" : "no");
        Serial.print("  STA ");
        printHex(sta);
        Serial.println();
        Serial.println();
        Serial.println("  A completed CMD0 here means bootloader mode was");
        Serial.println("  withholding the PLL and nothing else, and the SD");
        Serial.println("  driver needs one more line in its bring-up. Still");
        Serial.println("  'no' means the PLL is not it either, and what is");
        Serial.println("  left is an instrument on the socket.");
        Serial.println("done.");
        done = true;
        return;
    }
}
