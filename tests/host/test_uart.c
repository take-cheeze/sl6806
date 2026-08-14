/*
 * test_uart.c - the serial port driver against a model of the block.
 *
 * Nobody has seen a character out of this port, so what these check is that
 * the driver performs the sequence the mask ROM performs. Five things, each
 * of which a plausible tidy-up would break in a way no host test would
 * otherwise catch:
 *
 *   1. **Wait, then write.** ROM 0x1D0 spins on status bit 4 and only then
 *      stores the byte. Storing first and checking afterwards would drop
 *      characters on a port whose FIFO is full, which is every port under
 *      load, and would look fine on a model that is always ready.
 *
 *   2. **The wait is bounded.** The ROM's is not. On a port that is not
 *      configured its loop never returns, inside the boot ROM's USB handler,
 *      taking the device off the bus with nothing printed - and this driver
 *      exists precisely to report things when that is happening.
 *
 *   3. **Nine bits, not eight.** ROM 0x1D0 masks with 0x1FF. The register is
 *      wider than a byte and the mask is the ROM's, not an accident.
 *
 *   4. **'\n' goes out as CR LF**, expanded in software, because ROM 0x23C
 *      does it and a terminal on the other end will want it.
 *
 *   5. **The gates come before the registers**, and a module that will not
 *      acknowledge means nothing is written at all - the same discipline
 *      sl6806_module.h spends a page on, and the reason the ADC was written
 *      off for four runs.
 */

#include <stdio.h>
#include <string.h>

#include "sl6806_uart.h"
#include "sl6806_module.h"

void delay(uint32_t ms) { (void)ms; }
void delayMicroseconds(uint32_t us) { (void)us; }

static int failures, checks;

#define CHECK(cond, what) do {                                          \
        checks++;                                                       \
        if (!(cond)) {                                                  \
            printf("  FAIL %s (%s:%d)\n", (what), __FILE__, __LINE__);  \
            failures++;                                                 \
        }                                                               \
    } while (0)

/* ------------------------------------------------------------------ */
/* A model of the port and the gates in front of it                    */
/* ------------------------------------------------------------------ */

#define CRU 0x40080000u

static uint32_t mod_shadow, mod_gate;      /* ids 64..95: +0x78 and +0x68 */
static uint32_t romclk;
static uint32_t reg[16];                   /* the port, by offset/4       */
static uint32_t pad_writes;

static int      gated;                     /* does the block answer?      */
static int      gate_refuses;              /* module 73 will not latch    */
static int      tx_ready;                  /* status bit 4                */
static uint8_t  sent[64];
static unsigned n_sent;
static int      wrote_before_wait;         /* the ordering violation      */
static int      waiting;

uint32_t sl6806_mmio_read(uint32_t addr)
{
    if (addr >= SL6806_UART_BASE && addr < SL6806_UART_BASE + 0x40u) {
        unsigned i = (addr - SL6806_UART_BASE) / 4u;

        if (!gated)
            return 0;
        if (i == SL6806_UART_STATUS / 4u) {
            waiting = 1;
            return tx_ready ? SL6806_UART_TX_READY : 0u;
        }
        return reg[i];
    }
    if (addr == CRU + 0x68u) return mod_gate;
    if (addr == CRU + 0x78u) return mod_shadow;
    if (addr == SL6806_UART_ROMCLK_REG) return romclk;
    return 0;
}

void sl6806_mmio_write(uint32_t addr, uint32_t value)
{
    if (addr >= SL6806_UART_BASE && addr < SL6806_UART_BASE + 0x40u) {
        unsigned i = (addr - SL6806_UART_BASE) / 4u;

        if (!gated)
            return;                        /* a gated block drops writes */
        if (i == SL6806_UART_DATA / 4u) {
            if (!waiting)
                wrote_before_wait = 1;
            if (n_sent < sizeof(sent))
                sent[n_sent++] = (uint8_t)value;
            /* The value must already be masked to nine bits by the driver. */
            if (value > 0x1FFu)
                wrote_before_wait = 1;
            return;
        }
        reg[i] = value;
        return;
    }
    if (addr == CRU + 0x78u) { mod_shadow = value; return; }
    if (addr == CRU + 0x68u) {
        mod_gate = gate_refuses ? 0u : (value & mod_shadow);
        return;
    }
    if (addr == SL6806_UART_ROMCLK_REG) { romclk = value; return; }
    if (addr >= 0x40081000u && addr < 0x400F7000u) { pad_writes++; return; }
}

static void model_reset(int is_gated)
{
    mod_shadow = mod_gate = romclk = 0;
    memset(reg, 0, sizeof(reg));
    pad_writes = 0;
    gated = is_gated;
    gate_refuses = !is_gated;
    tx_ready = 1;
    n_sent = 0;
    wrote_before_wait = 0;
    waiting = 0;
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

static void test_bringup(void)
{
    printf("bring-up\n");
    model_reset(1);

    CHECK(sl6806_uart_begin(0) == 1, "begin succeeds when the gate answers");
    CHECK(mod_gate & (1u << (73u - 64u)), "module 73 is gated on");
    CHECK(romclk & SL6806_UART_ROMCLK_BIT, "ROM clock 22 is on");
    CHECK(pad_writes > 0, "the pad was configured");

    CHECK((reg[SL6806_UART_REG10 / 4u] & 0xB0u) == 0xB0u,
          "+0x10 got the vendor's 0xB0");
    CHECK((reg[SL6806_UART_REG14 / 4u] & 3u) == 3u, "+0x14 got its 3");
    CHECK((reg[SL6806_UART_CTRL / 4u] & 7u) == 7u, "the enable bits are set");

    /* The rate is deliberately not written: CTRL's low bits are the enable,
     * not a divisor, and the fields that do carry it are undecoded. A driver
     * that put a divisor in the low twelve bits would disable the port, which
     * is exactly what the first version of this did. */
    CHECK((reg[SL6806_UART_CTRL / 4u] & ~7u) == 0u,
          "no divisor was invented in CTRL's other bits");

    model_reset(1);
    CHECK(sl6806_uart_begin(115200) == 1, "a baud argument is accepted");
    CHECK((reg[SL6806_UART_CTRL / 4u] & 7u) == 7u,
          "and ignored - the enable is all that is written");
}

static void test_gate_refused(void)
{
    printf("a module gate that will not answer\n");
    model_reset(0);                        /* block drops everything */

    CHECK(sl6806_uart_begin(0) == 0, "begin reports failure");
    CHECK(reg[SL6806_UART_CTRL / 4u] == 0u,
          "and nothing reached the port's registers");
}

static void test_write_order(void)
{
    printf("wait, then write\n");
    model_reset(1);
    sl6806_uart_begin(0);

    waiting = 0;
    CHECK(sl6806_uart_putc('A') == 1, "a byte goes out");
    CHECK(!wrote_before_wait, "the status was read before the data was stored");
    CHECK(n_sent == 1 && sent[0] == 'A', "and it is the byte we asked for");

    /* Nine bits: 0xFF must survive, and nothing may exceed the mask. */
    n_sent = 0;
    CHECK(sl6806_uart_putc(0xFF) == 1, "a high byte goes out");
    CHECK(sent[0] == 0xFF, "unmangled");
    CHECK(!wrote_before_wait, "and still within the nine-bit mask");
}

static void test_bounded(void)
{
    printf("a port that is never ready\n");
    model_reset(1);
    sl6806_uart_begin(0);
    tx_ready = 0;

    /* The whole point: this returns. */
    CHECK(sl6806_uart_putc('X') == 0, "putc gives up instead of hanging");
    CHECK(n_sent == 0, "and writes nothing");
    CHECK(sl6806_uart_puts("hello") == 0, "puts stops at the first refusal");
}

static void test_newline(void)
{
    printf("newline expansion\n");
    model_reset(1);
    sl6806_uart_begin(0);
    n_sent = 0;

    CHECK(sl6806_uart_puts("a\nb") == 3, "three characters accepted");
    CHECK(n_sent == 4, "four bytes on the wire");
    CHECK(sent[0] == 'a' && sent[1] == '\r' && sent[2] == '\n'
          && sent[3] == 'b', "the newline became CR LF");
}

static void test_alive_and_rx(void)
{
    printf("liveness and receive\n");

    model_reset(0);
    CHECK(!sl6806_uart_alive(), "a gated block is not alive");

    model_reset(1);
    CHECK(!sl6806_uart_alive(), "nor is one that has not been configured");

    sl6806_uart_begin(0);
    CHECK(sl6806_uart_alive(), "a configured port is");

    /* Receive: the level field is empty, so getc must not block or invent. */
    CHECK(sl6806_uart_getc() == -1, "getc returns -1 when nothing arrived");
}

int main(void)
{
    printf("test_uart\n");

    test_bringup();
    test_gate_refused();
    test_write_order();
    test_bounded();
    test_newline();
    test_alive_and_rx();

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
