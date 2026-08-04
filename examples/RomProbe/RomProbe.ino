/*
 * RomProbe - look for the vendor driver routines in memory, and optionally
 * use them.
 *
 * WHAT THIS IS FOR
 * The SL6806's LCD writers, GPIO writer and delay routines are not in the
 * flash image. They belong to the mask ROM's driver set and are called by the
 * stock firmware at fixed SRAM addresses (sl6806_re_notes.md 7d). Whether
 * they are also resident when *your* payload runs is a question nobody has
 * answered, and it is the difference between "the display needs a driver
 * written" and "the display needs six lines of glue".
 *
 * This sketch answers it. It reads the memory at each known entry point and
 * prints it, so you can see whether there is real Thumb code there.
 *
 *     make SKETCH=examples/RomProbe run
 *
 * READING THE OUTPUT
 * A Cortex-M function almost always starts with a push. In Thumb that is
 * 0xB5xx (16-bit) or 0xE92D (32-bit), so a line beginning "10 b5", "38 b5",
 * "f8 b5" or "2d e9" is a live function. All zeros or all 0xFF means the
 * routine is not there in this mode, and you need the mask ROM dump instead
 * (docs/DUMPING.md).
 *
 * IF THEY ARE THERE
 * Set USE_ROM_ROUTINES to 1 below and rebuild. The sketch then builds an
 * sl6806_lcd_bus_t out of the two LCD writers and asks the display stack to
 * run the real panel init sequence.
 *
 * That is deliberately not the default. Calling whatever happens to sit at a
 * fixed address is how you get a fault several seconds later in unrelated
 * code, and the probe above costs nothing while the call might cost you a
 * confusing afternoon. Look first.
 */

#define USE_ROM_ROUTINES 0

/* Entry points, from the stock firmware's own call sites. */
static const struct {
    uint32_t addr;
    const char *name;
} kEntries[] = {
    { 0x0080E842, "lcd_write_cmd(last, devid, cmd)" },
    { 0x0080E8D8, "lcd_write_data(last, byte)" },
    { 0x00811C7C, "gpio_write(id, value)" },
    { 0x00811C90, "gpio_config(id, value)" },
    { 0x008072E4, "delay_ms(ms)" },
    { 0x00805454, "log/printf" },
};

static bool looksLikeCode(uint32_t addr)
{
    uint16_t first = *(const volatile uint16_t *)addr;

    /* push {…, lr} in either encoding. */
    return (first & 0xFF00) == 0xB500 || first == 0xE92D;
}

static void dumpEntry(uint32_t addr, const char *name)
{
    const volatile uint8_t *p = (const volatile uint8_t *)addr;

    Serial.print("  0x");
    Serial.print(addr, HEX);
    Serial.print("  ");
    for (int i = 0; i < 8; i++) {
        uint8_t b = p[i];
        if (b < 0x10)
            Serial.print('0');
        Serial.print(b, HEX);
        Serial.print(' ');
    }
    Serial.print(looksLikeCode(addr) ? " <- code   " : " <- not code ");
    Serial.println(name);
}

#if USE_ROM_ROUTINES

typedef void (*rom_cmd_fn)(uint32_t last, uint32_t devid, uint32_t cmd);
typedef void (*rom_data_fn)(uint32_t last, uint32_t byte);
typedef void (*rom_gpio_fn)(uint32_t id, uint32_t value);

/* The Thumb bit has to be set or the call faults immediately. Written as
 * macros rather than initialised pointers so nothing has to run before
 * setup() for these to be valid. */
#define romCmd   ((rom_cmd_fn)(0x0080E842u | 1u))
#define romData  ((rom_data_fn)(0x0080E8D8u | 1u))
#define romGpio  ((rom_gpio_fn)(0x00811C7Cu | 1u))

/* The panel descriptor's device id field, +0x0D. */
static const uint32_t kDevId = 2;

static void busCommand(uint8_t cmd, const uint8_t *args, uint8_t nargs)
{
    /* `last` is 0 on the final byte of the command and 1 while more follow -
     * so a command with parameters is sent with last = 1. */
    romCmd(nargs ? 1 : 0, kDevId, cmd);
    for (uint8_t i = 0; i < nargs; i++)
        romData(i + 1 < nargs ? 1 : 0, args[i]);
}

static void busPixels(const sl6806_color_t *src, uint32_t count)
{
    /* Byte at a time, which is slow but has no unknowns in it. A real driver
     * would hand this to the LCDC's DMA - see cores/sl6806/sl6806_lcdc.h. */
    for (uint32_t i = 0; i < count; i++) {
        bool last = (i + 1 == count);
        romData(1, (uint8_t)(src[i] >> 8));
        romData(last ? 0 : 1, (uint8_t)(src[i] & 0xFF));
    }
}

static void busReset(uint8_t level)
{
    romGpio(SL6806_PANEL_RESET_PIN_ID, level);
}

static const sl6806_lcd_bus_t romBus = {
    "ROM lcd_write", busCommand, busPixels, busReset, nullptr,
};

#endif /* USE_ROM_ROUTINES */

void setup()
{
    Serial.begin(115200);
    while (!Serial)
        ;

    Serial.println();
    Serial.println("SL6806 ROM routine probe");
    Serial.println("first 8 bytes at each address the stock firmware calls:");
    Serial.println();

    int live = 0;
    for (unsigned i = 0; i < sizeof(kEntries) / sizeof(kEntries[0]); i++) {
        dumpEntry(kEntries[i].addr, kEntries[i].name);
        if (looksLikeCode(kEntries[i].addr))
            live++;
    }

    Serial.println();
    Serial.print(live);
    Serial.print(" of ");
    Serial.print((int)(sizeof(kEntries) / sizeof(kEntries[0])));
    Serial.println(" entry points look like live code.");

    if (live == 0) {
        Serial.println("Nothing is resident in this mode. The routines have to");
        Serial.println("come from a mask ROM dump - see docs/DUMPING.md.");
    } else {
        Serial.println("Worth trying: set USE_ROM_ROUTINES to 1 and rebuild.");
    }

#if USE_ROM_ROUTINES
    Serial.println();
    Serial.println("registering a bus made of the ROM routines...");
    if (sl6806_lcd_bus_register(&romBus) != 0) {
        Serial.println("bus rejected - command() and pixels() are required");
        return;
    }

    /* begin() runs the panel's own init sequence. A full framebuffer would be
     * 139 KB, so use an 8-row band and push it repeatedly. */
    static sl6806_color_t band[SL6806_PANEL_WIDTH * 8];
    if (!Screen.begin(band, SL6806_PANEL_WIDTH, 8)) {
        Serial.println("Screen.begin() failed");
        return;
    }

    for (int16_t y = 0; y < SL6806_PANEL_HEIGHT; y += 8) {
        Screen.fill(((y / 8) & 1) ? SL6806_RED : SL6806_BLUE);
        Screen.displayAt(0, y);
    }
    Serial.println("pushed a test pattern; look at the panel");
#endif
}

void loop()
{
    delay(1000);
}
