#include "sl6806.h"
#include "sl6806_console.h"

#if SL6806_CONSOLE_SIZE & (SL6806_CONSOLE_SIZE - 1)
#error "SL6806_CONSOLE_SIZE must be a power of two"
#endif

/*
 * Deliberately not static: the monitor tool locates this by symbol name in
 * the ELF, so it must survive into the symbol table. Aligned so the host can
 * read the header with a single aligned read_mem.
 */
__attribute__((used, aligned(4)))
sl6806_console_t _sl6806_console;

void sl6806_console_init(void)
{
    if (_sl6806_console.magic == SL6806_CONSOLE_MAGIC)
        return;   /* already live - keep whatever the host has not read yet */

    _sl6806_console.size    = SL6806_CONSOLE_SIZE;
    _sl6806_console.head    = 0;
    _sl6806_console.rx_head = 0;
    _sl6806_console.rx_tail = 0;
    _sl6806_console.version = 1;
    /* Magic last: the host must never see a half-initialised header. */
    sl6806_dsb();
    _sl6806_console.magic   = SL6806_CONSOLE_MAGIC;
}

void sl6806_console_write(const uint8_t *buf, size_t len)
{
    uint32_t pm, head;
    size_t i;

    if (_sl6806_console.magic != SL6806_CONSOLE_MAGIC)
        sl6806_console_init();

    pm = sl6806_irq_save();
    head = _sl6806_console.head;
    for (i = 0; i < len; i++)
        _sl6806_console.tx[(head + i) & (SL6806_CONSOLE_SIZE - 1)] = buf[i];
    /* Publish the data before the index that advertises it. */
    sl6806_dsb();
    _sl6806_console.head = head + (uint32_t)len;
    sl6806_irq_restore(pm);
}

void sl6806_debug_print(const char *s)
{
    const char *p = s;
    size_t n = 0;

    while (*p++)
        n++;
    sl6806_console_write((const uint8_t *)s, n);
}

int sl6806_console_available(void)
{
    return (int)(_sl6806_console.rx_head - _sl6806_console.rx_tail);
}

int sl6806_console_peek(void)
{
    if (!sl6806_console_available())
        return -1;
    return _sl6806_console.rx[_sl6806_console.rx_tail & (sizeof(_sl6806_console.rx) - 1)];
}

int sl6806_console_read(void)
{
    int c = sl6806_console_peek();

    if (c >= 0)
        _sl6806_console.rx_tail++;
    return c;
}
