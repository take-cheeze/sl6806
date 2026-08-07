/*
 * sl6806_padctl.c - pad controller driver, reimplemented from the mask ROM.
 *
 * Every function here is a transcription of a mask ROM routine; the address
 * of the original is on each one. Reimplementing rather than calling the ROM
 * costs a few hundred bytes and buys two things: it works in firmware mode,
 * where the ROM is no longer the running program, and it can be tested on
 * the host against a model.
 *
 * See sl6806_padctl.h for the register map and the pin id encoding.
 */

#include "sl6806_padctl.h"
#include "sl6806_mmio.h"
#include "hal_gpio.h"

/* [V] ROM 0x00065004. */
const uint32_t sl6806_pad_bank_base[SL6806_PAD_NBANKS] = {
    0x40081000u, 0x40081040u, 0x400F6080u,
    0x400810C0u, 0x40081100u, 0x400F6000u,
};

/*
 * [V] ROM 0x0006501C. Indexed by (id & 0xF) - 4; a selector outside 4..15
 * means "no pull" and uses 0x80000000. Only two 2-bit fields of each entry
 * are ever used - bits [1:0] and bits [17:16] - which is why the table looks
 * so sparse. Bit 31 is set in every entry and is not read by anything.
 */
static const uint32_t pull_table[12] = {
    0x80020001u, 0x80030001u, 0x80000000u, 0x80000000u,
    0x80000002u, 0x80000000u, 0x80000000u, 0x80020002u,
    0x80000003u, 0x80010003u, 0x80020003u, 0x80030003u,
};

#define PAD_NO_PULL 0x80000000u

/* Register offsets, from sl6806_padctl.h's map. */
#define PAD_FUNC_BASE   0x000   /* + (pin>>3)*4, 4 bits per pin */
#define PAD_IN          0x010
#define PAD_DRIVE_BASE  0x014   /* + (pin>>4)*4, 2 bits per pin */
#define PAD_PULLA_BASE  0x024   /* + (pin>>4)*4, 2 bits per pin */
#define PAD_PULLB_BASE  0x02C   /* + (pin>>4)*4, 2 bits per pin */
#define PAD_SET         0x034
#define PAD_CLR         0x038

/* Resolve a pad id to its bank base, or 0 if the bank does not exist. */
static uint32_t bank_of(uint32_t id)
{
    unsigned bank = SL6806_PAD_BANK(id);

    if (bank >= SL6806_PAD_NBANKS)
        return 0;
    return sl6806_pad_bank_base[bank];
}

/* [V] ROM 0x6F0. */
static void pad_func(uint32_t base, unsigned pin, unsigned func)
{
    sl6806_mmio_field(base + PAD_FUNC_BASE + (pin >> 3) * 4,
                      (pin & 7) * 4, 4, func);
}

/* [V] ROM 0x6DE. */
static void pad_level(uint32_t base, unsigned pin, unsigned level)
{
    sl6806_mmio_write(base + (level ? PAD_SET : PAD_CLR), 1u << pin);
}

/* [V] ROM 0x76A. */
static void pad_drive(uint32_t base, unsigned pin, unsigned drive)
{
    sl6806_mmio_field(base + PAD_DRIVE_BASE + (pin >> 4) * 4,
                      (pin & 15) * 2, 2, drive);
}

/* [V] ROM 0x736. Two 2-bit fields taken from one word of the pull table. */
static void pad_pull(uint32_t base, unsigned pin, uint32_t pull)
{
    sl6806_mmio_field(base + PAD_PULLA_BASE + (pin >> 4) * 4,
                      (pin & 15) * 2, 2, pull & 3u);
    sl6806_mmio_field(base + PAD_PULLB_BASE + (pin >> 4) * 4,
                      (pin & 15) * 2, 2, (pull >> 16) & 3u);
}

/* [V] ROM 0x93C, which is ROM 0x878 applied to a decoded id. The order
 * matters and is the ROM's: function, then level, then drive, then pull. */
int sl6806_pad_configure(uint32_t id)
{
    uint32_t base = bank_of(id);
    unsigned pin  = SL6806_PAD_PIN(id);
    unsigned sel  = SL6806_PAD_PULL(id);
    uint32_t pull = (sel >= 4u && sel <= 15u) ? pull_table[sel - 4u]
                                              : PAD_NO_PULL;

    if (!base)
        return -1;

    pad_func(base, pin, SL6806_PAD_FUNC(id));
    pad_level(base, pin, SL6806_PAD_LEVEL(id));
    pad_drive(base, pin, SL6806_PAD_DRIVE(id));
    pad_pull(base, pin, pull);
    return 0;
}

/* [V] ROM 0xAAE. */
int sl6806_pad_set_func(uint32_t id, unsigned func)
{
    uint32_t base = bank_of(id);

    if (!base)
        return -1;
    pad_func(base, SL6806_PAD_PIN(id), func & 0x0Fu);
    return 0;
}

/* [V] ROM 0x99E. */
int sl6806_pad_write(uint32_t id, unsigned level)
{
    uint32_t base = bank_of(id);

    if (!base)
        return -1;
    pad_level(base, SL6806_PAD_PIN(id), level ? 1u : 0u);
    return 0;
}

/* [V] ROM 0x97C. */
int sl6806_pad_read(uint32_t id)
{
    uint32_t base = bank_of(id);

    if (!base)
        return -1;
    return (int)((sl6806_mmio_read(base + PAD_IN) >> SL6806_PAD_PIN(id)) & 1u);
}

/* ------------------------------------------- hal_gpio.h vendor back end */

static void vendor_write(uint32_t id, uint32_t value)
{
    sl6806_pad_write(id, value ? 1u : 0u);
}

static uint32_t vendor_read(uint32_t id)
{
    int v = sl6806_pad_read(id);

    return (v > 0) ? 1u : 0u;
}

/*
 * pinMode(). Direction on this chip is the function nibble, so unlike the
 * old vendor back end - which had to leave pads alone because it did not
 * know which field was which - this one can set it.
 *
 * It sets only the function. Drive and pull stay as whatever configured the
 * pad, because an Arduino INPUT/OUTPUT carries no opinion about either and
 * overwriting them would quietly undo a variant's pad setup.
 */
static void vendor_set_dir(uint32_t id, int output)
{
    sl6806_pad_set_func(id, output ? SL6806_PAD_FUNC_OUTPUT
                                   : SL6806_PAD_FUNC_INPUT);
}

/*
 * The vendor's gpio_config(id, value) passes a configuration word next to
 * the id. Here the two are the same encoding, so this takes the bank and pin
 * from `id` and every configuration field from `value` - which lets a caller
 * reconfigure a named pin without rebuilding its id.
 */
static void vendor_config(uint32_t id, uint32_t value)
{
    sl6806_pad_configure((id & 0x000FF800u) | (value & 0x000007FFu));
}

static const sl6806_gpio_vendor_t padctl_vendor = {
    .write   = vendor_write,
    .read    = vendor_read,
    .set_dir = vendor_set_dir,
    .config  = vendor_config,
};

const sl6806_gpio_vendor_t *sl6806_padctl_vendor(void)
{
    return &padctl_vendor;
}
