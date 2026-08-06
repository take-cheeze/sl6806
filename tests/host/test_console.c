/*
 * test_console.c - native tests for the USB serial ring logic.
 *
 * The console protocol is the one piece of device code whose behaviour can be
 * checked without hardware: it is pure arithmetic over two ring buffers. The
 * cases that matter are the ones that are hard to spot on a device - wrapping
 * exactly at the buffer boundary, and losing output when the host polls too
 * slowly.
 *
 *   make -C tests/host
 */

#include <stdio.h>
#include <string.h>

#include "sl6806_console.h"

/*
 * wiring_time.c is Cortex-M only and is not linked here, but the poll handler
 * calls into it to stamp the clock-calibration counter. Counting the calls is
 * more useful than an empty stub: it lets a test assert that every poll takes
 * a stamp, which is what tools/sl6806-clockcal depends on.
 */
static unsigned stamps;
void sl6806_time_stamp(void);
void sl6806_time_stamp(void) { stamps++; }

static int failures;
static int checks;

#define CHECK(cond, ...) do {                                   \
        checks++;                                               \
        if (!(cond)) {                                          \
            failures++;                                         \
            printf("  FAIL %s:%d: ", __func__, __LINE__);       \
            printf(__VA_ARGS__);                                \
            printf("\n");                                       \
        }                                                       \
    } while (0)

static void reset(void)
{
    memset(&_sl6806_console, 0, sizeof(_sl6806_console));
    sl6806_console_init();
}

static void put(const char *s)
{
    sl6806_console_write((const uint8_t *)s, strlen(s));
}

/* Drain the whole queue through polls, concatenating into `out`. */
static int drain(char *out, int cap, int *overflowed)
{
    sl6806_console_pkt_t pkt;
    int total = 0;

    if (overflowed)
        *overflowed = 0;

    for (;;) {
        int n = sl6806_console_poll(&pkt);
        if (overflowed && (pkt.flags & SL6806_CONSOLE_F_OVERFLOW))
            *overflowed = 1;
        if (n == 0)
            break;
        if (total + n < cap) {
            memcpy(out + total, pkt.data, n);
            total += n;
        }
    }
    out[total] = '\0';
    return total;
}

static void test_every_poll_stamps_the_clock(void)
{
    sl6806_console_pkt_t pkt;

    /* The host measures the real CPU clock by timing poll transactions and
     * reading the stamp each one leaves behind. A poll that does not stamp
     * would not fail loudly - it would just make the regression quietly wrong,
     * so pin it down here. */
    reset();
    stamps = 0;
    sl6806_console_poll(&pkt);
    CHECK(stamps == 1, "an empty poll must still stamp, got %u", stamps);

    put("x");
    sl6806_console_poll(&pkt);
    CHECK(stamps == 2, "a poll with data must stamp too, got %u", stamps);

    /* Even with the ring uninitialised - that path returns early. */
    memset(&_sl6806_console, 0, sizeof(_sl6806_console));
    stamps = 0;
    sl6806_console_poll(&pkt);
    CHECK(stamps == 1, "the early-return path skipped the stamp");
}

static void test_roundtrip(void)
{
    char buf[4096];

    reset();
    put("hello world");

    int n = drain(buf, sizeof(buf), NULL);
    CHECK(n == 11, "expected 11 bytes, got %d", n);
    CHECK(strcmp(buf, "hello world") == 0, "got \"%s\"", buf);
}

static void test_empty_poll(void)
{
    sl6806_console_pkt_t pkt;

    reset();
    int n = sl6806_console_poll(&pkt);
    CHECK(n == 0, "empty queue should yield 0, got %d", n);
    CHECK(pkt.len == 0, "len should be 0");
    CHECK(pkt.pending == 0, "pending should be 0");
    CHECK((pkt.flags & SL6806_CONSOLE_F_OVERFLOW) == 0, "no overflow expected");
}

static void test_packet_is_capped(void)
{
    sl6806_console_pkt_t pkt;
    char big[200];

    reset();
    memset(big, 'x', sizeof(big));
    sl6806_console_write((const uint8_t *)big, sizeof(big));

    int n = sl6806_console_poll(&pkt);
    CHECK(n == SL6806_CONSOLE_POLL_MAX, "first packet should be full (%d), got %d",
          SL6806_CONSOLE_POLL_MAX, n);
    CHECK(pkt.pending == sizeof(big) - SL6806_CONSOLE_POLL_MAX,
          "pending should be %zu, got %u",
          sizeof(big) - SL6806_CONSOLE_POLL_MAX, pkt.pending);
}

static void test_multi_packet_drain(void)
{
    char buf[4096];
    char expect[1000];

    reset();
    for (int i = 0; i < (int)sizeof(expect) - 1; i++)
        expect[i] = (char)('a' + (i % 26));
    expect[sizeof(expect) - 1] = '\0';
    put(expect);

    int over = 0;
    int n = drain(buf, sizeof(buf), &over);
    CHECK(n == (int)strlen(expect), "expected %zu bytes, got %d",
          strlen(expect), n);
    CHECK(memcmp(buf, expect, strlen(expect)) == 0,
          "content mismatch across packet boundaries");
    CHECK(!over, "should not overflow below the ring size");
}

/* The interesting case: writes that straddle the end of the ring. */
static void test_wraparound(void)
{
    char buf[4096];
    char chunk[100];

    reset();
    memset(chunk, 'A', sizeof(chunk));

    /* Push the head to just before the wrap point, draining as we go so
     * nothing is ever lost. */
    int pushed = 0;
    while (pushed + (int)sizeof(chunk) < SL6806_CONSOLE_SIZE - 10) {
        sl6806_console_write((const uint8_t *)chunk, sizeof(chunk));
        drain(buf, sizeof(buf), NULL);
        pushed += sizeof(chunk);
    }

    /* Now write a run that must wrap. */
    const char *msg = "WRAPPED-PAYLOAD-0123456789-END";
    put(msg);

    int over = 0;
    int n = drain(buf, sizeof(buf), &over);
    CHECK(n == (int)strlen(msg), "expected %zu across the wrap, got %d",
          strlen(msg), n);
    CHECK(strcmp(buf, msg) == 0, "wrapped content corrupted: \"%s\"", buf);
    CHECK(!over, "wrap alone must not report overflow");
}

static void test_overflow_reported(void)
{
    char buf[8192];
    char big[SL6806_CONSOLE_SIZE + 500];

    reset();
    memset(big, 'z', sizeof(big));

    /* Write more than the ring holds without draining: the oldest bytes must
     * be dropped, and the loss must be reported exactly once. */
    sl6806_console_write((const uint8_t *)big, sizeof(big));

    sl6806_console_pkt_t pkt;
    sl6806_console_poll(&pkt);
    CHECK(pkt.flags & SL6806_CONSOLE_F_OVERFLOW,
          "overflow flag should be set after overrunning the ring");

    /* The flag is one-shot - it describes loss since the previous poll. */
    sl6806_console_poll(&pkt);
    CHECK(!(pkt.flags & SL6806_CONSOLE_F_OVERFLOW),
          "overflow flag should clear after being reported");

    drain(buf, sizeof(buf), NULL);

    /* Only the most recent SL6806_CONSOLE_SIZE bytes survive. */
    reset();
    char tagged[SL6806_CONSOLE_SIZE + 16];
    memset(tagged, '.', sizeof(tagged));
    memcpy(tagged + sizeof(tagged) - 5, "TAIL!", 5);
    sl6806_console_write((const uint8_t *)tagged, sizeof(tagged));

    int n = drain(buf, sizeof(buf), NULL);
    CHECK(n == SL6806_CONSOLE_SIZE, "should retain exactly one ring (%d), got %d",
          SL6806_CONSOLE_SIZE, n);
    CHECK(memcmp(buf + n - 5, "TAIL!", 5) == 0,
          "the newest bytes must be the ones kept");
}

static void test_oversized_single_write(void)
{
    char buf[8192];
    char huge[SL6806_CONSOLE_SIZE * 3];

    reset();
    memset(huge, 'q', sizeof(huge));
    memcpy(huge + sizeof(huge) - 4, "LAST", 4);

    /* A single write larger than the ring must not walk off the buffer. */
    sl6806_console_write((const uint8_t *)huge, sizeof(huge));

    int n = drain(buf, sizeof(buf), NULL);
    CHECK(n == SL6806_CONSOLE_SIZE, "expected one ring of data, got %d", n);
    CHECK(memcmp(buf + n - 4, "LAST", 4) == 0,
          "tail of an oversized write must survive");
}

static void test_space_accounting(void)
{
    char buf[4096];

    reset();
    CHECK(sl6806_console_space() == SL6806_CONSOLE_SIZE,
          "empty ring should report full space, got %d",
          sl6806_console_space());

    put("1234567890");
    CHECK(sl6806_console_space() == SL6806_CONSOLE_SIZE - 10,
          "space should drop by 10, got %d", sl6806_console_space());

    drain(buf, sizeof(buf), NULL);
    CHECK(sl6806_console_space() == SL6806_CONSOLE_SIZE,
          "space should recover after draining, got %d",
          sl6806_console_space());
}

static void test_rx(void)
{
    reset();
    CHECK(sl6806_console_available() == 0, "rx should start empty");
    CHECK(sl6806_console_read() == -1, "read on empty rx should be -1");

    /* Emulate what the monitor does: place bytes, then publish rx_head. */
    const char *in = "ping\n";
    for (unsigned i = 0; i < strlen(in); i++)
        _sl6806_console.rx[i] = (uint8_t)in[i];
    _sl6806_console.rx_head = strlen(in);

    CHECK(sl6806_console_available() == (int)strlen(in),
          "available should be %zu, got %d", strlen(in),
          sl6806_console_available());
    CHECK(sl6806_console_peek() == 'p', "peek should not consume");
    CHECK(sl6806_console_available() == (int)strlen(in),
          "peek must not change available");

    char got[16];
    int n = 0;
    int c;
    while ((c = sl6806_console_read()) >= 0)
        got[n++] = (char)c;
    got[n] = '\0';

    CHECK(strcmp(got, in) == 0, "rx content mismatch: \"%s\"", got);
    CHECK(sl6806_console_available() == 0, "rx should be drained");
}

int main(void)
{
    printf("SL6806 console tests\n");

    test_roundtrip();
    test_empty_poll();
    test_packet_is_capped();
    test_multi_packet_drain();
    test_wraparound();
    test_overflow_reported();
    test_oversized_single_write();
    test_space_accounting();
    test_rx();
    test_every_poll_stamps_the_clock();

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
