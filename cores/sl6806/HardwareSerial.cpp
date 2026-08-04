#include "HardwareSerial.h"
#include "sl6806_console.h"

#if SL6806_BUILD_PAYLOAD
#include "sl6806_rom.h"
#endif

HardwareSerial Serial;

void HardwareSerial::begin(unsigned long baud)
{
    (void)baud;   /* no wire, no speed - see the header */
    sl6806_console_init();
}

void HardwareSerial::end()
{
    flush();
}

int HardwareSerial::available()
{
    return sl6806_console_available();
}

int HardwareSerial::read()
{
    return sl6806_console_read();
}

int HardwareSerial::peek()
{
    return sl6806_console_peek();
}

void HardwareSerial::flush()
{
    /* Writes land in the ring immediately; there is no transmit queue to
     * drain. The host polls on its own schedule. */
}

int HardwareSerial::availableForWrite()
{
    /* The ring always accepts data - it overwrites the oldest bytes rather
     * than blocking, so the sketch is never stalled by a slow host. */
    return SL6806_CONSOLE_SIZE;
}

size_t HardwareSerial::write(uint8_t c)
{
    return write(&c, 1);
}

size_t HardwareSerial::write(const uint8_t *buf, size_t size)
{
    sl6806_console_write(buf, size);

#if SL6806_BUILD_PAYLOAD
    if (rom_echo_) {
        /* rom_printf is varargs; feed it a literal %s with a bounded copy so
         * a caller's '%' in the data cannot be interpreted as a format. */
        char tmp[65];
        size_t off = 0;
        while (off < size) {
            size_t n = size - off;
            if (n > sizeof(tmp) - 1)
                n = sizeof(tmp) - 1;
            for (size_t i = 0; i < n; i++)
                tmp[i] = static_cast<char>(buf[off + i]);
            tmp[n] = '\0';
            rom_printf("%s", tmp);
            off += n;
        }
    }
#endif

    return size;
}
