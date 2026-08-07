/*
 * sl6806_adc.c - ADC driver. See sl6806_adc.h for where every constant came
 * from and which of them were measured rather than read.
 */

#include "sl6806_adc.h"

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
