/*
 * sl6806_adc.c - ADC driver. See sl6806_adc.h for where every constant came
 * from and which of them were measured rather than read.
 */

#include "sl6806_adc.h"

#define CRU_BASE        0x40080000u
#define MOD_BASE_HI     0x400F1000u

/*
 * 0x00001C5C, transcribed with a bounded poll.
 *
 * The poll is deliberately short. An acknowledgement is a few cycles of
 * hardware; a long one only costs time when the id is one nothing implements,
 * and a walk pays that for every miss. The first version of this used 100000
 * iterations, spent over a second inside the boot ROM's USB handler while
 * walking, and took the device off the bus.
 */
int sl6806_module_enable(unsigned id)
{
    uint32_t base, gate, shadow, bit;
    unsigned i;

    if (id >= 128)
        return 0;

    if (id < 32)        { base = CRU_BASE;    gate = 0x60; shadow = 0x70; }
    else if (id < 64)   { base = CRU_BASE;    gate = 0x64; shadow = 0x74; id -= 32; }
    else if (id < 96)   { base = CRU_BASE;    gate = 0x68; shadow = 0x78; id -= 64; }
    else                { base = MOD_BASE_HI; gate = 0x20; shadow = 0x30; id -= 96; }

    bit = 1u << id;

    /* Order is the operation: shadow, then gate, then wait for the gate to
     * read the bit back. Setting both at once does nothing at all. */
    sl6806_mmio_write(base + shadow, sl6806_mmio_read(base + shadow) | bit);
    sl6806_mmio_write(base + gate,   sl6806_mmio_read(base + gate)   | bit);

    for (i = 0; i < 200u; i++)
        if (sl6806_mmio_read(base + gate) & bit)
            return 1;
    return 0;
}

int sl6806_adc_writable(void)
{
    uint32_t reg = SL6806_ADC_BASE + SL6806_ADC_CFG;
    uint32_t saved = sl6806_mmio_read(reg);
    int ok;

    sl6806_mmio_write(reg, SL6806_ADC_INIT_CFG);
    ok = (sl6806_mmio_read(reg) == SL6806_ADC_INIT_CFG);
    sl6806_mmio_write(reg, saved);
    return ok;
}

int sl6806_adc_begin(void)
{
    sl6806_module_enable(SL6806_ADC_MODULE_ID);

    if (!sl6806_adc_writable())
        return 0;

    /* 0x00D994EC. Note this leaves every channel disabled. */
    sl6806_mmio_write(SL6806_ADC_BASE + SL6806_ADC_CFG10, 0);
    sl6806_mmio_write(SL6806_ADC_BASE + SL6806_ADC_CFG18, 0);
    sl6806_mmio_write(SL6806_ADC_BASE + SL6806_ADC_IRQ,   0);
    sl6806_mmio_write(SL6806_ADC_BASE + SL6806_ADC_CFG,   SL6806_ADC_INIT_CFG);
    sl6806_mmio_write(SL6806_ADC_BASE + SL6806_ADC_CTRL,  SL6806_ADC_INIT_CTRL);
    return 1;
}

void sl6806_adc_channel(unsigned ch, int on)
{
    uint32_t ctrl, irq, bit;

    if (ch >= SL6806_ADC_CHANNELS)
        return;

    bit  = 1u << ch;
    ctrl = sl6806_mmio_read(SL6806_ADC_BASE + SL6806_ADC_CTRL);
    irq  = sl6806_mmio_read(SL6806_ADC_BASE + SL6806_ADC_IRQ);

    if (on) {
        ctrl |= bit;
        irq  |= bit;
    } else {
        ctrl &= ~bit;
        irq  &= ~bit;
    }
    sl6806_mmio_write(SL6806_ADC_BASE + SL6806_ADC_CTRL, ctrl);
    sl6806_mmio_write(SL6806_ADC_BASE + SL6806_ADC_IRQ,  irq);
}

uint32_t sl6806_adc_read(unsigned ch)
{
    if (ch >= SL6806_ADC_CHANNELS)
        return 0;
    return sl6806_mmio_read(SL6806_ADC_BASE + SL6806_ADC_RESULT(ch));
}
