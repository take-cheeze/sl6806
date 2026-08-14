#include "sl6806.h"
#include "sl6806_console.h"

#if SL6806_CONSOLE_SIZE & (SL6806_CONSOLE_SIZE - 1)
#error "SL6806_CONSOLE_SIZE must be a power of two"
#endif
#if SL6806_CONSOLE_RX_SIZE & (SL6806_CONSOLE_RX_SIZE - 1)
#error "SL6806_CONSOLE_RX_SIZE must be a power of two"
#endif

#define TX_MASK (SL6806_CONSOLE_SIZE - 1)
#define RX_MASK (SL6806_CONSOLE_RX_SIZE - 1)

/*
 * The host decodes these layouts with fixed struct formats (see
 * tools/sl6806-monitor), so any padding change here would silently desync the
 * two sides. Pin the contract down at compile time instead.
 */
#include <stddef.h>
_Static_assert(sizeof(sl6806_console_pkt_t) == 64,
               "poll packet must fill the ROM's 64-byte return buffer exactly");
_Static_assert(offsetof(sl6806_console_pkt_t, data) == 4,
               "host unpacks the packet as '<BBH60s'");
_Static_assert(offsetof(sl6806_console_t, rx_head) == 24,
               "monitor writes rx_head at header offset 24");
_Static_assert(offsetof(sl6806_console_t, tx) == 32,
               "host assumes a 32-byte header before the tx ring");

/*
 * Deliberately not static: the monitor locates this by symbol name in the
 * ELF, so it must survive into the symbol table. Aligned so the host can read
 * the header with a single aligned transfer.
 */
__attribute__((used, aligned(4)))
sl6806_console_t _sl6806_console;

void sl6806_console_init(void)
{
    /* Already live: keep whatever the host has not collected yet, so a
     * re-init inside a sketch does not swallow its own startup banner. */
    if (_sl6806_console.magic == SL6806_CONSOLE_MAGIC &&
        _sl6806_console.version == SL6806_CONSOLE_VERSION)
        return;

    _sl6806_console.size    = SL6806_CONSOLE_SIZE;
    _sl6806_console.version = SL6806_CONSOLE_VERSION;
    _sl6806_console.head    = 0;
    _sl6806_console.tail    = 0;
    _sl6806_console.dropped = 0;
    _sl6806_console.rx_head = 0;
    _sl6806_console.rx_tail = 0;

    /* Magic last: the host must never observe a half-built header. */
    sl6806_dsb();
    _sl6806_console.magic = SL6806_CONSOLE_MAGIC;
}

int sl6806_console_space(void)
{
    uint32_t used = _sl6806_console.head - _sl6806_console.tail;

    return (int)(SL6806_CONSOLE_SIZE - used);
}

static sl6806_console_sink_fn g_sink;

void sl6806_console_set_sink(sl6806_console_sink_fn fn)
{
    g_sink = fn;
}

void sl6806_console_write(const uint8_t *buf, size_t len)
{
    uint32_t pm, head, tail, used, skipped = 0;
    size_t i;

    if (_sl6806_console.magic != SL6806_CONSOLE_MAGIC)
        sl6806_console_init();

    /* Before the ring, and before anything can be dropped: a sink exists to
     * receive output the ring would lose, so giving it the truncated copy
     * would defeat the purpose. */
    if (g_sink)
        g_sink(buf, len);

    /* A single write larger than the ring can only ever leave its tail end,
     * so skip the part that is guaranteed to be overwritten. Those bytes are
     * still lost output and must be counted as dropped - otherwise one huge
     * write discards data and the host is never told. */
    if (len > SL6806_CONSOLE_SIZE) {
        skipped = (uint32_t)(len - SL6806_CONSOLE_SIZE);
        buf += skipped;
        len = SL6806_CONSOLE_SIZE;
    }

    pm = sl6806_irq_save();

    _sl6806_console.dropped += skipped;

    head = _sl6806_console.head;
    tail = _sl6806_console.tail;
    used = head - tail;

    /* Overwrite the oldest output rather than blocking. A sketch must never
     * stall because nobody is running the monitor. */
    if (used + len > SL6806_CONSOLE_SIZE) {
        uint32_t drop = used + (uint32_t)len - SL6806_CONSOLE_SIZE;
        _sl6806_console.tail = tail + drop;
        _sl6806_console.dropped += drop;
    }

    for (i = 0; i < len; i++)
        _sl6806_console.tx[(head + i) & TX_MASK] = buf[i];

    /* Publish the bytes before the index that advertises them. */
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

int sl6806_console_poll(sl6806_console_pkt_t *pkt)
{
    uint32_t pm, head, tail, avail;
    uint32_t n, i;

    pkt->len = 0;
    pkt->flags = 0;
    pkt->pending = 0;

    if (_sl6806_console.magic != SL6806_CONSOLE_MAGIC)
        return 0;

    pm = sl6806_irq_save();

    head = _sl6806_console.head;
    tail = _sl6806_console.tail;
    avail = head - tail;

    n = avail > SL6806_CONSOLE_POLL_MAX ? SL6806_CONSOLE_POLL_MAX : avail;
    for (i = 0; i < n; i++)
        pkt->data[i] = _sl6806_console.tx[(tail + i) & TX_MASK];

    _sl6806_console.tail = tail + n;

    pkt->len = (uint8_t)n;
    pkt->pending = (uint16_t)((avail - n) > 0xFFFFu ? 0xFFFFu : (avail - n));

    /* Report loss once, on the next poll after it happened, so the host can
     * show exactly where the gap is in the stream. */
    if (_sl6806_console.dropped) {
        pkt->flags |= SL6806_CONSOLE_F_OVERFLOW;
        _sl6806_console.dropped = 0;
    }

    sl6806_irq_restore(pm);
    return (int)n;
}

int sl6806_console_available(void)
{
    return (int)(_sl6806_console.rx_head - _sl6806_console.rx_tail);
}

int sl6806_console_peek(void)
{
    if (!sl6806_console_available())
        return -1;
    return _sl6806_console.rx[_sl6806_console.rx_tail & RX_MASK];
}

int sl6806_console_read(void)
{
    int c = sl6806_console_peek();

    if (c >= 0)
        _sl6806_console.rx_tail++;
    return c;
}
