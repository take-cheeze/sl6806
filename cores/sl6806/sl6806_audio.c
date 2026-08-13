/*
 * sl6806_audio.c - the audio controller at 0x40009000.
 *
 * Every register write below is somewhere in the vendor's own bring-up, and
 * the comments say where. Nothing has been run on hardware. See the header
 * for how the block was identified and for what is inferred rather than read.
 */

#include "sl6806_audio.h"
#include "sl6806_module.h"
#include "sl6806_romclk.h"
#include "wiring_time.h"

/*
 * The romclk family is a table, not a call: sl6806_romclk.h explains why it
 * is transcribed rather than invoked (the veneer the application uses is not
 * resident in bootloader mode). Each entry is one OR into one register.
 */
static int romclk_enable(unsigned id)
{
    unsigned i;

    for (i = 0; i < SL6806_ROMCLK_COUNT; i++) {
        if (sl6806_romclk[i].id == id) {
            sl6806_mmio_set(sl6806_romclk[i].addr, sl6806_romclk[i].bit);
            return 1;
        }
    }
    return 0;
}

/*
 * The audio PLL. 0x00807300, minus its scheduler critical section.
 *
 * The poll is bounded for the reason every poll in this codebase is bounded:
 * in payload mode this can run inside the boot ROM's USB handler, and a spin
 * there takes the device off the bus with no log. If the busy bit never
 * clears the writes go ahead anyway - which is what the vendor would have
 * done on the next call regardless, and is strictly more informative than
 * hanging.
 */
static void audio_pll_set(uint32_t rate)
{
    uint32_t sel, ratio, v;
    unsigned i;

    if (sl6806_audio_mclk_for(rate) == SL6806_AUD_MCLK_48K) {
        sel   = SL6806_AUD_PLL_48K_SEL;
        ratio = SL6806_AUD_PLL_48K_RATIO;
    } else {
        sel   = SL6806_AUD_PLL_44K1_SEL;
        ratio = SL6806_AUD_PLL_44K1_RATIO;
    }

    for (i = 0; i < 1000u; i++)
        if (!(sl6806_mmio_read(SL6806_AUD_PLL_RATIO) & SL6806_AUD_PLL_BUSY))
            break;

    v = sl6806_mmio_read(SL6806_AUD_PLL_SEL);
    v = (v & SL6806_AUD_PLL_SEL_KEEP) | SL6806_AUD_PLL_SEL_SET
        | (sel << SL6806_AUD_PLL_SEL_SHIFT);
    sl6806_mmio_write(SL6806_AUD_PLL_SEL, v);

    v = sl6806_mmio_read(SL6806_AUD_PLL_RATIO);
    v = (v & SL6806_AUD_PLL_RATIO_KEEP) | SL6806_AUD_PLL_RATIO_SET
        | (ratio << SL6806_AUD_PLL_RATIO_SHIFT);
    sl6806_mmio_write(SL6806_AUD_PLL_RATIO, v);
}

/*
 * ---------------------------------------------------------------------
 *  [M] 2026-08-13: THE FIRST VERSION OF THIS FUNCTION LIED, AND HOW
 * ---------------------------------------------------------------------
 * It wrote 0x5A5A5A50 to the buffer-address register and compared for
 * equality. On hardware that came back false while the block was plainly
 * awake - every DMA and volume register in it had just changed from zero to a
 * sensible reset default. 0x5A5A5A50 is not a valid SRAM address on this
 * chip, and a DMA address register that ignores or masks the bits above its
 * memory map is completely ordinary.
 *
 * So the probe now writes a value the register could actually hold. This is
 * the same failure sl6806_pwm.c documents ("writing 0x40 and comparing the
 * read-back for equality fails on a chip where the block is already
 * running") in a new costume: a write test is only a test if the value is one
 * the register is allowed to keep.
 */
int sl6806_audio_writable(void)
{
    uint32_t saved, probe;

    /* An address inside SRAM, word-aligned, and different from the reset
     * default of 0x00800000 so a stuck register cannot pass. */
    saved = sl6806_mmio_read(SL6806_AUD_TX_ADDR);
    sl6806_mmio_write(SL6806_AUD_TX_ADDR, 0x00830000u);
    probe = sl6806_mmio_read(SL6806_AUD_TX_ADDR);
    sl6806_mmio_write(SL6806_AUD_TX_ADDR, saved);
    if (probe == 0x00830000u)
        return 1;

    /*
     * Second opinion, on the least ambiguous field in the block: the 9-bit
     * playback level, whose reset value is measured at 0x1FF inside a word of
     * 0x0000A1FF. If the address register turns out to be write-protected
     * until a transfer is armed, this still answers.
     */
    saved = sl6806_mmio_read(SL6806_AUD_TX_VOL(0));
    sl6806_mmio_field(SL6806_AUD_TX_VOL(0), 0, 9, 0x155u);
    probe = sl6806_mmio_read(SL6806_AUD_TX_VOL(0)) & 0x1FFu;
    sl6806_mmio_write(SL6806_AUD_TX_VOL(0), saved);
    return probe == 0x155u;
}

int sl6806_audio_begin(uint32_t rate)
{
    unsigned ch;

    /* [V] 0x00D9674A, before either clock. */
    sl6806_mmio_set(SL6806_AUD_MISC_REG, SL6806_AUD_MISC_BIT);

    if (!sl6806_module_enable(SL6806_AUD_MODULE_ID))
        return 0;
    romclk_enable(SL6806_AUD_ROMCLK_ID);

    /*
     * [M] 2026-08-13: this used to bail out here if sl6806_audio_writable()
     * said no, and that was the wrong call twice over. The write test was
     * broken (see its comment), and even a correct one should not veto: the
     * evidence that this block wakes is that thirty of its registers go from
     * flat zero to sensible reset defaults the moment module 37 and romclk 19
     * are on, which the probe measured. Refusing to configure a block on the
     * strength of one scratch write throws that away and reports nothing.
     *
     * So: configure regardless, and let the DMA's own completion flag be the
     * evidence. A caller that wants the diagnostic can still ask.
     */
    /*
     * [V] The hardware init, 0x00D94A6C: enable, settle, then the three
     * playback levels to maximum. The 20 ms wait is the vendor's own; the
     * coefficient-RAM clear that follows it there is deliberately not done
     * here - it needs a second clock domain and this driver never programs a
     * coefficient.
     */
    sl6806_mmio_set(SL6806_AUD_TX_ENABLE, SL6806_AUD_TX_EN_BIT);
    delay(20);

    for (ch = 0; ch < SL6806_AUD_TX_CHANNELS; ch++)
        sl6806_mmio_field(SL6806_AUD_TX_VOL(ch), 0, 9, SL6806_AUD_VOL_MAX);

    /*
     * [V] And the block init, 0x00D96764: clear the four fields the vendor
     * clears, then arm +0x3C bit 10. The three 0x10000 clears are the mute
     * bits of the same three channels whose levels were just set, which is
     * the reading of bit 16 the header marks [I].
     */
    sl6806_mmio_field(SL6806_AUD_REG(0x100), 12, 4, 0);
    for (ch = 0; ch < SL6806_AUD_TX_CHANNELS; ch++)
        sl6806_mmio_field(SL6806_AUD_TX_VOL_CTRL(ch), 16, 1, 0);
    sl6806_mmio_field(SL6806_AUD_REG(0x3C), 10, 1, 1);

    /*
     * The sample-rate clock. THE ONE PLACE THIS DEVIATES FROM THE VENDOR'S
     * ORDER, and it is worth flagging: the vendor programs the audio PLL at
     * stream-open time (0x00D979E0), not at init, because opening a stream
     * is where it first learns the rate. There is no separate open here, so
     * it happens at the end of bring-up instead - after every register write
     * the init does, and before the DAC is enabled.
     */
    audio_pll_set(rate);

    /* [V] The DAC enables, 0x00D93FB0 and 0x00D93FBC - separately, in this
     * order, each as its own read-modify-write. */
    sl6806_mmio_field(SL6806_AUD_CTRL, 24, 1, 1);
    sl6806_mmio_field(SL6806_AUD_CTRL, 25, 1, 1);

    /* [M] And the bit clock, without which the DMA retires a 25 ms buffer in
     * 10 us. The vendor does this at stream start rather than at init; there
     * is no separate stream here. Mode 1 is a guess - see the header. */
    sl6806_audio_clock_start(1);

    return 1;
}

void sl6806_audio_end(void)
{
    sl6806_audio_stop();
    sl6806_mmio_field(SL6806_AUD_CTRL, 25, 1, 0);
    sl6806_mmio_field(SL6806_AUD_CTRL, 24, 1, 0);
}

/*
 * [V] The stream start, 0x00D9662C - the routine that turns out to carry the
 * bit clock. See the long note in the header for why a 25 ms buffer was
 * completing in 10 us without it.
 *
 * ROM 0x2B1A, the third clock family's handler for id 44, is four
 * instructions: 0x40080094[10:8] = value - 1. Transcribed rather than called,
 * for the same reason sl6806_romclk.h transcribes the other family - the
 * veneer the application uses is not resident in bootloader mode.
 */
void sl6806_audio_clock_start(unsigned mode)
{
    /* See the header: enable rather than the vendor's disable-then-enable. */
    sl6806_module_enable(SL6806_AUD_STREAM_MODULE_ID);
    delay(10);

    sl6806_mmio_field(SL6806_AUD_BITCLK_REG, SL6806_AUD_BITCLK_DIV_SHIFT,
                      SL6806_AUD_BITCLK_DIV_BITS, SL6806_AUD_BITCLK_DIV - 1u);
    sl6806_mmio_set(SL6806_AUD_BITCLK_REG, SL6806_AUD_BITCLK_ENABLE);

    sl6806_mmio_set(SL6806_AUD_EQ_CTRL, SL6806_AUD_EQ_CTRL_START);

    /* [V] One of three 3-bit source fields moved from its reset value of 4
     * to 6. Which one is the open question. */
    switch (mode) {
    case 1:  sl6806_mmio_field(SL6806_AUD_REG(0x80), 9, 3, 6); break;
    case 2:  sl6806_mmio_field(SL6806_AUD_REG(0x7C), 24, 3, 6); break;
    case 3:  sl6806_mmio_field(SL6806_AUD_REG(0x7C), 3, 3, 6); break;
    default: break;
    }
}

void sl6806_audio_clock_stop(void)
{
    sl6806_mmio_clr(SL6806_AUD_EQ_CTRL, SL6806_AUD_EQ_CTRL_START);
    sl6806_mmio_clr(SL6806_AUD_BITCLK_REG, SL6806_AUD_BITCLK_ENABLE);
}

/*
 * [V] The output route, 0x00D93F54 for channel 0 and 0x00D94028 for channel 1
 * - the same function twice, every constant of the second shifted left 16.
 *
 * [M] 2026-08-13 is why this exists at all: ToneDemo ran a complete transfer
 * with +0x08 reading 0x00000000, because nothing in the framework had ever
 * written it. The DMA moved 4796 bytes and finished in under a millisecond,
 * which is what an audio path with no output stage does.
 *
 * The four cases, as the vendor builds them:
 *
 *     route 0   v = (v & ~0x0041) | 0x2080 | 3   and 0x40009080[14:12] = 0
 *     route 1   v =  v           | 0x2040 | 3
 *     route 2   v = (v & ~0x2000)         | 2    and 0x40009014 |= 3
 *     route 3   v =  v           | 0x2040 | 3    and 0x40009014 |= 3
 *
 * then a 2 ms wait, then the 5-bit field at [12:8] from a driver-state byte
 * this analysis did not follow (`trim` here), then the two DAC enables.
 *
 * [?] WHICH ROUTE IS WHICH OUTPUT. The strings around the state byte the
 * vendor selects on are `-play status to spk play` and `-audio switch
 * earphone`, so these are speaker / headphone / line / something. Route 0 is
 * the only one that sets bit 7 and the only one that touches 0x40009080;
 * routes 2 and 3 are the only ones that touch 0x40009014. Nothing says more
 * than that, which is why examples/ToneDemo tries all four.
 */
void sl6806_audio_route(unsigned ch, unsigned route, unsigned trim)
{
    unsigned shift = (ch == 1) ? SL6806_AUD_DAC_CH1_SHIFT : 0;
    uint32_t v;

    if (ch > 1)
        return;

    v = sl6806_mmio_read(SL6806_AUD_DAC);

    switch (route) {
    case 0:
        v &= ~(0x0041u << shift);
        v |= (0x2080u | 3u) << shift;
        sl6806_mmio_field(SL6806_AUD_REG(0x80), 12, 3, 0);
        break;
    case 2:
        v &= ~(0x2000u << shift);
        v |= 2u << shift;
        sl6806_mmio_set(SL6806_AUD_REG(0x14), 3u);
        break;
    case 3:
        v |= (0x2040u | 3u) << shift;
        sl6806_mmio_set(SL6806_AUD_REG(0x14), 3u);
        break;
    case 1:
        v |= (0x2040u | 3u) << shift;
        break;
    default:
        v |= (0x2000u | 2u) << shift;
        break;
    }

    sl6806_mmio_write(SL6806_AUD_DAC, v);
    delay(2);                           /* [V] the vendor's own wait */

    /* [V] The 5-bit field, written as its own read-modify-write afterwards
     * rather than folded into the word above - which is the vendor's order
     * and the reason for the delay between them. */
    sl6806_mmio_field(SL6806_AUD_DAC, 8 + shift, 5, trim);

    /* [V] And the two DAC enables again, which 0x00D93FB0 does at the end of
     * this same function - so a route change re-asserts them. */
    sl6806_mmio_field(SL6806_AUD_CTRL, 24, 1, 1);
    sl6806_mmio_field(SL6806_AUD_CTRL, 25, 1, 1);
}

int sl6806_audio_play(const void *buf, uint32_t len)
{
    uint32_t addr = (uint32_t)(uintptr_t)buf;

    /* [V] 0x0080D9BC rejects rather than rounds. */
    if (!buf || !len || (len & 3u) || (addr & 3u) || len > 0xFFFFu)
        return -1;

    sl6806_mmio_write(SL6806_AUD_TX_ADDR, addr);

    /* This read-modify-write acknowledges any interrupt flag that happens to
     * be set, because they are write-1-to-clear and it writes the word back.
     * The vendor's own submit has the same property, so it is kept rather
     * than worked around - but it means a flag raised between the last
     * sl6806_audio_done() and here is lost. */
    sl6806_mmio_field(SL6806_AUD_TX_CTRL, SL6806_AUD_LEN_SHIFT, 16, len);
    sl6806_mmio_write(SL6806_AUD_TX_TRIG, sl6806_audio_trigger_word(len));

    /* [V] 0x00D97A5E, the start, a separate write after the descriptor. */
    sl6806_mmio_set(SL6806_AUD_TX_CTRL, SL6806_AUD_START);
    return 0;
}

/*
 * [V] Capture, 0x0080D964 - the same four registers one page up.
 *
 * This exists for a diagnostic rather than for recording: a DMA engine that
 * *writes* memory can be caught doing it, and a TX engine cannot. Fill a
 * buffer with a pattern, submit it here, and if the pattern changes the
 * engine really moves data - which separates "the output stage is not
 * clocked" from "the completion flag is a lie and nothing runs at all".
 */
int sl6806_audio_capture(void *buf, uint32_t len)
{
    uint32_t addr = (uint32_t)(uintptr_t)buf;

    if (!buf || !len || (len & 3u) || (addr & 3u) || len > 0xFFFFu)
        return -1;

    sl6806_mmio_write(SL6806_AUD_RX_ADDR, addr);
    sl6806_mmio_field(SL6806_AUD_RX_CTRL, SL6806_AUD_LEN_SHIFT, 16, len);
    sl6806_mmio_write(SL6806_AUD_RX_TRIG, sl6806_audio_trigger_word(len));
    sl6806_mmio_set(SL6806_AUD_RX_CTRL, SL6806_AUD_START);
    return 0;
}

int sl6806_audio_capture_done(void)
{
    uint32_t v = sl6806_mmio_read(SL6806_AUD_RX_CTRL);

    if (v & SL6806_AUD_IRQ_ALL)
        sl6806_mmio_write(SL6806_AUD_RX_CTRL, v);
    return (v & SL6806_AUD_IRQ_DONE) != 0;
}

uint32_t sl6806_audio_flags(void)
{
    uint32_t v = sl6806_mmio_read(SL6806_AUD_TX_CTRL);

    /* [V] The handler's own acknowledgement: write the word straight back. */
    if (v & SL6806_AUD_IRQ_ALL)
        sl6806_mmio_write(SL6806_AUD_TX_CTRL, v);
    return v & SL6806_AUD_IRQ_ALL;
}

int sl6806_audio_done(void)
{
    return (sl6806_audio_flags() & SL6806_AUD_IRQ_DONE) != 0;
}

void sl6806_audio_stop(void)
{
    sl6806_mmio_clr(SL6806_AUD_TX_CTRL, SL6806_AUD_START);
}

void sl6806_audio_volume(unsigned ch, unsigned level)
{
    if (ch >= SL6806_AUD_TX_CHANNELS)
        return;
    if (level > SL6806_AUD_VOL_MAX)
        level = SL6806_AUD_VOL_MAX;
    sl6806_mmio_field(SL6806_AUD_TX_VOL(ch), 0, 9, level);
}

void sl6806_audio_mute(unsigned ch, int on)
{
    if (ch >= SL6806_AUD_TX_CHANNELS)
        return;
    sl6806_mmio_field(SL6806_AUD_TX_VOL_CTRL(ch), 16, 1, on ? 1u : 0u);
}
