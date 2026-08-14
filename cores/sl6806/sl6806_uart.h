/*
 * sl6806_uart.h - the serial port at 0x40091000, where the vendor's printf goes.
 *
 * =====================================================================
 *  WHY THIS EXISTS
 * =====================================================================
 * Every hardware session in this project has lost output. The console is a
 * 2 KB ring that overwrites rather than blocking, and in RUN_MODE=poll the
 * host drains it only between loop() calls - so a sketch that prints more than
 * a screenful loses the beginning of it, which is reliably the register dump
 * you wanted. Nine runs in the SD investigation alone opened with
 * `[lost output - device outran the poll rate]`, and the standing workaround
 * is to restructure a probe as a state machine (examples/BtProbe, SdLife,
 * PadMap) so it emits one section per USB poll.
 *
 * That is a workaround for not having a serial port. **This is the serial
 * port**, and it also keeps working when a payload wedges the USB handler -
 * which is the failure every bounded poll in cores/ exists to avoid, and the
 * one case where the existing console cannot report anything at all.
 *
 * =====================================================================
 *  WHERE IT COMES FROM
 * =====================================================================
 * The bootloader's hardware_init configures one pad and then hands a
 * descriptor to ROM 0x260, which installs the sink the ROM's printf writes
 * to (§26). Following that pointer gives the whole register map:
 *
 *     ROM 0x0000023C   putchar: expands '\n' to CR LF, then ROM 0x224
 *     ROM 0x00000224   -> ROM 0x1D0
 *     ROM 0x000001D0   while (!(base[0x08] & 0x10)) ; base[0x00] = c & 0x1FF
 *     ROM 0x000001E2   while (!(base[0x08] & 0x1F00)) ; c = base[0x00] & 0x1FF
 *     ROM 0x00000228   base[0x08] & 0x1F00   receive fill level
 *     ROM 0x00000232   base[0x08] & 0x001F   transmit fill level
 *     ROM 0x000001F8   base[0x0C] |= bits    interrupt enables
 *     ROM 0x00000204   base[0x0C] &= ~bits
 *
 * and the bring-up around it, from bootloader 0x0082A220 and its identical
 * twin in FIRM at 0x00D9A4C0:
 *
 *     module_clock_enable(73)
 *     romclk_enable(22)                       0x400800B4 bit 0
 *     pad bank 1 pin 2, function 6
 *     base[0x10] |= 0xB0
 *     base[0x14] |= 3
 *     base[0x04] |= 7                         (ROM 0x1B6, the enable)
 *
 * Provenance: [V] the register offsets, the bit tests, both clock ids and the
 * pad, all read out of the ROM or of code both images share. [I] that it is a
 * UART at all, and the baud divisor below.
 *
 * =====================================================================
 *  WHAT IS NOT KNOWN
 * =====================================================================
 * **The framing, and the rate.** No string in either image names this block,
 * and nothing says 8N1. The vendor configures 1,500,000 against a 48 MHz
 * clock; the routine that turns that into register fields is thirty
 * instructions of soft-float writing bits 12 and 16 of CTRL, and this driver
 * does not attempt it - it leaves the rate as the ROM left it. So the first
 * thing to check on a scope is the bit period.
 *
 * **Whether anything is connected.** Bank 1 pin 2 is a pad; whether this
 * board brings it anywhere you can reach is unknown, and the pad census
 * (§"the pad census") cannot say, because a pad in function 6 is unreadable.
 * Finding the test point is a meter-and-magnifier job.
 *
 * So the honest summary is: the block is real, both vendor images drive it,
 * the write path is four instructions, and nobody has seen a character.
 */
#ifndef SL6806_UART_H
#define SL6806_UART_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* [V] bootloader 0x0082A220, FIRM 0x00D9A4C0. */
#define SL6806_UART_BASE        0x40091000u

/* [V] the offsets ROM 0x1D0 / 0x1E2 / 0x228 / 0x1F8 use. */
#define SL6806_UART_DATA        0x00u   /* 9 bits, read and write */
#define SL6806_UART_CTRL        0x04u   /* divisor and the enable bits */
#define SL6806_UART_STATUS      0x08u
#define SL6806_UART_IRQ_ENABLE  0x0Cu
#define SL6806_UART_REG10       0x10u   /* |= 0xB0 at bring-up, [?] */
#define SL6806_UART_REG14       0x14u   /* |= 3 at bring-up, [?] */
#define SL6806_UART_MODE        0x20u   /* [I] parity and stop bits */

/*
 * [V] STATUS bits, from the two spin loops - and the transmit one is a
 * five-bit FIELD, not a single bit.
 *
 * ROM 0x1D0 is `lsls r2, r2, #27` then `beq` back. That shifts bits [4:0]
 * into the top of the word and branches while the result is zero, so the
 * condition is `(status & 0x1F) != 0` - the whole transmit level, which
 * ROM 0x232 also reads as a field. Reading `lsls #27` as "test bit 4" is
 * wrong by four bits and was in this file for one hardware run: the port
 * reported status 0x00000001, the ROM's condition would have passed, and
 * every putc timed out waiting for a bit that never sets.
 */
#define SL6806_UART_TX_READY    0x0000001Fu   /* ROM 0x1D0: any of [4:0] */
#define SL6806_UART_RX_LEVEL    0x00001F00u   /* ROM 0x1E2 / 0x228 */
#define SL6806_UART_TX_LEVEL    0x0000001Fu   /* ROM 0x232, the same field */

/* [V] the two clock ids, and the pad. */
#define SL6806_UART_MODULE_ID   73u
#define SL6806_UART_ROMCLK_ID   22u
#define SL6806_UART_ROMCLK_REG  0x400800B4u
#define SL6806_UART_ROMCLK_BIT  0x00000001u
#define SL6806_UART_PAD         0x00011300u   /* bank 1 pin 2, function 6 */

/* [V] the vendor's own numbers, bootloader hardware_init. */
#define SL6806_UART_CLOCK       48000000u
#define SL6806_UART_BAUD        1500000u

/*
 * Bring the port up: both clocks, the pad, the two configuration registers
 * and the enable.
 *
 * **`baud` is accepted and ignored.** The rate lives in fields of CTRL at
 * bits 12 and 16 that are computed in soft-float and are not decoded, so this
 * leaves whatever the ROM's own init left - the vendor's 1,500,000 against
 * 48 MHz on a device that has booted normally. The argument is here so the
 * signature does not change when the divisor is worked out. See the .c file
 * for the misreading that made this explicit.
 *
 * Returns 1 if module 73 acknowledged its gate, 0 if it did not - in which
 * case nothing was written and the port is not there.
 */
int sl6806_uart_begin(uint32_t baud);

/*
 * Use the port without configuring it, for the case where the boot ROM has
 * already brought it up. Writes nothing but the data register.
 *
 * There is a reason to prefer this where it works: this driver's bring-up is
 * transcribed from two images that both run before a payload does, and the
 * one thing it cannot check is whether re-running it on a live port breaks
 * something. sl6806_uart_alive() is how to decide.
 */
void sl6806_uart_attach(void);

/*
 * Does the block look like a configured, running port? True when the status
 * register reports transmit-ready and the enable bits in CTRL are set - which
 * a gated block, reading zero, cannot fake.
 */
int sl6806_uart_alive(void);

/*
 * Send one byte. Waits for transmit-ready, bounded: the ROM's loop is
 * unbounded and would hang a payload forever on a port that is not there.
 * Returns 1 if the byte went out, 0 on timeout.
 */
int sl6806_uart_putc(uint8_t c);

/* Send a NUL-terminated string, expanding '\n' to CR LF exactly as ROM
 * 0x23C does. Returns the number of bytes accepted. */
int sl6806_uart_puts(const char *s);

/* Send `len` bytes with no expansion. Returns the number accepted. */
int sl6806_uart_write(const void *buf, unsigned len);

/* A byte if one has arrived, or -1. Never blocks. */
int sl6806_uart_getc(void);

/*
 * Send everything the console prints to this port as well.
 *
 * Costs nothing when the port is not there: the sink checks
 * sl6806_uart_alive() once when it is installed, and a putc that times out
 * gives up in a bounded loop rather than blocking. Call it after
 * sl6806_uart_begin(), and after Serial.begin() if you want the banner.
 *
 * Passing 0 removes the sink.
 */
void sl6806_uart_console_mirror(int on);

/* The raw status word, for a probe. */
uint32_t sl6806_uart_status(void);

#ifdef __cplusplus
}
#endif

#endif /* SL6806_UART_H */
