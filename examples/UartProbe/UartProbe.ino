/*
 * UartProbe - is there a serial port on bank 1 pin 2?
 *
 * ===================================================================
 *  WHAT THIS IS FOR
 * ===================================================================
 * The bootloader's printf goes to a block at 0x40091000 (notes §26), and
 * following its sink through the mask ROM gives the whole write path: wait
 * for status bit 4, store nine bits to the data register. Both vendor images
 * bring the block up identically. Nobody has seen a character.
 *
 * If it works, this project gets a console that does not depend on USB
 * polling - which matters because nine runs in the SD investigation opened
 * with `[lost output - device outran the poll rate]`, and because the one
 * failure the USB console cannot report is the one where the USB handler is
 * wedged.
 *
 *     make SKETCH=examples/UartProbe RUN_MODE=poll run
 *
 * ===================================================================
 *  WHAT YOU NEED ON THE BENCH
 * ===================================================================
 * A USB-serial adapter, its RX on **bank 1 pin 2** and its ground on the
 * board's, at **1,500,000 baud, 8N1** - the rate is the vendor's own and the
 * framing is a guess (sl6806_uart.h). Where bank 1 pin 2 comes out on this
 * board is not known; that is a meter-and-magnifier job, and the pad census
 * cannot help because a pad in function 6 is unreadable.
 *
 * If nothing appears, try 115200 and the other common rates before
 * concluding: this driver deliberately does not program the divisor, so the
 * port runs at whatever the ROM left, and the ROM's own rate is what the
 * bootloader configured on the last normal boot - which may not survive
 * entering bootloader mode.
 *
 * ===================================================================
 *  WHAT IT REPORTS ON USB, WHICHEVER WAY IT GOES
 * ===================================================================
 * Everything below is printed to the ordinary console too, so the run is
 * useful even with no adapter attached:
 *
 *   1. the block's registers before anything is touched - a gated block
 *      reads zero, so this says whether the boot ROM left it configured;
 *   2. whether module 73 acknowledges its gate;
 *   3. the registers after bring-up, and whether a write sticks - the same
 *      nonce test SdProbe uses, because "unchanged" means nothing when the
 *      value was already there;
 *   4. whether the status register ever reports transmit-ready, which is the
 *      one bit the whole write path waits on;
 *   5. and then it sends a banner, repeatedly, so a scope has something
 *      periodic to trigger on even if the framing is wrong.
 */

#include <Arduino.h>

extern "C" {
#include "sl6806_uart.h"
#include "sl6806_mmio.h"
#include "sl6806_module.h"
}

static int  phase;
static bool done;
static uint32_t cold[8];

static void printHex(uint32_t v, int digits = 8)
{
    static const char *hex = "0123456789ABCDEF";

    Serial.print("0x");
    for (int i = digits - 1; i >= 0; i--)
        Serial.print(hex[(v >> (i * 4)) & 0xF]);
}

static const struct { const char *name; uint32_t off; } REGS[] = {
    { "DATA   ", SL6806_UART_DATA       },
    { "CTRL   ", SL6806_UART_CTRL       },
    { "STATUS ", SL6806_UART_STATUS     },
    { "IRQEN  ", SL6806_UART_IRQ_ENABLE },
    { "+0x10  ", SL6806_UART_REG10      },
    { "+0x14  ", SL6806_UART_REG14      },
    { "+0x20  ", SL6806_UART_MODE       },
};
#define N_REGS (sizeof(REGS) / sizeof(REGS[0]))

static void dump(const char *what, uint32_t *into)
{
    Serial.print("  ");
    Serial.println(what);
    for (unsigned i = 0; i < N_REGS; i++) {
        uint32_t v = sl6806_mmio_read(SL6806_UART_BASE + REGS[i].off);

        if (into)
            into[i] = v;
        Serial.print("    ");
        Serial.print(REGS[i].name);
        printHex(v);
        if (!into && v != cold[i])
            Serial.print("   <-- changed");
        Serial.println();
    }
}

void setup()
{
    Serial.begin(115200);
    Serial.println();
    Serial.println("=== SL6806 serial port probe, 0x40091000 ===");
    Serial.println("Adapter RX on bank 1 pin 2, ground to ground,");
    Serial.println("1500000 8N1. See the sketch header.");
    Serial.flush();

    phase = 0;
    done = false;
}

void loop()
{
    if (done)
        return;

    switch (phase) {

    case 0:
        Serial.println();
        Serial.println("--- as found ---");
        Serial.print("  module 73 reads ");
        Serial.println(sl6806_module_enabled(SL6806_UART_MODULE_ID)
                       ? "ENABLED" : "disabled");
        Serial.print("  ROM clock 22 (0x400800B4 bit 0) reads ");
        Serial.println((sl6806_mmio_read(SL6806_UART_ROMCLK_REG)
                        & SL6806_UART_ROMCLK_BIT) ? "ENABLED" : "disabled");
        dump("registers before anything is touched:", cold);
        Serial.print("  alive? ");
        Serial.println(sl6806_uart_alive() ? "YES - the ROM left it running"
                                           : "no");
        phase++;
        return;

    case 1: {
        int ok = sl6806_uart_begin(0);

        Serial.println();
        Serial.println("--- after bring-up ---");
        Serial.print("  module 73 gate: ");
        Serial.println(ok ? "acknowledged" : "REFUSED - nothing was written");
        dump("registers now:", NULL);

        /* The nonce test. A gated block reads plausible values and drops
         * writes; "unchanged" proves nothing when the value was already
         * there. This is the mistake SdProbe's first version made. */
        {
            uint32_t saved = sl6806_mmio_read(SL6806_UART_BASE
                                              + SL6806_UART_IRQ_ENABLE);
            uint32_t got;

            sl6806_mmio_write(SL6806_UART_BASE + SL6806_UART_IRQ_ENABLE, 0x2Au);
            got = sl6806_mmio_read(SL6806_UART_BASE + SL6806_UART_IRQ_ENABLE);
            sl6806_mmio_write(SL6806_UART_BASE + SL6806_UART_IRQ_ENABLE, saved);

            Serial.print("  IRQEN <- 0x2A reads ");
            printHex(got);
            Serial.println((got & 0x2Au) ? "   writes stick"
                                         : "   this register does not hold");
            /* Measured 2026-08-14: IRQEN does not hold, while CTRL, +0x10 and
             * +0x14 all did in the dump above. So a nonce test on ONE
             * register cannot say whether a block is gated - it says whether
             * that register is writable. The dump is the real evidence, and
             * the first version of this sketch printed "still gated" on the
             * strength of the wrong one. */
            Serial.println("  (a register that does not hold is not the same");
            Serial.println("   as a gated block - compare the dump above)");
        }

        Serial.print("  transmit-ready bit: ");
        Serial.println((sl6806_uart_status() & SL6806_UART_TX_READY)
                       ? "SET - the write path will proceed"
                       : "clear - every putc will time out");
        Serial.print("  alive? ");
        Serial.println(sl6806_uart_alive() ? "YES" : "no");
        phase++;
        return;
    }

    case 2:
        Serial.println();
        Serial.println("--- sending ---");
        Serial.println("  A banner goes out once a second from here on.");
        Serial.println("  Watch bank 1 pin 2. If the framing is wrong you");
        Serial.println("  will still see periodic activity on a scope, which");
        Serial.println("  is itself the answer to 'is this pin a UART'.");
        Serial.flush();
        phase++;
        return;

    default: {
        static uint32_t last;
        static uint32_t seq;
        int n;

        if (millis() - last < 1000u)
            return;
        last = millis();

        n = sl6806_uart_puts("SL6806 uart probe ");
        n += sl6806_uart_putc((uint8_t)('0' + (seq % 10u)));
        n += sl6806_uart_puts("\n");
        seq++;

        Serial.print("  sent ");
        Serial.print(n);
        Serial.print(" bytes, status ");
        printHex(sl6806_uart_status());
        if (n == 0)
            Serial.print("   (timed out - the port is not transmitting)");
        Serial.println();

        /* Anything typed at the adapter comes back here, which would settle
         * the framing from the other direction. */
        {
            int c = sl6806_uart_getc();

            if (c >= 0) {
                Serial.print("  *** received ");
                printHex((uint32_t)c, 3);
                Serial.println(" ***");
            }
        }
        return;
    }
    }
}
