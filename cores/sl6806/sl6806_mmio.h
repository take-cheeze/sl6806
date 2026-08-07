/*
 * sl6806_mmio.h - the single door every peripheral access goes through.
 *
 * There is exactly one reason this exists rather than each driver casting to
 * volatile uint32_t * itself: the host tests. A driver written against these
 * two calls can be compiled natively and run against a model of the
 * peripheral, which is the only way to check a register sequence when the
 * peripheral has no datasheet and one unit to test on.
 *
 * tests/host/test_lcdc.c defines SL6806_MMIO_HOOKED and supplies its own
 * pair. On the device the definitions below compile to the same load and
 * store you would have written by hand.
 */
#ifndef SL6806_MMIO_H
#define SL6806_MMIO_H

#include <stdint.h>

#ifdef SL6806_MMIO_HOOKED

/* The test supplies these. */
uint32_t sl6806_mmio_read(uint32_t addr);
void     sl6806_mmio_write(uint32_t addr, uint32_t value);

#else

static inline uint32_t sl6806_mmio_read(uint32_t addr)
{
    return *(volatile uint32_t *)(uintptr_t)addr;
}

static inline void sl6806_mmio_write(uint32_t addr, uint32_t value)
{
    *(volatile uint32_t *)(uintptr_t)addr = value;
}

#endif /* SL6806_MMIO_HOOKED */

/* Read, replace an n-bit field at `shift`, write back. This is the shape
 * every vendor register write on this chip has - a BFI followed by a store -
 * and doing it any other way (accumulating fields and storing once) would be
 * a different sequence on the bus, which is not a change worth making
 * blindly on a peripheral nobody has documented. */
static inline void sl6806_mmio_field(uint32_t addr, unsigned shift,
                                     unsigned width, uint32_t value)
{
    uint32_t mask = (width >= 32) ? 0xFFFFFFFFu : (((1u << width) - 1u) << shift);
    uint32_t v = sl6806_mmio_read(addr);

    v = (v & ~mask) | ((value << shift) & mask);
    sl6806_mmio_write(addr, v);
}

static inline void sl6806_mmio_set(uint32_t addr, uint32_t bits)
{
    sl6806_mmio_write(addr, sl6806_mmio_read(addr) | bits);
}

static inline void sl6806_mmio_clr(uint32_t addr, uint32_t bits)
{
    sl6806_mmio_write(addr, sl6806_mmio_read(addr) & ~bits);
}

#endif /* SL6806_MMIO_H */
