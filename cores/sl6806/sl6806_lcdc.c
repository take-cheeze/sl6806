/*
 * sl6806_lcdc.c - LCD controller driver: the QSPI bus the display stack
 * has been waiting for.
 *
 * Read sl6806_lcdc.h first. In short: the panel is a QSPI display, the
 * controller at 0x400D9000 is the QSPI master, and every sequence below is a
 * transcription of the HLKJ bootloader's driver, which is in flash and so
 * could be disassembled. Bootloader addresses are in the comments so any
 * line here can be checked against the original.
 *
 * Two deliberate departures from the vendor:
 *
 *   - Pixels go out by polling, 16 bytes per transfer, instead of by DMA.
 *     The FIFO holds 16 bytes; the bootloader's own short-write helper at
 *     0x00821AA8 refuses more. This avoids needing a DMA driver to see a
 *     first picture, at the cost of about 8900 transfers per full frame.
 *
 *   - Every wait is bounded. In RUN_MODE=poll this code runs inside the boot
 *     ROM's USB handler, where spinning forever does not merely hang the
 *     sketch, it desynchronises the endpoint and the device stops answering
 *     until it is unplugged. A controller that never reports completion
 *     gives up and says so instead.
 */

#include "sl6806_lcdc.h"
#include "sl6806_cru.h"
#include "sl6806_mmio.h"
#include "sl6806_padctl.h"
#include "sl6806_console.h"
#include "wiring_time.h"

#define R(off)  (SL6806_LCDC_BASE + (uint32_t)(off))
#define CRU(off) (SL6806_CRU_BASE + (uint32_t)(off))

/*
 * How many polls of the status register a single transfer gets before the
 * driver decides the controller is not answering.
 *
 * A transfer is a handful of bytes at the panel's clock, so the real figure
 * is a few hundred polls at most. This is loose enough not to trip on a slow
 * clock setting and short enough that a dead controller costs milliseconds
 * rather than a session.
 */
#define POLL_LIMIT 200000u

/* The DCS command that starts a pixel stream. Hardcoded, exactly as the
 * vendor hardcodes it (`cmp r4, #44` at 0x00821BBE): the controller treats
 * RAMWR differently from every other command, so this is a property of the
 * bus, not of the panel description. */
#define DCS_RAMWR 0x2C

uint8_t sl6806_lcdc_timed_out;

static sl6806_lcdc_config_t g_cfg;
static uint8_t g_ready;
static uint8_t g_configured;
static sl6806_lcdc_stats_t g_stats = { 0, 0, 0, 0xFFFFFFFFu, 0, 0 };

const sl6806_lcdc_config_t *sl6806_lcdc_config(void)
{
    return g_configured ? &g_cfg : 0;
}

const sl6806_lcdc_stats_t *sl6806_lcdc_stats(void)
{
    return &g_stats;
}

uint32_t sl6806_lcdc_watch_addr;
uint32_t sl6806_lcdc_watch_ones;
uint32_t sl6806_lcdc_watch_zeros;
uint32_t sl6806_lcdc_watch_samples;

void sl6806_lcdc_watch(uint32_t addr)
{
    /* Starting a watch clears the accumulators; stopping one does not, or
     * the results would be wiped before anyone could read them. That is not
     * hypothetical - the first version did exactly that and reported "no pad
     * ever moved" from zero samples. */
    if (addr) {
        sl6806_lcdc_watch_ones = 0;
        sl6806_lcdc_watch_zeros = 0;
        sl6806_lcdc_watch_samples = 0;
    }
    sl6806_lcdc_watch_addr = addr;
}

void sl6806_lcdc_stats_reset(void)
{
    g_stats.transfers = 0;
    g_stats.timeouts = 0;
    g_stats.last_polls = 0;
    g_stats.min_polls = 0xFFFFFFFFu;
    g_stats.max_polls = 0;
    g_stats.busy_waits = 0;
}

/* Pixel bytes held back so that the last transfer of a stream - and only the
 * last - can be the one that releases CS. See pixels()/pixels_end(). */
static uint8_t  g_pend[16];
static unsigned g_npend;

/* ------------------------------------------------------------ plumbing */

static void report_timeout(void)
{
    if (sl6806_lcdc_timed_out)
        return;
    sl6806_lcdc_timed_out = 1;
    g_ready = 0;
    sl6806_debug_print(
        "\r\n*** SL6806: the LCD controller stopped answering ***\r\n"
        "    A transfer was started and 0x400D9014 never reported it\r\n"
        "    complete. The driver gave up rather than spin, so the screen\r\n"
        "    is frozen from here on. Most likely the module clock or the\r\n"
        "    gate in sl6806_cru.h is wrong for this board.\r\n\r\n");
}

/* [V] Every transfer waits for the busy bits before touching anything. */
static int wait_idle(void)
{
    uint32_t n = POLL_LIMIT;

    if (!(sl6806_mmio_read(R(SL6806_LCDC_STATUS)) & SL6806_LCDC_STAT_BUSY))
        return 0;

    g_stats.busy_waits++;
    while (sl6806_mmio_read(R(SL6806_LCDC_STATUS)) & SL6806_LCDC_STAT_BUSY) {
        if (!--n) {
            report_timeout();
            return -1;
        }
    }
    return 0;
}

/* [V] 0x008218D4 and friends clear these before every polled transfer, so
 * that the completion the driver is about to wait on cannot also raise the
 * interrupt the vendor's DMA path uses. */
static void irq_quiet(void)
{
    sl6806_mmio_field(R(SL6806_LCDC_IE), 8, 9, 0);
    sl6806_mmio_field(R(SL6806_LCDC_IE), 31, 1, 0);
    sl6806_mmio_field(R(SL6806_LCDC_IE), 22, 1, 0);
    sl6806_mmio_field(R(SL6806_LCDC_IE), 19, 1, 0);
}

/* [V] Start, wait for bit 31, write it back, wait for it to clear. The
 * write-back-and-recheck is the vendor's, not belt and braces: the bit is
 * write-1-to-clear and the next transfer's wait_idle() would otherwise see
 * the previous completion. */
static int go(void)
{
    uint32_t n;

    sl6806_mmio_set(R(SL6806_LCDC_XFER), SL6806_LCDC_XFER_GO);

    n = POLL_LIMIT;
    for (;;) {
        /* Sampled here, inside the wait and starting immediately after GO,
         * because that is the only window in which the bus is active. */
        if (sl6806_lcdc_watch_addr) {
            uint32_t v = sl6806_mmio_read(sl6806_lcdc_watch_addr);

            sl6806_lcdc_watch_ones  |= v;
            sl6806_lcdc_watch_zeros |= ~v;
            sl6806_lcdc_watch_samples++;
        }
        if (sl6806_mmio_read(R(SL6806_LCDC_STATUS)) & SL6806_LCDC_STAT_DONE)
            break;
        if (!--n) {
            g_stats.timeouts++;
            report_timeout();
            return -1;
        }
    }

    g_stats.last_polls = POLL_LIMIT - n;
    if (g_stats.last_polls < g_stats.min_polls)
        g_stats.min_polls = g_stats.last_polls;
    if (g_stats.last_polls > g_stats.max_polls)
        g_stats.max_polls = g_stats.last_polls;
    g_stats.transfers++;

    sl6806_mmio_set(R(SL6806_LCDC_STATUS), SL6806_LCDC_STAT_DONE);

    /* [V] What the vendor's ISR at 0x00827D6C does to the other two flags:
     * write the bit back, then spin until it reads 0 (0x00829C9C). */
    if (g_cfg.quirk_clear_status) {
        static const uint32_t flags[] = { 1u << 27, 1u << 22 };
        unsigned f;

        for (f = 0; f < 2; f++) {
            uint32_t k = POLL_LIMIT;

            sl6806_mmio_write(R(SL6806_LCDC_STATUS), flags[f]);
            while (sl6806_mmio_read(R(SL6806_LCDC_STATUS)) & flags[f])
                if (!--k)
                    break;
        }
    }

    n = POLL_LIMIT;
    while (sl6806_mmio_read(R(SL6806_LCDC_STATUS)) & SL6806_LCDC_STAT_DONE) {
        if (!--n) {
            report_timeout();
            return -1;
        }
    }
    return 0;
}

/* [V] 0x00821B88 / 0x00821B9C, with the polarity switchable - see the quirk
 * fields in sl6806_lcdc.h. */
static void cs_assert(void)
{
    if (g_cfg.quirk_invert_cs)
        sl6806_mmio_clr(R(SL6806_LCDC_XFER), SL6806_LCDC_XFER_CS);
    else
        sl6806_mmio_set(R(SL6806_LCDC_XFER), SL6806_LCDC_XFER_CS);
}

static void cs_release(void)
{
    if (g_cfg.quirk_invert_cs)
        sl6806_mmio_set(R(SL6806_LCDC_XFER), SL6806_LCDC_XFER_CS);
    else
        sl6806_mmio_clr(R(SL6806_LCDC_XFER), SL6806_LCDC_XFER_CS);
}

/* [V] 0x008218D4 - one byte of a command frame, out of +0x24. */
static int send_frame_byte(uint8_t b)
{
    sl6806_mmio_write(R(SL6806_LCDC_CMD0), b);
    sl6806_mmio_field(R(SL6806_LCDC_XFER), 8, 2, 0);
    sl6806_mmio_field(R(SL6806_LCDC_XFER), 2, 2, 1);
    sl6806_mmio_field(R(SL6806_LCDC_XFER), 20, 2, 0);

    if (wait_idle() != 0)
        return -1;
    irq_quiet();
    return go();
}

/* [V] 0x00821940, generalised from one word to the whole FIFO - the vendor
 * pushes a run of words the same way in 0x00821AA8. Up to 16 bytes. */
static int send_data(const uint8_t *buf, unsigned n)
{
    unsigned words = (n + 3u) / 4u;
    unsigned i;

    for (i = 0; i < words; i++) {
        unsigned k = i * 4u;
        uint32_t w = buf[k];

        if (k + 1 < n) w |= (uint32_t)buf[k + 1] << 8;
        if (k + 2 < n) w |= (uint32_t)buf[k + 2] << 16;
        if (k + 3 < n) w |= (uint32_t)buf[k + 3] << 24;
        sl6806_mmio_write(R(SL6806_LCDC_FIFO_WR), w);
    }

    sl6806_mmio_write(R(SL6806_LCDC_LENGTH), n - 1u);
    sl6806_mmio_field(R(SL6806_LCDC_XFER), 2, 2, 2);
    sl6806_mmio_field(R(SL6806_LCDC_XFER), 20, 2, 0);
    sl6806_mmio_set(R(SL6806_LCDC_XFER), SL6806_LCDC_XFER_WRITE);

    if (wait_idle() != 0)
        return -1;
    irq_quiet();
    return go();
}

/*
 * [V] 0x00821BB2 - the command frame: opcode, 0, command, 0.
 *
 * `last` says whether this command ends the transaction. A command with
 * parameters keeps CS asserted so the parameter bytes belong to it; a
 * command without them releases CS before its final byte, which is what the
 * vendor's `last` argument does and why the panel sees each bare command as
 * a complete frame.
 */
static int write_cmd(int last, uint8_t opcode, uint8_t cmd)
{
    cs_assert();

    /* [I] bit 17: emit a new frame. Cleared for RAMWR so the pixel stream
     * continues the transaction the frame opened. Polarity switchable. */
    if ((cmd != DCS_RAMWR) != (g_cfg.quirk_invert_frame != 0))
        sl6806_mmio_set(R(SL6806_LCDC_XFER), SL6806_LCDC_XFER_FRAME);
    else
        sl6806_mmio_clr(R(SL6806_LCDC_XFER), SL6806_LCDC_XFER_FRAME);

    if (send_frame_byte(opcode) != 0) return -1;
    if (send_frame_byte(0) != 0)      return -1;
    if (send_frame_byte(cmd) != 0)    return -1;
    if (!last)
        cs_release();
    return send_frame_byte(0);
}

/* [V] 0x00821C00. */
static int write_param(int last, uint8_t b)
{
    if (!last)
        cs_release();
    return send_data(&b, 1);
}

/*
 * [V] 0x00821C16 - the read twin of the combined command+data write at
 * 0x00821CC8, which is the one place the controller's two command fields are
 * used the way the hardware means them: +0x24 holds the lane opcode with its
 * length in bits [9:8], +0x2C holds the address - the DCS command shifted up
 * by 8 - with its length in bits [11:10], and then the data moves.
 *
 * That is also what confirms the QSPI reading: the vendor's window write
 * passes (0x02, 1, cmd << 8, 3, ...), which is a one-byte opcode followed by
 * a three-byte address whose middle byte is the command.
 *
 * Direction is bit 1, cleared here and set for writes.
 */
int sl6806_lcdc_read(uint8_t opcode, uint8_t cmd, unsigned addr_bytes,
                     uint8_t *dest, unsigned n)
{
    unsigned words, i;

    if (!g_ready || !dest || !n || n > 16 || !addr_bytes || addr_bytes > 4)
        return -1;

    sl6806_mmio_field(R(SL6806_LCDC_XFER), 10, 2, addr_bytes - 1);
    sl6806_mmio_field(R(SL6806_LCDC_XFER), 8, 2, 0);      /* one opcode byte */
    sl6806_mmio_write(R(SL6806_LCDC_CMD0), opcode);
    sl6806_mmio_write(R(SL6806_LCDC_CMD1), (uint32_t)cmd << 8);
    sl6806_mmio_field(R(SL6806_LCDC_LENGTH), 0, 16, n - 1u);
    sl6806_mmio_field(R(SL6806_LCDC_XFER), 2, 2, 3);      /* opcode + address */
    sl6806_mmio_clr(R(SL6806_LCDC_XFER), SL6806_LCDC_XFER_WRITE);

    cs_assert();
    if (wait_idle() != 0)
        return -1;
    irq_quiet();
    if (go() != 0)
        return -1;
    cs_release();

    words = (n + 3u) / 4u;
    for (i = 0; i < words; i++) {
        uint32_t w = sl6806_mmio_read(R(SL6806_LCDC_FIFO_RD));
        unsigned k;

        for (k = 0; k < 4 && i * 4u + k < n; k++)
            dest[i * 4u + k] = (uint8_t)(w >> (k * 8));
    }
    return 0;
}

/* ------------------------------------------------------------- the bus */

static void bus_command(uint8_t cmd, const uint8_t *args, uint8_t nargs)
{
    uint8_t i;

    if (!g_ready)
        return;

    /* A pixel stream is opened with the four-lane opcode and left open; the
     * pixels are its parameters. Everything else goes out on one lane. */
    if (cmd == DCS_RAMWR && nargs == 0) {
        g_npend = 0;
        write_cmd(1, g_cfg.pixel_opcode, cmd);
        return;
    }

    if (write_cmd(nargs ? 1 : 0, g_cfg.cmd_opcode, cmd) != 0)
        return;

    for (i = 0; i < nargs; i++)
        if (write_param((i + 1) < nargs, args[i]) != 0)
            return;
}

/* Push the staged bytes. `last` releases CS first, ending the transaction
 * on this transfer - the controller wants that decided before the transfer
 * starts, which is the whole reason for staging. */
static int emit_pending(int last)
{
    int rc;

    if (!g_npend)
        return 0;
    if (last)
        cs_release();
    rc = send_data(g_pend, g_npend);
    g_npend = 0;
    return rc;
}

static void stage(uint8_t b)
{
    if (g_npend == sizeof(g_pend))
        if (emit_pending(0) != 0)
            return;
    g_pend[g_npend++] = b;
}

static void bus_pixels(const sl6806_color_t *src, uint32_t count)
{
    uint32_t i;

    if (!g_ready)
        return;

    /* Nothing is flushed on the way out: the caller may be part way through
     * a rectangle whose rows are not contiguous, and only pixels_end() knows
     * where the stream really finishes. */
    if (g_cfg.swap_pixel_bytes) {
        for (i = 0; i < count; i++) {
            stage((uint8_t)(src[i] >> 8));
            stage((uint8_t)src[i]);
        }
    } else {
        for (i = 0; i < count; i++) {
            stage((uint8_t)src[i]);
            stage((uint8_t)(src[i] >> 8));
        }
    }
}

static void bus_pixels_end(void)
{
    if (g_ready)
        emit_pending(1);
}

static void bus_reset(uint8_t level)
{
    if (g_cfg.reset_pad)
        sl6806_pad_write(g_cfg.reset_pad, level);
}

static const sl6806_lcd_bus_t lcdc_bus = {
    .name       = "LCDC QSPI 0x400D9000",
    .command    = bus_command,
    .pixels     = bus_pixels,
    .pixels_end = bus_pixels_end,
    .reset      = bus_reset,
    .backlight  = 0,
};

const sl6806_lcd_bus_t *sl6806_lcdc_bus(void)
{
    return g_ready ? &lcdc_bus : 0;
}

/* ------------------------------------------------------------ bring-up */

/* [V] 0x00827FAC / application 0x00D3E944. Gate the module off and on, then
 * pick its clock. */
static void clocks(const sl6806_lcdc_config_t *cfg)
{
    sl6806_mmio_clr(CRU(SL6806_CRU_GATE0), SL6806_CRU_GATE_BIT);
    sl6806_mmio_clr(CRU(SL6806_CRU_GATE1), SL6806_CRU_GATE_BIT);
    delayMicroseconds(100);
    sl6806_mmio_set(CRU(SL6806_CRU_GATE1), SL6806_CRU_GATE_BIT);
    sl6806_mmio_set(CRU(SL6806_CRU_GATE0), SL6806_CRU_GATE_BIT);

    sl6806_mmio_write(CRU(SL6806_CRU_MODCLK),
                      (sl6806_mmio_read(CRU(SL6806_CRU_MODCLK)) &
                       ~SL6806_CRU_MODCLK_SEL_MASK) | cfg->modclk);
    sl6806_mmio_set(CRU(SL6806_CRU_MODCLK), SL6806_CRU_MODCLK_ENABLE);
}

/*
 * [V] HAL_lcdc_module_init, 0x00829A28.
 *
 * The vendor passes a 20-byte struct; both callers fill it with the same
 * constants except for the colour format, so the constants are inlined here
 * and only the format comes from the config. Each line names the config byte
 * it stands for so the two can be compared.
 */
static void module_init(const sl6806_lcdc_config_t *cfg)
{
    /* cfg[0] = 1: interface type 1. */
    sl6806_mmio_field(R(SL6806_LCDC_CTRL), 0, 1, 1);
    sl6806_mmio_field(R(SL6806_LCDC_CTRL), 3, 1, 0);

    /* cfg[1..4] = 9. */
    sl6806_mmio_field(R(SL6806_LCDC_CFG0), 12, 4, 9);
    sl6806_mmio_field(R(SL6806_LCDC_CFG0),  8, 4, 9);
    sl6806_mmio_field(R(SL6806_LCDC_CFG0),  4, 4, 9);
    sl6806_mmio_field(R(SL6806_LCDC_CFG0),  0, 4, 9);

    /* cfg[5] = the panel's colour mode, cfg[6] = 0, cfg[7] = 3, cfg[8] = 0. */
    sl6806_mmio_field(R(SL6806_LCDC_XFER), 20, 2, cfg->color_format);
    sl6806_mmio_field(R(SL6806_LCDC_XFER), 12, 4, 0);
    sl6806_mmio_field(R(SL6806_LCDC_XFER), 10, 2, 3);
    sl6806_mmio_field(R(SL6806_LCDC_XFER),  4, 1, 0);
    sl6806_mmio_field(R(SL6806_LCDC_XFER), 16, 1, 0);
    sl6806_mmio_field(R(SL6806_LCDC_XFER), 17, 1, 0);

    /* cfg[9] = 0, cfg[11] = 1, cfg[12] = 0, cfg[13] = 1, cfg[10] = 0. */
    sl6806_mmio_field(R(SL6806_LCDC_CFG1), 1, 1, 0);
    sl6806_mmio_field(R(SL6806_LCDC_CFG1), 6, 1, 1);
    sl6806_mmio_field(R(SL6806_LCDC_CFG1), 2, 1, 0);
    sl6806_mmio_field(R(SL6806_LCDC_CFG1), 3, 1, 1);
    sl6806_mmio_field(R(SL6806_LCDC_CFG1), 5, 1, 0);
}

/* [V] 0x008299C4 - bit 31, then bit 30, one tick apart. */
static void soft_reset(void)
{
    sl6806_mmio_set(R(SL6806_LCDC_CTRL), SL6806_LCDC_CTRL_RESET0);
    delayMicroseconds(1);
    sl6806_mmio_clr(R(SL6806_LCDC_CTRL), SL6806_LCDC_CTRL_RESET0);

    sl6806_mmio_set(R(SL6806_LCDC_CTRL), SL6806_LCDC_CTRL_RESET1);
    delayMicroseconds(1);
    sl6806_mmio_clr(R(SL6806_LCDC_CTRL), SL6806_LCDC_CTRL_RESET1);
}

int sl6806_lcdc_begin(const sl6806_lcdc_config_t *cfg)
{
    uint8_t i;

    if (!cfg || !cfg->cmd_opcode || !cfg->pixel_opcode)
        return -1;

    g_cfg = *cfg;
    g_configured = 1;
    g_npend = 0;
    g_ready = 0;
    sl6806_lcdc_timed_out = 0;
    sl6806_lcdc_stats_reset();

    clocks(&g_cfg);

    /*
     * The vendor resets the controller in its command-list builder, i.e.
     * after init and then again before every frame - which is evidence that
     * a reset does not clear the configuration, or the second frame would
     * never work. This driver resets once and does it before the
     * configuration anyway, because "configure last" is true whichever way
     * that evidence goes.
     */
    soft_reset();
    module_init(&g_cfg);

    if (g_cfg.quirk_ctrl_bit4)
        sl6806_mmio_set(R(SL6806_LCDC_CTRL), SL6806_LCDC_CTRL_CMDLIST);

    /* [V] 0x00828068 / application 0x00D3E9DC: the data pads, then the lane
     * map. The reset line is usually one of the pads, which is what leaves
     * it configured as a high output before the panel's reset pulse. */
    for (i = 0; i < g_cfg.npads; i++)
        sl6806_pad_configure(g_cfg.pads[i]);

    sl6806_mmio_write(R(SL6806_LCDC_MAP0), g_cfg.map0);
    sl6806_mmio_write(R(SL6806_LCDC_MAP1), g_cfg.map1);

    if (wait_idle() != 0)
        return -1;

    g_ready = 1;
    if (sl6806_lcd_bus_register(&lcdc_bus) != 0) {
        g_ready = 0;
        return -1;
    }
    return 0;
}
