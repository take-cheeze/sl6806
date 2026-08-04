/*
 * sl6806_console.h - Serial console transport for SL6806.
 *
 * HOW OUTPUT GETS OFF THE CHIP
 * The SL6806 has no known, pinned-out debug UART, and in payload mode the
 * only channel to the host is the USB link the boot ROM already owns. So the
 * console is a ring buffer in the sketch's own RAM that the host polls with
 * the stock tool's read_mem command. Nothing custom is needed on the ROM
 * side, and nothing can break the USB link that carries it.
 *
 * The buffer lives in .bss, so its address is decided by the linker, not
 * guessed. The build exports it to build/<sketch>.sym and tools/sl6806-monitor
 * reads it from there - no hardcoded addresses anywhere.
 *
 * `head` is monotonic (never wrapped), so the host can tell the difference
 * between "nothing new" and "wrapped all the way round and lost data".
 */
#ifndef SL6806_CONSOLE_H
#define SL6806_CONSOLE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef SL6806_CONSOLE_SIZE
#define SL6806_CONSOLE_SIZE 2048    /* must be a power of two */
#endif

#define SL6806_CONSOLE_MAGIC 0x36384C53u   /* "SL68" little-endian */

typedef struct {
    uint32_t magic;                  /* SL6806_CONSOLE_MAGIC */
    uint32_t size;                   /* SL6806_CONSOLE_SIZE  */
    volatile uint32_t head;          /* total bytes ever written (monotonic) */
    volatile uint32_t rx_head;       /* host -> device: bytes ever written   */
    volatile uint32_t rx_tail;       /* device consumed up to here           */
    uint32_t version;                /* layout version = 1                   */
    uint8_t  tx[SL6806_CONSOLE_SIZE];
    uint8_t  rx[256];
} sl6806_console_t;

extern sl6806_console_t _sl6806_console;

void   sl6806_console_init(void);
void   sl6806_console_write(const uint8_t *buf, size_t len);
void   sl6806_debug_print(const char *s);

/* Host -> device input, used by Serial.read()/available(). */
int    sl6806_console_available(void);
int    sl6806_console_read(void);
int    sl6806_console_peek(void);

#ifdef __cplusplus
}
#endif

#endif /* SL6806_CONSOLE_H */
