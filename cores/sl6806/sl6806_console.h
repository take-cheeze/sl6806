/*
 * sl6806_console.h - bidirectional USB serial for SL6806.
 *
 * HOW THIS IS A SERIAL PORT
 * The SL6806 has no known debug UART, and in payload mode the boot ROM owns
 * the only USB link. So "Serial" is a pair of ring buffers in the sketch's
 * RAM, serviced by a vendor SCSI command that the sketch itself answers from
 * inside the ROM's USB loop. The host drives it with the stock smtlink_dump -
 * nothing custom is needed on the ROM side, and nothing here can break the
 * link the sketch was loaded over.
 *
 * TRANSMIT (device -> host) is one USB round trip per poll. The host issues
 * read_mem2 against the sentinel address SL6806_CONSOLE_POLL_ADDR; the
 * sketch recognises it and answers with a packet of queued output instead of
 * memory contents, consuming it in the same operation. That makes the read
 * atomic and lets the device report exactly how many bytes were lost if the
 * host polled too slowly - a plain memory read can do neither.
 *
 * RECEIVE (host -> device) uses write_mem into the rx ring followed by an
 * update of rx_head, so Serial.read() works and the monitor is interactive.
 *
 * The buffers live in .bss, so the linker picks the address and the build
 * exports it to build/<sketch>.sym. No address is hardcoded on either side.
 */
#ifndef SL6806_CONSOLE_H
#define SL6806_CONSOLE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The transmit ring. Must be a power of two.
 *
 * THIS WAS 2048 FOR MOST OF THIS PROJECT'S LIFE, AND IT SHOULD NOT HAVE
 * BEEN. The ring overwrites rather than blocking - deliberately, because a
 * sketch must not stall when nobody is running the monitor - and in
 * RUN_MODE=poll the host drains it only between loop() calls. So any sketch
 * that printed more than two kilobytes without returning lost the beginning
 * of it, which is reliably the register dump you wanted.
 *
 * That cost nine runs in the SD investigation, each opening with
 * `[lost output - device outran the poll rate]`, and it produced four
 * sketches written as state machines that emit one section per USB poll
 * purely to work around it (examples/BtProbe, SdLife, PadMap, SdFiles). A
 * serial port was decoded and driven partly to escape it (docs/UART.md).
 *
 * The ring is in .bss, which the payload image does not carry - `objcopy -R
 * .bss` - and a payload has roughly 650 KB of SRAM to play with. Sixty-four
 * kilobytes is about a thousand lines: more than any sketch here prints, and
 * about 10% of the heap. The upload does not get bigger. Nothing else had to
 * change, because the header publishes its own size and tools/sl6806-monitor
 * reads it from there.
 *
 * Override with `make CONSOLE_SIZE=...` if a sketch needs more, or less.
 */
#ifndef SL6806_CONSOLE_SIZE
#define SL6806_CONSOLE_SIZE 65536
#endif

#define SL6806_CONSOLE_RX_SIZE 256  /* receive ring; must be a power of two */

#define SL6806_CONSOLE_MAGIC   0x36384C53u   /* "SL68" little-endian */
#define SL6806_CONSOLE_VERSION 2u

/*
 * Sentinel "address" that means "give me console output" rather than "read
 * this memory". Chosen as ASCII "CONS" so it is obvious in a USB trace, and
 * far outside any valid SL6806 address, so a real read can never collide
 * with it.
 */
#define SL6806_CONSOLE_POLL_ADDR 0x434F4E53u

/* One poll response. Exactly the 64 bytes the ROM's return buffer holds. */
#define SL6806_CONSOLE_POLL_MAX 60

typedef struct {
    uint8_t  len;        /* data bytes in this packet (0..60)        */
    uint8_t  flags;      /* SL6806_CONSOLE_F_*                        */
    uint16_t pending;    /* bytes still queued after this packet      */
    uint8_t  data[SL6806_CONSOLE_POLL_MAX];
} sl6806_console_pkt_t;

#define SL6806_CONSOLE_F_OVERFLOW 0x01u   /* output was lost since last poll */

typedef struct {
    uint32_t magic;                  /* SL6806_CONSOLE_MAGIC */
    uint32_t size;                   /* SL6806_CONSOLE_SIZE  */
    uint32_t version;                /* SL6806_CONSOLE_VERSION */
    volatile uint32_t head;          /* tx bytes ever written    (monotonic) */
    volatile uint32_t tail;          /* tx bytes ever consumed   (monotonic) */
    volatile uint32_t dropped;       /* tx bytes lost to overflow            */
    volatile uint32_t rx_head;       /* rx bytes ever written by host        */
    volatile uint32_t rx_tail;       /* rx bytes ever consumed by sketch     */
    uint8_t  tx[SL6806_CONSOLE_SIZE];
    uint8_t  rx[SL6806_CONSOLE_RX_SIZE];
} sl6806_console_t;

extern sl6806_console_t _sl6806_console;

void   sl6806_console_init(void);
void   sl6806_console_write(const uint8_t *buf, size_t len);

/*
 * A second destination for everything the console prints.
 *
 * WHY THIS EXISTS. The ring above overwrites rather than blocking, and in
 * RUN_MODE=poll the host drains it only between loop() calls - so a sketch
 * that prints more than about two kilobytes without returning loses the
 * beginning of it. Nine runs in the SD investigation opened with
 * `[lost output - device outran the poll rate]`, and the standing workaround
 * is to restructure a probe as a state machine so it emits one section per
 * poll (examples/BtProbe, SdLife, PadMap).
 *
 * A sink that writes to the serial port at 0x40091000 removes the problem
 * rather than working around it: the port has no ring to overflow, and it
 * keeps working when the USB handler is wedged - the one failure the USB
 * console cannot report. sl6806_uart_console_mirror() installs exactly that.
 *
 * The sink is called with the bytes as they are written, before they go into
 * the ring, and must not print: it is called from inside Serial.print(). NULL
 * removes it.
 */
typedef void (*sl6806_console_sink_fn)(const uint8_t *buf, size_t len);
void   sl6806_console_set_sink(sl6806_console_sink_fn fn);
void   sl6806_debug_print(const char *s);

/* Free space in the transmit ring, in bytes. */
int    sl6806_console_space(void);

/* Fill `pkt` with queued output and consume it. Returns bytes placed in the
 * packet. Called from the USB command handler. */
int    sl6806_console_poll(sl6806_console_pkt_t *pkt);

/* Host -> device input, behind Serial.read()/available(). */
int    sl6806_console_available(void);
int    sl6806_console_read(void);
int    sl6806_console_peek(void);

#ifdef __cplusplus
}
#endif

#endif /* SL6806_CONSOLE_H */
