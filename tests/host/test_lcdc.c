/*
 * test_lcdc.c - native tests for the LCD controller driver.
 *
 * The driver in cores/sl6806/sl6806_lcdc.c was written by reading the stock
 * bootloader's disassembly. Nobody has put a logic analyser on the pins. So
 * the question this file answers is not "does the panel light up" - it
 * cannot - but the one that is checkable, and is where the bugs would be:
 *
 *   Given the register sequence the vendor uses, does this driver put the
 *   right bytes on the QSPI bus, in the right transactions?
 *
 * It does that by putting a model of the controller behind sl6806_mmio_*.
 * The model watches +0x20 for the start bit, decodes the transfer the way
 * the hardware would have to, and records the bytes that would have gone out
 * and where chip select was released. The real panel tables from
 * variants/p20_player/panel.c are then run through it.
 *
 * Four things this pins down that reading cannot:
 *
 *   1. Every command comes out as a four-byte QSPI frame - opcode, 0,
 *      command, 0 - in one transaction with its parameters. That is the
 *      whole basis of the driver, and getting the transaction boundary wrong
 *      means the panel sees a stream of commands with no arguments.
 *   2. A pixel push is one transaction with the four-lane opcode, however
 *      many pixels() calls and however many 16-byte transfers it took -
 *      including the case where the source rows are not contiguous.
 *   3. Bring-up writes the constants the vendor writes, in the registers the
 *      vendor writes them in.
 *   4. A controller that never reports completion makes the driver give up
 *      and say so rather than spin. That behaviour exists because spinning
 *      inside the boot ROM's USB handler takes the device off the bus, and a
 *      test is the only place it can be provoked on purpose.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "sl6806_lcdc.h"
#include "sl6806_padctl.h"
#include "gfx/Panel.h"

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

/* ------------------------------------------------------ the peripheral model */

/*
 * Three flat windows rather than one array per GPIO bank, because the bank
 * bases are only 0x40 apart while the register map runs to +0x220 - the
 * banks are interleaved into one register file, and modelling them
 * separately would make overlapping addresses alias.
 */
#define LCDC_BASE  SL6806_LCDC_BASE
#define LCDC_SPAN  0x100
#define CRU_BASE   0x40080000u
#define CRU_SPAN   0x200
#define PADA_BASE  0x40081000u          /* banks 0, 1, 3, 4 */
#define PADA_SPAN  0x400
#define PADB_BASE  0x400F6000u          /* banks 2 and 5    */
#define PADB_SPAN  0x400

static uint32_t lcdc_reg[LCDC_SPAN / 4];
static uint32_t cru_reg[CRU_SPAN / 4];
static uint32_t pada_reg[PADA_SPAN / 4];
static uint32_t padb_reg[PADB_SPAN / 4];

/* Bytes the model believes were clocked out, split into transactions. A
 * transaction ends with the transfer that ran with chip select released,
 * which is how this controller marks the end of a frame. */
#define TXN_MAX   64
#define TXN_BYTES 1024

static struct {
    uint8_t  data[TXN_BYTES];
    unsigned len;
    int      opened_frame;   /* bit 17 was set when the transaction started */
} txn[TXN_MAX];
static unsigned ntxn;
static unsigned cur_len;
static int      cur_frame;
static int      cur_open;

/* The write FIFO. */
static uint32_t fifo[8];
static unsigned nfifo;

/* When 0 the model refuses to complete a transfer, which is how the timeout
 * path gets exercised. */
static int model_completes = 1;

static void model_reset(void)
{
    memset(lcdc_reg, 0, sizeof(lcdc_reg));
    memset(cru_reg, 0, sizeof(cru_reg));
    memset(pada_reg, 0, sizeof(pada_reg));
    memset(padb_reg, 0, sizeof(padb_reg));
    memset(txn, 0, sizeof(txn));
    ntxn = cur_len = nfifo = 0;
    cur_frame = cur_open = 0;
    model_completes = 1;
}

static void emit(uint8_t b)
{
    unsigned slot = (ntxn < TXN_MAX) ? ntxn : TXN_MAX - 1;

    if (!cur_open) {
        cur_open = 1;
        cur_len = 0;
        cur_frame = (int)((lcdc_reg[SL6806_LCDC_XFER / 4] >> 17) & 1u);
    }
    if (cur_len < TXN_BYTES)
        txn[slot].data[cur_len] = b;
    cur_len++;
}

static void close_txn(void)
{
    if (!cur_open)
        return;
    if (ntxn < TXN_MAX) {
        txn[ntxn].len = cur_len;
        txn[ntxn].opened_frame = cur_frame;
        ntxn++;
    }
    cur_open = 0;
    cur_len = 0;
}

/* One transfer, decoded the way the controller must: source from bits [3:2],
 * length from +0x28, chip select from bit 18. */
static void model_transfer(void)
{
    uint32_t xfer = lcdc_reg[SL6806_LCDC_XFER / 4];
    unsigned mode = (xfer >> 2) & 3u;
    unsigned cs   = (xfer >> 18) & 1u;
    unsigned n, i;

    if (mode == 1) {
        emit((uint8_t)lcdc_reg[SL6806_LCDC_CMD0 / 4]);
    } else if (mode == 2) {
        n = (lcdc_reg[SL6806_LCDC_LENGTH / 4] & 0xFFFFu) + 1u;
        for (i = 0; i < n; i++)
            emit((uint8_t)(fifo[i / 4] >> ((i % 4) * 8)));
    }
    nfifo = 0;

    if (!cs)
        close_txn();

    if (model_completes)
        lcdc_reg[SL6806_LCDC_STATUS / 4] |= SL6806_LCDC_STAT_DONE;
}

/* Map an address to its backing word, or NULL. */
static uint32_t *slot_of(uint32_t addr)
{
    if (addr >= LCDC_BASE && addr < LCDC_BASE + LCDC_SPAN)
        return &lcdc_reg[(addr - LCDC_BASE) / 4];
    if (addr >= CRU_BASE && addr < CRU_BASE + CRU_SPAN)
        return &cru_reg[(addr - CRU_BASE) / 4];
    if (addr >= PADA_BASE && addr < PADA_BASE + PADA_SPAN)
        return &pada_reg[(addr - PADA_BASE) / 4];
    if (addr >= PADB_BASE && addr < PADB_BASE + PADB_SPAN)
        return &padb_reg[(addr - PADB_BASE) / 4];
    return NULL;
}

uint32_t sl6806_mmio_read(uint32_t addr)
{
    uint32_t *p = slot_of(addr);

    if (!p) {
        printf("  FAIL: read of unmapped address 0x%08X\n", addr);
        failures++;
        return 0;
    }
    return *p;
}

void sl6806_mmio_write(uint32_t addr, uint32_t value)
{
    uint32_t *p = slot_of(addr);

    if (!p) {
        printf("  FAIL: write of 0x%08X to unmapped 0x%08X\n", value, addr);
        failures++;
        return;
    }

    if (addr == LCDC_BASE + SL6806_LCDC_FIFO_WR) {
        if (nfifo < sizeof(fifo) / sizeof(fifo[0]))
            fifo[nfifo++] = value;
        else {
            printf("  FAIL: FIFO overrun - a transfer pushed more than %u"
                   " words\n", (unsigned)(sizeof(fifo) / sizeof(fifo[0])));
            failures++;
        }
        return;
    }
    if (addr == LCDC_BASE + SL6806_LCDC_STATUS) {
        *p &= ~value;            /* write 1 to clear */
        return;
    }

    *p = value;

    if (addr == LCDC_BASE + SL6806_LCDC_XFER && (value & SL6806_LCDC_XFER_GO)) {
        *p &= ~SL6806_LCDC_XFER_GO;
        model_transfer();
    }
}

/* ------------------------------------------------------------------ stubs */

void delay(uint32_t ms)             { (void)ms; }
void delayMicroseconds(uint32_t us) { (void)us; }
void yield(void)                    { }

static uint32_t stub_block_limit;
uint32_t sl6806_block_limit(void)        { return stub_block_limit; }
void sl6806_set_block_limit(uint32_t ms) { stub_block_limit = ms; }

static int quiet = 1;
void sl6806_debug_print(const char *s)
{
    if (!quiet)
        fputs(s, stdout);
}

/* --------------------------------------------------------------- the board */

/*
 * The P20's values, copied here rather than linked from the variant, so that
 * changing the variant shows up as a failure rather than silently changing
 * what is being tested.
 */
static const uint32_t p20_pads[] = {
    0x00010930u, 0x00011130u, 0x00011930u, 0x00012130u,
    0x00012930u, 0x00013130u, 0x00014130u, 0x000138CBu,
};

static const sl6806_lcdc_config_t p20_cfg = {
    .color_format     = 1,
    .cmd_opcode       = 0x02,
    .pixel_opcode     = 0x32,
    .swap_pixel_bytes = 1,
    .modclk           = 0x910u,
    .map0             = 0xB0C432DBu,
    .map1             = 0xBBBBBBBFu,
    .pads             = p20_pads,
    .npads            = sizeof(p20_pads) / sizeof(p20_pads[0]),
    .reset_pad        = 0x000138CBu,
};

/* Bank 1's registers, from the ROM's bank table. */
#define BANK1 0x40081040u

static int begin_clean(void)
{
    model_reset();
    sl6806_lcd_bus_register(0);
    return sl6806_lcdc_begin(&p20_cfg);
}

/* ---------------------------------------------------------------- helpers */

static void dump_txn(unsigned i)
{
    unsigned k;

    printf("    txn %u:", i);
    for (k = 0; k < txn[i].len && k < 24; k++)
        printf(" %02X", txn[i].data[k]);
    if (txn[i].len > 24)
        printf(" ... (%u bytes)", txn[i].len);
    printf("\n");
}

static int txn_is(unsigned i, const uint8_t *want, unsigned n)
{
    if (i >= ntxn || txn[i].len != n || memcmp(txn[i].data, want, n) != 0) {
        if (i < ntxn)
            dump_txn(i);
        return 0;
    }
    return 1;
}

/* --------------------------------------------------------------- the tests */

static void test_bringup(void)
{
    unsigned i;

    CHECK(begin_clean() == 0, "sl6806_lcdc_begin failed");
    CHECK(sl6806_lcdc_bus() != NULL, "no bus after begin");
    CHECK(sl6806_lcd_bus() == sl6806_lcdc_bus(), "begin did not register the bus");
    CHECK(sl6806_lcdc_timed_out == 0, "timeout flagged on a healthy controller");

    /* HAL_lcdc_module_init's constants, 0x00829A28. */
    CHECK((lcdc_reg[SL6806_LCDC_CFG0 / 4] & 0xFFFFu) == 0x9999u,
          "CFG0 = 0x%08X, want the four 9s", lcdc_reg[SL6806_LCDC_CFG0 / 4]);
    CHECK((lcdc_reg[SL6806_LCDC_CTRL / 4] & 1u) == 1u, "CTRL bit 0 not set");
    CHECK((lcdc_reg[SL6806_LCDC_CTRL / 4] & 8u) == 0u, "CTRL bit 3 set");
    CHECK((lcdc_reg[SL6806_LCDC_CFG1 / 4] & 0x6Eu) == 0x48u,
          "CFG1 = 0x%08X, want bits 3 and 6 only", lcdc_reg[SL6806_LCDC_CFG1 / 4]);

    /* The lane map. */
    CHECK(lcdc_reg[SL6806_LCDC_MAP0 / 4] == 0xB0C432DBu, "MAP0 wrong");
    CHECK(lcdc_reg[SL6806_LCDC_MAP1 / 4] == 0xBBBBBBBFu, "MAP1 wrong");

    /* The module clock: field 0xF10 replaced, then enabled. */
    CHECK((cru_reg[0x10C / 4] & 0xF10u) == 0x910u,
          "CRU MODCLK = 0x%08X", cru_reg[0x10C / 4]);
    CHECK((cru_reg[0x10C / 4] & 1u) == 1u, "CRU MODCLK not enabled");

    /* Both module gates end up on. */
    CHECK((cru_reg[0x64 / 4] & 0x8000u) != 0, "CRU gate 0 left off");
    CHECK((cru_reg[0x74 / 4] & 0x8000u) != 0, "CRU gate 1 left off");

    /* Bank 1 pins 1..6 and 8 are the data pads, on function 2; pin 7 is the
     * reset line, on function 1 (output). */
    for (i = 1; i <= 8; i++) {
        uint32_t w = sl6806_mmio_read(BANK1 + (i >> 3) * 4);
        unsigned nib = (w >> ((i & 7) * 4)) & 0xFu;
        unsigned want = (i == 7) ? SL6806_PAD_FUNC_OUTPUT : 2u;

        CHECK(nib == want, "bank 1 pin %u function %u, want %u", i, nib, want);
    }

    /* The reset pad's id says "initially high", so pad setup must have set
     * it, not cleared it: the panel is not left held in reset. */
    CHECK((sl6806_mmio_read(BANK1 + 0x34) & (1u << 7)) != 0,
          "reset pad was not driven high by pad setup");

    /* Nothing should have gone out on the wire yet. */
    CHECK(ntxn == 0, "%u transactions during bring-up, want 0", ntxn);
}

static void test_command_frames(void)
{
    const sl6806_lcd_bus_t *bus;
    static const uint8_t want_slpout[] = { 0x02, 0x00, 0x11, 0x00 };
    static const uint8_t want_colmod[] = { 0x02, 0x00, 0x3A, 0x00, 0x55 };
    static const uint8_t want_caset[]  = { 0x02, 0x00, 0x2A, 0x00,
                                           0x00, 0x00, 0x00, 0xEF };
    static const uint8_t one_param[]   = { 0x55 };
    static const uint8_t four_params[] = { 0x00, 0x00, 0x00, 0xEF };

    CHECK(begin_clean() == 0, "begin failed");
    bus = sl6806_lcd_bus();

    /* A bare command: one four-byte frame, one transaction. */
    bus->command(0x11, NULL, 0);
    CHECK(ntxn == 1, "SLPOUT made %u transactions, want 1", ntxn);
    CHECK(txn_is(0, want_slpout, sizeof(want_slpout)), "SLPOUT frame wrong");

    /* One parameter, in the same transaction as its command. */
    bus->command(0x3A, one_param, 1);
    CHECK(ntxn == 2, "COLMOD made %u transactions total, want 2", ntxn);
    CHECK(txn_is(1, want_colmod, sizeof(want_colmod)), "COLMOD frame wrong");

    /* Four parameters, still one transaction. This is the case that breaks
     * if `last` is computed wrongly, and it breaks silently. */
    bus->command(0x2A, four_params, 4);
    CHECK(txn_is(2, want_caset, sizeof(want_caset)), "CASET frame wrong");

    /* Every one of those opened a new frame; only RAMWR does not. */
    CHECK(txn[0].opened_frame && txn[1].opened_frame && txn[2].opened_frame,
          "a command went out without bit 17 set");
}

/* A 2x2 rectangle, not at the origin, pushed through the whole DCS layer so
 * the window arithmetic and the bus meet the way they do on the device. */
static void test_flush(void)
{
    extern const sl6806_dcs_panel_t sl6806_p20_panel;
    static const sl6806_color_t src[4] = { 0x1234, 0x5678, 0x9ABC, 0xDEF0 };
    static const uint8_t want_caset[] = { 0x02, 0x00, 0x2A, 0x00,
                                          0x00, 0x03, 0x00, 0x04 };
    static const uint8_t want_raset[] = { 0x02, 0x00, 0x2B, 0x00,
                                          0x00, 0x0F, 0x00, 0x10 };
    static const uint8_t want_pixels[] = { 0x32, 0x00, 0x2C, 0x00,
                                           0x12, 0x34, 0x56, 0x78,
                                           0x9A, 0xBC, 0xDE, 0xF0 };

    CHECK(begin_clean() == 0, "begin failed");

    sl6806_dcs_flush(&sl6806_p20_panel, 3, 3, 2, 2, src);

    CHECK(ntxn == 3, "flush made %u transactions, want 3", ntxn);

    /* x is unshifted, y carries the panel's +12 offset - the same property
     * test_panel.c checks at the bus API, now at the byte level. */
    CHECK(txn_is(0, want_caset, sizeof(want_caset)), "CASET wrong");
    CHECK(txn_is(1, want_raset, sizeof(want_raset)), "RASET wrong");

    /* The pixel transaction: four-lane opcode, no new frame, pixels most
     * significant byte first. */
    CHECK(txn_is(2, want_pixels, sizeof(want_pixels)), "pixel stream wrong");
    CHECK(txn[2].opened_frame == 0,
          "the pixel transaction set bit 17; it must continue the frame");
}

/*
 * The case pixels_end() exists for. A rectangle clipped by the right edge
 * keeps its original stride, so its rows are not contiguous and reach the
 * bus as one pixels() call each. It must still be one transaction, and it
 * must contain the left-hand columns only.
 */
static void test_flush_clipped(void)
{
    extern const sl6806_dcs_panel_t sl6806_p20_panel;
    static const sl6806_color_t src[12] = {
        0x0102, 0x0304, 0x0506, 0x0708,
        0x090A, 0x0B0C, 0x0D0E, 0x0F10,
        0x1112, 0x1314, 0x1516, 0x1718,
    };
    static const uint8_t want[] = { 0x32, 0x00, 0x2C, 0x00,
                                    0x01, 0x02, 0x03, 0x04,
                                    0x09, 0x0A, 0x0B, 0x0C,
                                    0x11, 0x12, 0x13, 0x14 };

    CHECK(begin_clean() == 0, "begin failed");

    /* 4 wide at x=238 on a 240-wide panel: clipped to 2, stride stays 4. */
    sl6806_dcs_flush(&sl6806_p20_panel, 238, 0, 4, 3, src);

    CHECK(ntxn == 3, "clipped flush made %u transactions, want 3", ntxn);
    CHECK(txn_is(2, want, sizeof(want)), "clipped pixel stream wrong");
}

/* Longer than the 16-byte FIFO, so the stream is several transfers, and only
 * the last of them may release chip select. */
static void test_long_stream(void)
{
    extern const sl6806_dcs_panel_t sl6806_p20_panel;
    static sl6806_color_t src[64];
    unsigned i;

    for (i = 0; i < 64; i++)
        src[i] = (sl6806_color_t)(0x0101u * i);

    CHECK(begin_clean() == 0, "begin failed");
    sl6806_dcs_flush(&sl6806_p20_panel, 0, 0, 8, 8, src);

    CHECK(ntxn == 3, "long flush made %u transactions, want 3", ntxn);
    CHECK(txn[2].len == 4u + 128u,
          "pixel transaction is %u bytes, want %u", txn[2].len, 4u + 128u);
    CHECK(txn[2].data[4] == 0x00 && txn[2].data[5] == 0x00 &&
          txn[2].data[6] == 0x01 && txn[2].data[7] == 0x01,
          "first pixels wrong: %02X %02X %02X %02X",
          txn[2].data[4], txn[2].data[5], txn[2].data[6], txn[2].data[7]);
    CHECK(txn[2].data[130] == 0x3F && txn[2].data[131] == 0x3F,
          "last pixel wrong: %02X %02X", txn[2].data[130], txn[2].data[131]);
}

static void test_no_swap(void)
{
    sl6806_lcdc_config_t cfg = p20_cfg;
    const sl6806_lcd_bus_t *bus;
    static const sl6806_color_t src[1] = { 0x1234 };

    cfg.swap_pixel_bytes = 0;
    model_reset();
    sl6806_lcd_bus_register(0);
    CHECK(sl6806_lcdc_begin(&cfg) == 0, "begin failed");

    bus = sl6806_lcd_bus();
    bus->command(0x2C, NULL, 0);
    bus->pixels(src, 1);
    bus->pixels_end();

    CHECK(ntxn == 1, "%u transactions, want 1", ntxn);
    CHECK(txn[0].len == 6 && txn[0].data[4] == 0x34 && txn[0].data[5] == 0x12,
          "swap_pixel_bytes = 0 should send the low byte first");
}

static void test_reset_pin(void)
{
    const sl6806_lcd_bus_t *bus;

    CHECK(begin_clean() == 0, "begin failed");
    bus = sl6806_lcd_bus();

    CHECK(bus->reset != NULL, "no reset() on the bus");

    sl6806_mmio_write(BANK1 + 0x34, 0);
    sl6806_mmio_write(BANK1 + 0x38, 0);
    bus->reset(0);
    CHECK(sl6806_mmio_read(BANK1 + 0x38) == (1u << 7),
          "reset(0) did not clear pin 7");
    bus->reset(1);
    CHECK(sl6806_mmio_read(BANK1 + 0x34) == (1u << 7),
          "reset(1) did not set pin 7");
}

/* The whole recovered init sequence, byte for byte on the wire: variant
 * tables -> command stream -> QSPI frames. */
static void test_real_init_sequence(void)
{
    extern const sl6806_dcs_panel_t sl6806_p20_panel;
    unsigned i;
    int saw_colmod = 0;

    CHECK(begin_clean() == 0, "begin failed");
    CHECK(sl6806_dcs_begin(&sl6806_p20_panel) == 0, "dcs_begin failed");

    CHECK(ntxn == 33, "init produced %u transactions, want 33", ntxn);

    for (i = 0; i < ntxn; i++) {
        CHECK(txn[i].len >= 4, "transaction %u is only %u bytes", i, txn[i].len);
        CHECK(txn[i].data[0] == 0x02, "transaction %u opcode %02X, want 02",
              i, txn[i].data[0]);
        CHECK(txn[i].data[1] == 0x00 && txn[i].data[3] == 0x00,
              "transaction %u frame padding wrong", i);
        CHECK(txn[i].opened_frame, "transaction %u did not open a frame", i);
    }

    /* Spot-check the ends, and the one parameter whose value matters most:
     * COLMOD 0x55 is what makes the panel RGB565. */
    CHECK(txn[0].data[2] == 0xFD, "first command is %02X, want FD",
          txn[0].data[2]);
    CHECK(txn[ntxn - 1].data[2] == 0x29, "last command is %02X, want DISPON",
          txn[ntxn - 1].data[2]);
    for (i = 0; i < ntxn; i++) {
        if (txn[i].data[2] == 0x3A) {
            saw_colmod = 1;
            CHECK(txn[i].len == 5 && txn[i].data[4] == 0x55,
                  "COLMOD parameter is %02X, want 55", txn[i].data[4]);
        }
    }
    CHECK(saw_colmod, "the init sequence never sets COLMOD");
}

/* A controller that never says "done". The driver must come back, flag it,
 * and stop driving - not spin, because in RUN_MODE=poll this code runs
 * inside the boot ROM's USB handler. */
static void test_timeout(void)
{
    const sl6806_lcd_bus_t *bus;

    CHECK(begin_clean() == 0, "begin failed");
    bus = sl6806_lcd_bus();

    model_completes = 0;
    bus->command(0x11, NULL, 0);

    CHECK(sl6806_lcdc_timed_out == 1, "timeout not reported");
    CHECK(sl6806_lcdc_bus() == NULL, "bus still live after a timeout");

    /* And it goes quiet rather than retrying on every later call. */
    bus->command(0x11, NULL, 0);
    CHECK(sl6806_lcdc_timed_out == 1, "timeout flag changed");
}

int main(void)
{
    printf("test_lcdc\n");

    test_bringup();
    test_command_frames();
    test_flush();
    test_flush_clipped();
    test_long_stream();
    test_no_swap();
    test_reset_pin();
    test_real_init_sequence();
    test_timeout();

    printf("  %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
