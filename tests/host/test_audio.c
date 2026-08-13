/*
 * test_audio.c - native tests for the audio controller driver.
 *
 * Like test_regfile.c, and for the same reason, these are a different kind of
 * thing from the tests next to them: nothing in cores/sl6806/sl6806_audio.c
 * has ever run on hardware, so these cannot tell you the decode is right.
 * What they can do is hold the transcription to the sequence the vendor's
 * code performs, and every one of the following is easy to "tidy" into
 * something that would fail silently on a device nobody has:
 *
 *   1. **The descriptor order.** Address, then length, then the trigger
 *      word, and only then the start bit as its own read-modify-write. The
 *      vendor's 0x0080D9BC writes the address first and the trigger last, and
 *      0x00D97A5E starts the transfer in a separate function entirely. A
 *      block that latches its descriptor on START would not care; one that
 *      begins fetching when the length is written would play garbage.
 *
 *   2. **The three-quarter watermark.** +0x104 gets (len * 3 / 4) << 16, not
 *      len, not half. It is the refill interrupt's threshold, it looks like a
 *      duplicate of the length register, and deleting it is the obvious
 *      simplification.
 *
 *   3. **The rate split.** rate % 8000 chooses between 24.576 MHz and
 *      22.579 MHz - the 48 kHz family against the 44.1 kHz one. Getting this
 *      backwards produces audio at 8/7.35 of the right pitch, which is the
 *      kind of bug that gets diagnosed as "the sample format must be wrong".
 *
 *   4. **The rejections.** A length that is not a multiple of 4, or a buffer
 *      that is not word-aligned, is refused rather than rounded, because the
 *      vendor refuses it. Rounding it here would hand the hardware a
 *      descriptor the hardware was never given.
 *
 *   5. **Both DAC enables, separately.** Bits 24 and 25 of +0x00 are set by
 *      two consecutive read-modify-writes in 0x00D93FB0/0x00D93FBC. Folding
 *      them into one store is one bus cycle instead of two on a block whose
 *      write behaviour nobody has observed.
 *
 *   6. **The gate check.** A gated peripheral on this chip reads zero and
 *      drops writes. begin() must probe with a write and a read-back rather
 *      than believing a plausible read - the mistake that recorded the ADC as
 *      not working for four hardware sessions (notes 15b).
 */

#include <stdio.h>
#include <string.h>

#include "sl6806_audio.h"
#include "sl6806_module.h"

static int failures, checks;

#define CHECK(cond, what) do {                                          \
        checks++;                                                       \
        if (!(cond)) {                                                  \
            printf("  FAIL %s (%s:%d)\n", (what), __FILE__, __LINE__);  \
            failures++;                                                 \
        }                                                               \
    } while (0)

/* ------------------------------------------------------------------ */
/* A model of the block, its gate, and the CRU in front of both        */
/* ------------------------------------------------------------------ */

#define CRU_BASE    0x40080000u
#define AUD_WORDS   0x180       /* 0x000..0x5FC at word stride          */

static uint32_t aud[AUD_WORDS];
static uint32_t cru[0x80];
static uint32_t padmux;
static uint32_t misc;           /* 0x400F70D8                           */

/* The gate. Like the real block: until the module clock is on, every
 * register reads zero and every write is dropped. */
static int gated;

/* force_gated keeps the block dark even after the clock acknowledges - the
 * failure the ADC hit. no_ack makes the module gate itself never answer. */
static int force_gated, no_ack;

/* addr_mask models a DMA address register that only implements the bits its
 * memory map needs; addr_ro models one that is write-protected until armed.
 * Both are ordinary, and both defeated the original write test. */
static uint32_t addr_mask = 0xFFFFFFFFu;
static int addr_ro;

/* What the model saw, in order, so the sequence itself can be asserted. */
static char     trace[2048];

static void note(const char *fmt, uint32_t a, uint32_t v)
{
    char buf[64];

    snprintf(buf, sizeof(buf), fmt, a, v);
    if (strlen(trace) + strlen(buf) + 2 < sizeof(trace)) {
        strcat(trace, buf);
        strcat(trace, " ");
    }
}

/* Is `a` before `b` in the trace? Both must be present. */
static int before(const char *a, const char *b)
{
    const char *pa = strstr(trace, a);
    const char *pb = strstr(trace, b);

    return pa && pb && pa < pb;
}

uint32_t sl6806_mmio_read(uint32_t a)
{
    if (a >= SL6806_AUD_BASE && a < SL6806_AUD_BASE + AUD_WORDS * 4)
        return gated ? 0 : aud[(a - SL6806_AUD_BASE) / 4];
    if (a >= CRU_BASE && a < CRU_BASE + sizeof(cru))
        return cru[(a - CRU_BASE) / 4];
    if (a == SL6806_AUD_PADMUX_REG)
        return padmux;
    if (a == SL6806_AUD_MISC_REG)
        return misc;
    return 0;
}

void sl6806_mmio_write(uint32_t a, uint32_t v)
{
    if (a >= SL6806_AUD_BASE && a < SL6806_AUD_BASE + AUD_WORDS * 4) {
        uint32_t off = a - SL6806_AUD_BASE;

        note("A%03X=%08X", off, v);
        if (gated)
            return;
        /*
         * The two CTRL registers carry write-1-to-clear interrupt flags in
         * bits 8 and 10 - which is the whole reason the vendor's handler
         * reads the word and writes it straight back. Model that, or the ack
         * looks like a no-op and the test cannot tell a driver that
         * acknowledges from one that does not.
         */
        if (off == 0x10C || off == 0x20C) {
            if (!addr_ro)
                aud[off / 4] = v & addr_mask;
            return;
        }
        if (off == 0x108 || off == 0x208) {
            uint32_t old = aud[off / 4];
            aud[off / 4] = (v & ~(uint32_t)SL6806_AUD_IRQ_ALL)
                         | (old & SL6806_AUD_IRQ_ALL & ~v);
            return;
        }
        aud[off / 4] = v;
        return;
    }
    if (a >= CRU_BASE && a < CRU_BASE + sizeof(cru)) {
        note("C%03X=%08X", a - CRU_BASE, v);
        cru[(a - CRU_BASE) / 4] = v;
        /* The gate: module 37 is bank 1, bit 5, gate +0x64 shadow +0x74. */
        if (no_ack)
            cru[0x64 / 4] &= ~(1u << 5);
        else if ((cru[0x74 / 4] & (1u << 5)) && (cru[0x64 / 4] & (1u << 5))
                 && !force_gated)
            gated = 0;
        return;
    }
    if (a == SL6806_AUD_PADMUX_REG) { padmux = v; return; }
    if (a == SL6806_AUD_MISC_REG)   { note("M0D8=%08X", 0, v); misc = v; return; }
}

/* The driver delays; nothing to wait for here. */
void delay(uint32_t ms) { (void)ms; }

static void reset_model(void)
{
    memset(aud, 0, sizeof(aud));
    memset(cru, 0, sizeof(cru));
    padmux = misc = 0;
    gated = 1;
    addr_mask = 0xFFFFFFFFu;
    addr_ro = 0;
    trace[0] = '\0';
}

static uint32_t reg(uint32_t off) { return aud[off / 4]; }

/* ------------------------------------------------------------------ */

static void test_rate_split(void)
{
    printf("rate -> master clock\n");

    /* The 8000 family. */
    CHECK(sl6806_audio_mclk_for(48000) == SL6806_AUD_MCLK_48K, "48000");
    CHECK(sl6806_audio_mclk_for(8000)  == SL6806_AUD_MCLK_48K, "8000");
    CHECK(sl6806_audio_mclk_for(16000) == SL6806_AUD_MCLK_48K, "16000");
    CHECK(sl6806_audio_mclk_for(24000) == SL6806_AUD_MCLK_48K, "24000");
    CHECK(sl6806_audio_mclk_for(32000) == SL6806_AUD_MCLK_48K, "32000");
    CHECK(sl6806_audio_mclk_for(96000) == SL6806_AUD_MCLK_48K, "96000");

    /* And everything else. */
    CHECK(sl6806_audio_mclk_for(44100) == SL6806_AUD_MCLK_44K1, "44100");
    CHECK(sl6806_audio_mclk_for(22050) == SL6806_AUD_MCLK_44K1, "22050");
    CHECK(sl6806_audio_mclk_for(11025) == SL6806_AUD_MCLK_44K1, "11025");

    /* Both constants are 512 x their family's base rate, which is the
     * property that identifies them as audio master clocks at all. */
    CHECK(SL6806_AUD_MCLK_48K == 48000u * 512u,
          "24.576 MHz is 512 fs at 48 kHz");
    CHECK(SL6806_AUD_MCLK_44K1 / 512u == 44099u,
          "22.579 MHz is 512 fs at 44.1 kHz, to the vendor's rounding");
}

static void test_pll(void)
{
    printf("the audio PLL\n");

    reset_model();
    cru[0x10 / 4] = 0xFFFFFFFFu;
    cru[0x14 / 4] = 0xFFFFFFFFu;
    sl6806_audio_begin(48000);

    /* The vendor preserves exactly these bits and forces exactly those. */
    CHECK((cru[0x10 / 4] & ~SL6806_AUD_PLL_SEL_KEEP & ~0x3000u
           & ~(SL6806_AUD_PLL_48K_SEL << SL6806_AUD_PLL_SEL_SHIFT)) == 0,
          "+0x10 holds only the preserved bits and the new field");
    CHECK((cru[0x10 / 4] & SL6806_AUD_PLL_SEL_KEEP) == SL6806_AUD_PLL_SEL_KEEP,
          "+0x10 preserves [31:26] and [20:16]");
    CHECK(((cru[0x10 / 4] >> SL6806_AUD_PLL_SEL_SHIFT) & 3u)
              == SL6806_AUD_PLL_48K_SEL,
          "48 kHz selects 3");
    CHECK(((cru[0x14 / 4] >> SL6806_AUD_PLL_RATIO_SHIFT) & 0x1FFFFu)
              == SL6806_AUD_PLL_48K_RATIO,
          "48 kHz ratio is 0x3126");
    CHECK((cru[0x14 / 4] & SL6806_AUD_PLL_RATIO_SET) == SL6806_AUD_PLL_RATIO_SET,
          "+0x14 keeps bit 31 and bit 10");

    reset_model();
    sl6806_audio_begin(44100);
    CHECK(((cru[0x10 / 4] >> SL6806_AUD_PLL_SEL_SHIFT) & 3u)
              == SL6806_AUD_PLL_44K1_SEL,
          "44.1 kHz selects 2");
    CHECK(((cru[0x14 / 4] >> SL6806_AUD_PLL_RATIO_SHIFT) & 0x1FFFFu)
              == SL6806_AUD_PLL_44K1_RATIO,
          "44.1 kHz ratio is 0x186C2");
    CHECK(SL6806_AUD_PLL_44K1_RATIO < (1u << 17),
          "the ratio is 17 bits, so [30:14] - bit 31 is the enable above it");

    /* Bounded, because in payload mode this runs inside the ROM's USB
     * handler. A stuck busy bit must not stop the world. */
    reset_model();
    cru[0x14 / 4] = SL6806_AUD_PLL_BUSY;
    CHECK(sl6806_audio_begin(48000) == 1, "a stuck busy bit does not hang");
}

static void test_bringup(void)
{
    printf("bring-up\n");

    reset_model();
    CHECK(sl6806_audio_begin(48000) == 1, "begin() succeeds when the gate opens");
    CHECK(misc & SL6806_AUD_MISC_BIT, "0x400F70D8 bit 1 is set");
    CHECK(cru[0x74 / 4] & (1u << 5), "module 37's shadow bit");
    CHECK(cru[0x64 / 4] & (1u << 5), "module 37's gate bit");
    CHECK(before("C074", "C064"), "shadow before gate - see sl6806_module.h");
    CHECK(cru[0x88 / 4] & 1u, "romclk 19 is 0x40080088 bit 0");

    CHECK(reg(0x100) & SL6806_AUD_TX_EN_BIT, "+0x100 bit 0");
    CHECK((reg(0x100) & 0xF000u) == 0, "+0x100 [15:12] cleared");
    CHECK(reg(0x138) == SL6806_AUD_VOL_MAX, "TX ch0 at full level");
    CHECK(reg(0x158) == SL6806_AUD_VOL_MAX, "TX ch1 at full level");
    CHECK(reg(0x178) == SL6806_AUD_VOL_MAX, "TX ch2 at full level");
    CHECK(reg(0x3C) & (1u << 10), "+0x3C bit 10 armed");

    CHECK(reg(0x00) & SL6806_AUD_CTRL_EN0, "DAC enable 24");
    CHECK(reg(0x00) & SL6806_AUD_CTRL_EN1, "DAC enable 25");
    CHECK(before("A000=01000000", "A000=03000000"),
          "the two DAC enables are two separate writes, 24 then 25");

    /* A module clock that never acknowledges is the one thing begin() vetoes
     * on: there is no point configuring registers that have no clock. */
    reset_model();
    no_ack = 1;
    CHECK(sl6806_audio_begin(48000) == 0, "begin() reports a dead module gate");
    no_ack = 0;

    /*
     * [M] But a block that acknowledges its clock and still drops writes does
     * NOT veto, since 2026-08-13. begin() used to bail out there, and on
     * hardware that stopped ToneDemo dead over a write test that was itself
     * wrong. Configure and report; the DMA's flag is the evidence.
     */
    reset_model();
    force_gated = 1;
    CHECK(sl6806_audio_begin(48000) == 1,
          "begin() does not veto on a block that drops a scratch write");
    force_gated = 0;
}

static void test_writable(void)
{
    printf("the write test\n");

    /*
     * [M] The first version wrote 0x5A5A5A50 - not a valid SRAM address on
     * this chip - and reported "drops writes" about a block whose thirty
     * registers had just come up at their reset defaults. A write test is
     * only a test if the value is one the register is allowed to keep.
     */
    reset_model();
    sl6806_audio_begin(48000);
    CHECK(sl6806_audio_writable() == 1, "a live block says yes");
    CHECK(reg(0x10C) == 0, "and puts the address register back");

    /* A model of the failure: an address register that masks off everything
     * outside SRAM. The old test failed here; the new one must not. */
    reset_model();
    sl6806_audio_begin(48000);
    addr_mask = 0x00FFFFFFu;
    CHECK(sl6806_audio_writable() == 1,
          "an address register that masks to its memory map still says yes");
    addr_mask = 0xFFFFFFFFu;

    /* And if the address register is write-protected outright, the level
     * field is the second opinion. */
    reset_model();
    sl6806_audio_begin(48000);
    addr_ro = 1;
    CHECK(sl6806_audio_writable() == 1, "the level field is the fallback");
    CHECK(reg(0x138) == SL6806_AUD_VOL_MAX, "and it is restored");
    addr_ro = 0;

    reset_model();
    force_gated = 1;
    CHECK(sl6806_audio_writable() == 0, "a genuinely dead block says no");
    force_gated = 0;
}

static void test_route(void)
{
    printf("the output route\n");

    /*
     * [M] 2026-08-13: a whole buffer went through the DMA with +0x08 reading
     * zero, because nothing wrote it. These four cases are transcribed from
     * 0x00D93F54; getting one of them wrong is a silent wrong output, and
     * routes 1 and 3 produce the SAME +0x08 value and differ only in the
     * +0x14 write, which is exactly the kind of duplicate that gets merged.
     */
    reset_model();
    sl6806_audio_begin(48000);

    sl6806_audio_route(0, 1, 0);
    CHECK((reg(0x08) & 0xFFFFu) == (0x2040u | 3u), "route 1 is 0x2043");
    CHECK((reg(0x14) & 3u) == 0, "and does not touch +0x14");

    reset_model();
    sl6806_audio_begin(48000);
    sl6806_audio_route(0, 3, 0);
    CHECK((reg(0x08) & 0xFFFFu) == (0x2040u | 3u), "route 3 is the same word");
    CHECK((reg(0x14) & 3u) == 3u, "but route 3 also sets +0x14 bits 0 and 1");

    reset_model();
    sl6806_audio_begin(48000);
    aud[0x08 / 4] = 0x2000u;            /* something for the bic to clear */
    sl6806_audio_route(0, 2, 0);
    CHECK((reg(0x08) & 0xFFFFu) == 2u, "route 2 clears 0x2000 and sets 2");
    CHECK((reg(0x14) & 3u) == 3u, "and sets +0x14");

    reset_model();
    sl6806_audio_begin(48000);
    aud[0x08 / 4] = 0x41u;
    sl6806_audio_route(0, 0, 0);
    CHECK((reg(0x08) & 0xFFFFu) == (0x2080u | 3u), "route 0 clears 0x41");
    CHECK((reg(0x80) & 0x7000u) == 0, "and clears 0x40009080 [14:12]");

    /* Channel 1 is the same constants shifted 16 - the property that made the
     * two-half reading of this register certain in the first place. */
    reset_model();
    sl6806_audio_begin(48000);
    sl6806_audio_route(1, 1, 0);
    CHECK((reg(0x08) >> 16) == (0x2040u | 3u), "channel 1 is the top half");
    CHECK((reg(0x08) & 0xFFFFu) == 0, "and leaves channel 0 alone");

    /* The trim is a separate read-modify-write after the vendor's delay, not
     * folded into the word above. */
    reset_model();
    sl6806_audio_begin(48000);
    sl6806_audio_route(0, 1, 0x15);
    CHECK(((reg(0x08) >> 8) & 0x1Fu) == 0x15u, "the trim lands at [12:8]");
    sl6806_audio_route(1, 1, 0x0A);
    CHECK(((reg(0x08) >> 24) & 0x1Fu) == 0x0Au, "and at [28:24] for channel 1");

    /* A route change re-asserts both DAC enables, which is what the vendor's
     * own function does at its end. */
    reset_model();
    sl6806_audio_begin(48000);
    aud[0x00 / 4] = 0;
    sl6806_audio_route(0, 1, 0);
    CHECK(reg(0x00) & SL6806_AUD_CTRL_EN0, "route re-asserts DAC enable 24");
    CHECK(reg(0x00) & SL6806_AUD_CTRL_EN1, "and 25");

    sl6806_audio_route(2, 1, 0);
    CHECK(SL6806_AUD_ROUTES == 4, "there are four routes");
}

static void test_submit(void)
{
    static uint8_t buf[256] __attribute__((aligned(4)));

    printf("buffer submit\n");

    reset_model();
    sl6806_audio_begin(48000);
    trace[0] = '\0';

    CHECK(sl6806_audio_play(buf, sizeof(buf)) == 0, "a valid buffer is taken");
    CHECK(reg(0x10C) == (uint32_t)(uintptr_t)buf, "+0x10C is the address");
    CHECK((reg(0x108) >> SL6806_AUD_LEN_SHIFT) == sizeof(buf),
          "+0x108 [31:16] is the length");
    CHECK(reg(0x104) == (((uint32_t)sizeof(buf) * 3u / 4u) << 16 | 1u),
          "+0x104 is the three-quarter watermark with the interrupt armed");
    CHECK(reg(0x108) & SL6806_AUD_START, "and the start bit is set");

    CHECK(before("A10C", "A108"), "address before length");
    CHECK(before("A108", "A104"), "length before trigger");
    CHECK(strstr(trace, "A104") < strstr(strstr(trace, "A104"), "A108"),
          "and the start bit comes after the trigger, as its own write");

    /* The rejections. The vendor refuses rather than rounds; so must this,
     * because a rounded descriptor is one the hardware was never given. */
    CHECK(sl6806_audio_play(buf, 6) == -1, "a length that is not a multiple of 4");
    CHECK(sl6806_audio_play(buf + 1, 8) == -1, "an unaligned buffer");
    CHECK(sl6806_audio_play(NULL, 8) == -1, "a null buffer");
    CHECK(sl6806_audio_play(buf, 0) == -1, "a zero length");
    CHECK(sl6806_audio_play(buf, 0x10000) == -1,
          "a length that will not fit the 16-bit field");

    CHECK(sl6806_audio_trigger_word(0x1000) == ((0xC00u << 16) | 1u),
          "the trigger word is three quarters, shifted, armed");
}

static void test_flags(void)
{
    printf("interrupt flags\n");

    reset_model();
    sl6806_audio_begin(48000);

    aud[0x108 / 4] = SL6806_AUD_IRQ_DONE | SL6806_AUD_START;
    CHECK(sl6806_audio_done() == 1, "the completion flag is seen");
    CHECK((aud[0x108 / 4] & SL6806_AUD_IRQ_DONE) == 0,
          "and cleared by writing the word back - the handler's own ack");
    CHECK(aud[0x108 / 4] & SL6806_AUD_START,
          "without disturbing the start bit");
    CHECK(sl6806_audio_done() == 0, "a second read does not see it again");

    aud[0x108 / 4] |= SL6806_AUD_IRQ_WATER;
    CHECK(sl6806_audio_flags() == SL6806_AUD_IRQ_WATER,
          "the watermark flag is reported separately");
    CHECK(sl6806_audio_done() == 0, "and is not mistaken for completion");

    sl6806_audio_stop();
    CHECK((aud[0x108 / 4] & SL6806_AUD_START) == 0, "stop clears the start bit");
}

static void test_volume(void)
{
    printf("volume\n");

    reset_model();
    sl6806_audio_begin(48000);

    sl6806_audio_volume(1, 100);
    CHECK(reg(0x158) == 100, "channel 1's level");
    CHECK(reg(0x138) == SL6806_AUD_VOL_MAX, "channel 0 untouched");

    sl6806_audio_volume(1, 0xFFFF);
    CHECK(reg(0x158) == SL6806_AUD_VOL_MAX, "clamped to 9 bits");

    /* The level is 9 bits inside a register whose other bits belong to
     * something nobody has decoded. A whole-word write would take them. */
    aud[0x158 / 4] = 0xABCD0000u | 0x1FFu;
    sl6806_audio_volume(1, 1);
    CHECK(reg(0x158) == (0xABCD0000u | 1u), "the rest of the word survives");

    sl6806_audio_mute(2, 1);
    CHECK(reg(0x174) & SL6806_AUD_VOL_MUTE, "mute is bit 16 of the CTRL half");
    sl6806_audio_mute(2, 0);
    CHECK((reg(0x174) & SL6806_AUD_VOL_MUTE) == 0, "and clears");

    sl6806_audio_volume(SL6806_AUD_TX_CHANNELS, 1);
    sl6806_audio_mute(SL6806_AUD_TX_CHANNELS, 1);
    CHECK(reg(0x194) == 0 && reg(0x198) == 0,
          "a channel past the end writes nothing");
}

static void test_map(void)
{
    printf("the register map\n");

    /* The strides are the whole reason these are macros, and they differ
     * between the two directions - 0x20 for playback, 0x30 for capture -
     * which is the sort of thing that gets "unified" by accident. */
    CHECK(SL6806_AUD_TX_VOL(0) == 0x40009138u, "TX ch0 level");
    CHECK(SL6806_AUD_TX_VOL(2) == 0x40009178u, "TX ch2 level, 0x20 stride");
    CHECK(SL6806_AUD_RX_VOL(0) == 0x40009228u, "RX ch0 level");
    CHECK(SL6806_AUD_RX_VOL(3) == 0x400092B8u, "RX ch3 level, 0x30 stride");
    CHECK(SL6806_AUD_TX_VOL_CTRL(1) == 0x40009154u, "TX ch1 mute");
    CHECK(SL6806_AUD_RX_VOL_CTRL(2) == 0x40009284u, "RX ch2 mute");

    /* The two directions are one page apart, which is what made 7c read the
     * block as a two-channel timer. */
    CHECK(SL6806_AUD_RX_CTRL - SL6806_AUD_TX_CTRL == 0x100,
          "capture is playback plus 0x100");
}

int main(void)
{
    printf("audio controller tests\n");

    test_rate_split();
    test_pll();
    test_bringup();
    test_writable();
    test_route();
    test_submit();
    test_flags();
    test_volume();
    test_map();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
