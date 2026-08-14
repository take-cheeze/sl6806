/*
 * sl6806_dvp.c - the +0x30..0x3C DMA cluster on the camera front end.
 *
 * Everything else in sl6806_dvp.h is a map: register offsets recovered from
 * the vendor's own configure/open routines. This cluster is different - the
 * header's own words are "nothing has been run" - because no vendor routine
 * that has been read ever writes these four registers. DvpProbe found them
 * by writing every bit of the live block and reading back what stuck, not by
 * reading code.
 *
 * So there is no vendor sequence to transcribe, and this file does not
 * pretend otherwise. The order below (address, then length, then start) is
 * this driver's own choice, matching the convention every other DMA-shaped
 * block in this tree uses. It has never moved a pixel. What it can do, and
 * what sl6806_dvp_capture_writable() is for, is the same thing
 * sl6806_audio_writable() does for the audio DMA: prove the descriptor
 * registers hold what they are given, which is a real and useful fact even
 * before a single byte is known to move.
 */

#include <stddef.h>

#include "sl6806_dvp.h"
#include "sl6806_mmio.h"

/* A test pattern for sl6806_dvp_capture_writable(): in range, word-aligned,
 * and nothing a cold reset or a plausible previous descriptor would produce
 * by accident. */
#define DVP_TEST_ADDR_OFS  0x00012340u
#define DVP_TEST_LEN       0x00034560u

int sl6806_dvp_capture_start(void *dst, uint32_t len)
{
    uint32_t addr = (uint32_t)(uintptr_t)dst;

    if (dst == NULL)
        return -1;
    if (addr < SL6806_DVP_SRAM_BASE ||
        addr >= SL6806_DVP_SRAM_BASE + SL6806_DVP_SRAM_SPAN)
        return -1;
    if (addr & 3u)
        return -1;
    if (len == 0 || (len & 3u) || len > SL6806_DVP_DMA_LEN_MASK)
        return -1;

    sl6806_mmio_write(SL6806_DVP_BASE + SL6806_DVP_DMA_ADDR,
                      SL6806_DVP_SRAM_BASE
                          | (addr & SL6806_DVP_DMA_ADDR_MASK));
    sl6806_mmio_write(SL6806_DVP_BASE + SL6806_DVP_DMA_LEN20,
                      len & SL6806_DVP_DMA_LEN_MASK);
    sl6806_mmio_write(SL6806_DVP_BASE + SL6806_DVP_DMA_CTRL,
                      SL6806_DVP_DMA_CTRL_START);

    return 0;
}

void sl6806_dvp_capture_stop(void)
{
    sl6806_mmio_write(SL6806_DVP_BASE + SL6806_DVP_DMA_CTRL, 0);
}

int sl6806_dvp_capture_writable(void)
{
    uint32_t old_addr, old_len, test_addr, ok;

    old_addr = sl6806_mmio_read(SL6806_DVP_BASE + SL6806_DVP_DMA_ADDR);
    old_len  = sl6806_mmio_read(SL6806_DVP_BASE + SL6806_DVP_DMA_LEN20);

    test_addr = SL6806_DVP_SRAM_BASE
              | (DVP_TEST_ADDR_OFS & SL6806_DVP_DMA_ADDR_MASK);

    sl6806_mmio_write(SL6806_DVP_BASE + SL6806_DVP_DMA_ADDR, test_addr);
    sl6806_mmio_write(SL6806_DVP_BASE + SL6806_DVP_DMA_LEN20,
                      DVP_TEST_LEN & SL6806_DVP_DMA_LEN_MASK);

    ok = (sl6806_mmio_read(SL6806_DVP_BASE + SL6806_DVP_DMA_ADDR)
              == test_addr)
      && (sl6806_mmio_read(SL6806_DVP_BASE + SL6806_DVP_DMA_LEN20)
              == (DVP_TEST_LEN & SL6806_DVP_DMA_LEN_MASK));

    sl6806_mmio_write(SL6806_DVP_BASE + SL6806_DVP_DMA_ADDR, old_addr);
    sl6806_mmio_write(SL6806_DVP_BASE + SL6806_DVP_DMA_LEN20, old_len);

    return ok ? 1 : 0;
}

void *sl6806_dvp_capture_addr(void)
{
    return (void *)(uintptr_t)
        sl6806_mmio_read(SL6806_DVP_BASE + SL6806_DVP_DMA_ADDR);
}

uint32_t sl6806_dvp_capture_len(void)
{
    return sl6806_mmio_read(SL6806_DVP_BASE + SL6806_DVP_DMA_LEN20)
         & SL6806_DVP_DMA_LEN_MASK;
}
