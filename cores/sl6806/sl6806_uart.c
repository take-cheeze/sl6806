/*
 * sl6806_uart.c - see sl6806_uart.h. The serial port at 0x40091000.
 *
 * Four instructions do the work; everything else is bring-up and bounds.
 */

#include "sl6806_uart.h"

#include "sl6806_mmio.h"
#include "sl6806_module.h"
#include "sl6806_padctl.h"

#define U(off) (SL6806_UART_BASE + (off))

/*
 * How long to wait for transmit-ready. The ROM spins forever
 * (`while (!(base[8] & 0x1F));` at 0x1D0), and on a port that is not
 * configured that never returns - inside the boot ROM's USB handler, which
 * takes the device off the bus with nothing printed. A driver whose whole
 * purpose is to report things must not be able to hang.
 *
 * At 1.5 Mbaud a character is under 7 us, so this is thousands of character
 * times and still finite.
 */
#define TX_TIMEOUT 20000u

static int uart_wait_tx(void)
{
    unsigned n;

    for (n = 0; n < TX_TIMEOUT; n++)
        if (sl6806_mmio_read(U(SL6806_UART_STATUS)) & SL6806_UART_TX_READY)
            return 1;
    return 0;
}

int sl6806_uart_begin(uint32_t baud)
{
    /* [V] the pad first, so the line is driven before the port is enabled
     * rather than after - which is the order both vendor images use. */
    sl6806_pad_configure(SL6806_UART_PAD);

    if (!sl6806_module_enable(SL6806_UART_MODULE_ID))
        return 0;

    sl6806_mmio_set(SL6806_UART_ROMCLK_REG, SL6806_UART_ROMCLK_BIT);

    /* [V] bootloader 0x0082A220. Meanings unknown; both images write them. */
    sl6806_mmio_set(U(SL6806_UART_REG10), 0xB0u);
    sl6806_mmio_set(U(SL6806_UART_REG14), 3u);

    /*
     * THE BAUD RATE IS NOT SET HERE, and that is deliberate.
     *
     * The first version of this driver put a divisor in CTRL's low twelve
     * bits, on a misreading of ROM 0x13A. That instruction is
     * `ubfx r3, r3, #0, #12` followed by a store, which **keeps** the low
     * twelve bits and discards everything above them; the new fields then go
     * in at bit 12 and bit 16 (ROM 0x146 and 0x14E). So the low bits are the
     * enable and mode - the very bits `CTRL |= 7` sets below - and a divisor
     * written there would clobber them. The host test caught it because the
     * two writes collided; on hardware it would have produced a port that
     * looked configured and was disabled.
     *
     * What the rate actually is comes out of thirty instructions of
     * soft-float around `clock / (16 * baud)`, distributed across two
     * registers in fields whose widths are not established. Rather than write
     * a guess into a register nobody has decoded, this leaves the rate alone:
     * the port runs at whatever the ROM's own init left, which on a device
     * that has booted normally is the vendor's 1,500,000 against 48 MHz.
     *
     * If a scope shows the wrong rate, ROM 0x130..0x1A0 is the routine to
     * decode, and `baud` is the argument this function would then use.
     */
    (void)baud;

    /* [V] ROM 0x1B6, the last thing its init does. */
    sl6806_mmio_set(U(SL6806_UART_CTRL), 7u);

    return 1;
}

void sl6806_uart_attach(void)
{
    /* Nothing to do: every call below reads or writes the block directly.
     * This exists so a caller can say which it meant. */
}

int sl6806_uart_alive(void)
{
    uint32_t ctrl = sl6806_mmio_read(U(SL6806_UART_CTRL));
    uint32_t sta  = sl6806_mmio_read(U(SL6806_UART_STATUS));

    /* A gated block reads zero, so requiring a set bit in each of two
     * registers is a test it cannot pass by being absent. */
    return ((ctrl & 7u) == 7u) && ((sta & SL6806_UART_TX_READY) != 0u);
}

int sl6806_uart_putc(uint8_t c)
{
    if (!uart_wait_tx())
        return 0;

    /* [V] ROM 0x1D0 masks to nine bits before storing. Nine, not eight - the
     * data register is wider than a byte, which is what a port that can do
     * a ninth address bit looks like. */
    sl6806_mmio_write(U(SL6806_UART_DATA), (uint32_t)c & 0x1FFu);
    return 1;
}

int sl6806_uart_puts(const char *s)
{
    int n = 0;

    if (!s)
        return 0;

    while (*s) {
        /* [V] ROM 0x23C: a newline goes out as CR then LF. */
        if (*s == '\n' && !sl6806_uart_putc('\r'))
            return n;
        if (!sl6806_uart_putc((uint8_t)*s))
            return n;
        n++;
        s++;
    }
    return n;
}

int sl6806_uart_write(const void *buf, unsigned len)
{
    const uint8_t *p = (const uint8_t *)buf;
    unsigned i;

    if (!buf)
        return 0;

    for (i = 0; i < len; i++)
        if (!sl6806_uart_putc(p[i]))
            break;
    return (int)i;
}

int sl6806_uart_getc(void)
{
    /* [V] ROM 0x1E2 waits on the receive level field and then reads the same
     * data register. This does not wait: a probe that blocks on input nobody
     * is sending is the same hang in a different direction. */
    if (!(sl6806_mmio_read(U(SL6806_UART_STATUS)) & SL6806_UART_RX_LEVEL))
        return -1;
    return (int)(sl6806_mmio_read(U(SL6806_UART_DATA)) & 0x1FFu);
}

uint32_t sl6806_uart_status(void)
{
    return sl6806_mmio_read(U(SL6806_UART_STATUS));
}
