/*
 * test_dvp.c - native tests for the DVP DMA cluster driver.
 *
 * Like test_audio.c and test_regfile.c, this is a different kind of thing
 * from most tests next to it: sl6806_dvp_capture_start() has never run on
 * hardware, and unlike the audio and regfile drivers, there is no vendor
 * sequence behind it to transcribe - +0x30..0x3C are never written by any
 * routine that has been read. What these tests hold the driver to is its
 * own stated contract:
 *
 *   1. **The rejections.** An out-of-range, misaligned or oversized request
 *      must be refused before it touches a register - handing the hardware
 *      a half-written descriptor is worse than refusing outright.
 *   2. **The write order.** Address, then length, then start, as its own
 *      write - not folded into one store, matching every other DMA-shaped
 *      block in this tree.
 *   3. **The masks match the census.** SL6806_DVP_DMA_ADDR_MASK and
 *      SL6806_DVP_DMA_LEN_MASK are transcribed from DvpProbe's measured
 *      writable bits, not guessed, and a change to either is a change to
 *      what the hardware was actually seen to accept.
 *   4. **The gate matters.** A gated block reads zero and drops writes,
 *      exactly like the audio and ADC blocks - sl6806_dvp_capture_writable()
 *      must say so rather than reporting a plausible-looking read.
 */

#include <stdio.h>
#include <string.h>

#include "sl6806_dvp.h"

static int failures, checks;

#define CHECK(cond, what) do {                                          \
        checks++;                                                       \
        if (!(cond)) {                                                  \
            printf("  FAIL %s (%s:%d)\n", (what), __FILE__, __LINE__);  \
            failures++;                                                 \
        }                                                               \
    } while (0)

/* ------------------------------------------------------------------ */
/* A model of just the +0x30..0x3C cluster, and its gate                */
/* ------------------------------------------------------------------ */

static uint32_t ctrl, len24, addr, len20;
static int gated;
static char trace[512];

static void note(const char *tag)
{
    if (strlen(trace) + strlen(tag) + 2 < sizeof(trace)) {
        strcat(trace, tag);
        strcat(trace, " ");
    }
}

static int before(const char *a, const char *b)
{
    const char *pa = strstr(trace, a);
    const char *pb = strstr(trace, b);

    return pa && pb && pa < pb;
}

uint32_t sl6806_mmio_read(uint32_t a)
{
    if (gated)
        return 0;
    switch (a - SL6806_DVP_BASE) {
    case SL6806_DVP_DMA_CTRL:  return ctrl;
    case SL6806_DVP_DMA_LEN24: return len24;
    case SL6806_DVP_DMA_ADDR:  return addr;
    case SL6806_DVP_DMA_LEN20: return len20;
    default: return 0;
    }
}

void sl6806_mmio_write(uint32_t a, uint32_t v)
{
    switch (a - SL6806_DVP_BASE) {
    case SL6806_DVP_DMA_CTRL:  note("CTRL");  if (!gated) ctrl  = v & SL6806_DVP_DMA_CTRL_MASK; return;
    case SL6806_DVP_DMA_LEN24: note("LEN24"); if (!gated) len24 = v; return;
    case SL6806_DVP_DMA_ADDR:  note("ADDR");  if (!gated) addr  = v; return;
    case SL6806_DVP_DMA_LEN20: note("LEN20"); if (!gated) len20 = v; return;
    default: return;
    }
}

static void reset_model(void)
{
    ctrl = len24 = addr = len20 = 0;
    addr = SL6806_DVP_SRAM_BASE;   /* the block's own cold reset value */
    gated = 0;
    trace[0] = '\0';
}

/* ------------------------------------------------------------------ */

static void test_rejections(void)
{
    /* In-range, so the length checks below exercise the length field and
     * not the address range check. */
    void *buf = (void *)(uintptr_t)(SL6806_DVP_SRAM_BASE + 0x1000u);
    void *sram_lo = (void *)(uintptr_t)SL6806_DVP_SRAM_BASE;
    void *sram_hi = (void *)(uintptr_t)
        (SL6806_DVP_SRAM_BASE + SL6806_DVP_SRAM_SPAN - 4);
    void *outside = (void *)(uintptr_t)
        (SL6806_DVP_SRAM_BASE + SL6806_DVP_SRAM_SPAN);

    printf("rejections\n");

    reset_model();
    CHECK(sl6806_dvp_capture_start(NULL, 64) == -1, "a null buffer");
    CHECK(trace[0] == '\0', "and touches no register");

    reset_model();
    CHECK(sl6806_dvp_capture_start((void *)(uintptr_t)0x00700000u, 64) == -1,
          "an address below the SRAM base");

    reset_model();
    CHECK(sl6806_dvp_capture_start(outside, 64) == -1,
          "an address at the top of the 1 MB span");

    reset_model();
    CHECK(sl6806_dvp_capture_start((void *)(uintptr_t)
              (SL6806_DVP_SRAM_BASE + 1), 64) == -1,
          "an address that is not word-aligned");

    reset_model();
    CHECK(sl6806_dvp_capture_start(buf, 0) == -1, "a zero length");
    CHECK(sl6806_dvp_capture_start(buf, 6) == -1,
          "a length that is not a multiple of 4");
    CHECK(sl6806_dvp_capture_start(buf, SL6806_DVP_DMA_LEN_MASK + 4) == -1,
          "a length past the 20-bit field");

    /* The two edges of the valid range, both accepted. */
    reset_model();
    CHECK(sl6806_dvp_capture_start(sram_lo, 4) == 0,
          "the first word of SRAM, minimum length");
    reset_model();
    CHECK(sl6806_dvp_capture_start(sram_hi, 4) == 0,
          "the last word of the 1 MB span");
    reset_model();
    CHECK(sl6806_dvp_capture_start(sram_lo, SL6806_DVP_DMA_LEN_MASK & ~3u)
              == 0,
          "the largest length that fits the field and is a multiple of 4");
}

static void test_descriptor(void)
{
    /*
     * A payload's SRAM window is a small slice of the real 0x00800000..
     * 0x008FFFFF span (see startup_payload.c), so an ordinary host buffer
     * has no address in that range. What is under test is the register
     * arithmetic, not a real transfer, so a synthetic in-range pointer
     * exercises it exactly as a real payload buffer would.
     */
    void *buf = (void *)(uintptr_t)(SL6806_DVP_SRAM_BASE + 0x00040000u);
    const uint32_t len = 256;

    printf("the descriptor\n");

    reset_model();
    CHECK(sl6806_dvp_capture_start(buf, len) == 0, "a valid request is taken");
    CHECK(addr == (SL6806_DVP_SRAM_BASE
                    | ((uint32_t)(uintptr_t)buf & SL6806_DVP_DMA_ADDR_MASK)),
          "ADDR is the buffer, masked to the writable field, base preserved");
    CHECK(len20 == len, "LEN20 is the byte count");
    CHECK(ctrl == SL6806_DVP_DMA_CTRL_MASK,
          "CTRL gets every writable bit - there is no vendor sequence "
          "narrowing this");
    CHECK(len24 == 0,
          "LEN24 is left alone - it may be a second address, not a length");

    CHECK(before("ADDR", "LEN20"), "address before length");
    CHECK(before("LEN20", "CTRL"), "length before start");

    CHECK(sl6806_dvp_capture_addr() == (void *)(uintptr_t)addr,
          "capture_addr() reads back the raw descriptor");
    CHECK(sl6806_dvp_capture_len() == len,
          "capture_len() reads back the byte count");

    sl6806_dvp_capture_stop();
    CHECK(ctrl == 0, "capture_stop() clears CTRL");
    CHECK(addr != 0 && len20 == len,
          "and leaves the address and length registers alone");
}

static void test_writable(void)
{
    printf("the write-and-read-back proof\n");

    reset_model();
    CHECK(sl6806_dvp_capture_writable() == 1,
          "a live block holds a distinguishing test pattern");
    CHECK(addr == SL6806_DVP_SRAM_BASE && len20 == 0,
          "and restores the cold reset state it found");

    reset_model();
    sl6806_dvp_capture_start((void *)(uintptr_t)(SL6806_DVP_SRAM_BASE + 0x100),
                             128);
    CHECK(sl6806_dvp_capture_writable() == 1,
          "and restores an in-progress descriptor, not just the cold state");
    CHECK(addr == (SL6806_DVP_SRAM_BASE + 0x100) && len20 == 128,
          "exactly as sl6806_dvp_capture_start() left it");

    reset_model();
    gated = 1;
    CHECK(sl6806_dvp_capture_writable() == 0,
          "a gated block that drops writes says no");
}

/*
 * The bit-at-a-time trigger and the CTRL reader, both added after the first
 * hardware run. That run showed the descriptor holds what it is given and
 * the buffer never changes, which on its own cannot say whether the engine
 * ever started - so these two exist to ask the register instead of the
 * buffer. What is testable here is the guard and the plumbing; what the
 * hardware does with the write is a bench question.
 */
static void test_ctrl_bits(void)
{
    void *buf = (void *)(uintptr_t)(SL6806_DVP_SRAM_BASE + 0x00040000u);
    const uint32_t len = 256;

    printf("the per-bit trigger, and CTRL read back\n");

    reset_model();
    CHECK(sl6806_dvp_capture_start_bits(buf, len, 1u << 1) == 0,
          "a single writable CTRL bit is a valid trigger");
    CHECK(ctrl == (1u << 1), "and only that bit reaches the register");
    CHECK(len20 == len && addr != 0,
          "the descriptor is still programmed the same way");

    reset_model();
    CHECK(sl6806_dvp_capture_start_bits(buf, len, 0) == -1,
          "a zero trigger is refused - it would arm nothing");
    CHECK(ctrl == 0 && addr == SL6806_DVP_SRAM_BASE && len20 == 0,
          "and is refused before any register is touched");

    reset_model();
    CHECK(sl6806_dvp_capture_start_bits(buf, len, 1u << 2) == -1,
          "a bit the census measured as not sticking is refused");
    CHECK(sl6806_dvp_capture_start_bits(buf, len, 0x10u) == -1,
          "so is anything above the writable mask");
    CHECK(addr == SL6806_DVP_SRAM_BASE && len20 == 0,
          "both refused before the descriptor is written");

    /* The rejections must hold for the bit form too, or the guard could be
     * bypassed by choosing a trigger. */
    CHECK(sl6806_dvp_capture_start_bits(NULL, len, 1u) == -1,
          "null is refused whichever trigger is asked for");
    CHECK(sl6806_dvp_capture_start_bits((void *)0x10000000u, len, 1u) == -1,
          "so is an address outside the SRAM window");

    reset_model();
    CHECK(sl6806_dvp_capture_start(buf, len) == 0,
          "capture_start() still works");
    CHECK(ctrl == SL6806_DVP_DMA_CTRL_MASK,
          "and is exactly the all-bits case of the bit form");

    /* Unmasked on purpose: a status bit the hardware sets on its own is the
     * one thing this cluster could use to report progress, and masking the
     * accessor to the writable bits would hide it. */
    reset_model();
    ctrl = 0x80u;
    CHECK(sl6806_dvp_capture_ctrl() == 0x80u,
          "capture_ctrl() reports bits outside the writable mask");
    gated = 1;
    CHECK(sl6806_dvp_capture_ctrl() == 0,
          "and a gated block reads zero, like every other block here");
}

static void test_masks(void)
{
    printf("the field masks, against DvpProbe's census\n");

    /* [M] 2026-08-13, from the header: "+0x38 ... writable 0x000FFFFC" and
     * "+0x3C cold 0 writable 0x000FFFFF". Restating them here as constants
     * is what makes a future edit to either one a test failure instead of a
     * silent divergence from the one hardware measurement this cluster has. */
    CHECK(SL6806_DVP_DMA_ADDR_MASK == 0x000FFFFCu,
          "ADDR's writable bits are [19:2], measured");
    CHECK(SL6806_DVP_DMA_LEN_MASK == 0x000FFFFFu,
          "LEN20's writable bits are the full 20, measured");
    CHECK(SL6806_DVP_DMA_CTRL_MASK == 0x0Bu,
          "CTRL's writable bits are 0, 1 and 3, measured");
    CHECK(SL6806_DVP_SRAM_SPAN == (SL6806_DVP_DMA_ADDR_MASK | 3u) + 1u,
          "the address mask spans exactly the advertised 1 MB");
}

int main(void)
{
    printf("DVP DMA cluster tests\n");

    test_rejections();
    test_descriptor();
    test_writable();
    test_ctrl_bits();
    test_masks();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
