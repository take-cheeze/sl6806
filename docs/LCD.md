# Display: what exists, and what is needed

The display stack is in three layers. Two of them are finished.

| Layer | State |
|---|---|
| Drawing — framebuffer, primitives, font, `Display`/`Screen` | **done**, 64 host tests |
| Panel — geometry, init sequence, windowing, sleep/wake | **done**, recovered from the firmware, 53 host tests |
| Bus — putting a byte on the wire | **missing**, and it is the only thing missing |

## The panel is known

Everything about the P20 Player's panel was read out of the stock firmware
and lives in `variants/p20_player/panel.c`:

- **240 x 296**, drawn at controller offset **(0, 12)**
- **RGB565** (`COLMOD 0x55`), `MADCTL 0x00`, display inversion on
- standard MIPI DCS command set: `CASET 0x2A`, `RASET 0x2B`, `RAMWR 0x2C`
- a **33-command vendor init sequence**, ending
  `INVON / SLPOUT / 120 ms / DISPON / 50 ms`
- sleep, wake, display-on and display-off sequences
- reset pulse: high / 10 ms / low / 20 ms / high / 120 ms, on the pin the
  vendor code calls `0x13800`

None of that is transcribed by hand. Regenerate it from any dump:

```sh
tools/sl6806-panelseq dump.bin          # readable listing
tools/sl6806-panelseq dump.bin --c      # the exact tables in panel.c
```

The tool finds the panel descriptor by signature rather than by address
(`0x2C 0x2E 0x2A 0x2B 0x36` at +0x0F), then disassembles the five routines it
points at. The init sequence is not a table in the firmware — it is ~150
open-coded calls with the byte to send as an immediate in each — so recovering
it means walking the code, which is what the tool does.

`tests/host/test_panel.c` links the real tables and checks them command by
command, so regenerating them cannot silently change what gets sent.

## What is missing: `sl6806_lcd_bus_t`

Three functions:

```c
static const sl6806_lcd_bus_t my_bus = {
    .name    = "…",
    .command = my_command,   /* one command byte + n parameter bytes */
    .pixels  = my_pixels,    /* a run of RGB565 pixels for a RAMWR   */
    .reset   = my_reset,     /* drive the reset pin (optional)       */
};

sl6806_lcd_bus_register(&my_bus);
Screen.begin();
```

Register one and the init sequence, window addressing, framebuffer, text and
`Screen.print()` all start working unchanged. Until something does,
`sl6806_panel_get()` returns `NULL` and `Display::begin()` says why, because a
panel that silently drops frames is worse than an absent one.

## Three ways to write that bus

### 1. Call the ROM routines (easiest, if they are resident)

The stock firmware does not touch LCD registers directly. It calls two
routines in SRAM:

```
0x0080E842  lcd_write_cmd(last, devid, cmd)     devid = 2 on this board
0x0080E8D8  lcd_write_data(last, byte)
```

`last` is 0 on the final byte of a command and 1 while more follow.

These belong to the mask ROM's driver set
([`sl6806_re_notes.md`](sl6806_re_notes.md) §7d), not to the application, so
they may well be resident when your payload runs. That is a question a memory
read answers in one command — see
[DUMPING.md](DUMPING.md#dumping-ram-and-the-mask-rom). If there is real Thumb
code at `0x0080E842`, a bus is two `((void(*)(int,int,int))0x0080E843)(…)`
calls.

Do check first. Calling into whatever happens to be at a fixed SRAM address
is exactly the kind of thing that appears to work and then corrupts something
three seconds later.

### 2. Program the LCD controller

The controller is at **`0x400D9000`**, and unlike the application's copy, a
driver for it *is* in the flash image: the HLKJ bootloader initialises the
same peripheral, and the bootloader is stored verbatim. It even logs the
function name, `HAL_lcdc_module_init`.

The register map is in `cores/sl6806/sl6806_lcdc.h`. The handoff, which used
to be the open question, is:

```
store the command-list address to  +0x88
set bit 0 of                       +0x80
set bit 0 of                       +0x84
```

What is still undecoded is the command-list opcodes themselves —
`0xABAB0005` and `0xCDCDxx03`/`0xCDCDxx02` — and most of the 20-byte config
struct passed to `HAL_lcdc_module_init`. That is the single highest-value
piece of reverse engineering left in this project.

Note that the clock gating is understood: `cores/sl6806/sl6806_cru.h`. (The
clock unit is at `0x40080000`; earlier notes called that the LCD controller,
which was wrong and sent the analysis in a circle.)

### 3. Bit-bang it

Needs the GPIO registers, which are not known. Listed for completeness; the
first two routes are both shorter.

## Memory, before you pick a resolution

An RGB565 framebuffer costs `width * height * 2` bytes:

| Panel | Framebuffer | Fits in payload mode (~38 KB heap)? | Firmware mode (~190 KB)? |
|---|---|---|---|
| 128x64 | 16 KB | yes | yes |
| 160x128 | 40 KB | no | yes |
| **240x296 (this board)** | **139 KB** | **no** | yes |
| 320x240 | 150 KB | no | yes |

At 240x296 a full framebuffer is 139 KB. That rules it out in payload mode
entirely, and leaves little room in firmware mode, so band rendering is the
realistic approach on this board rather than an optimisation.

Render a band and push it repeatedly — `Display::begin(buffer, w, h)` takes a
buffer smaller than the panel, and `displayAt()` places it.
`examples/GfxDemo` does exactly this with a 160x40 band.

`sl6806_dcs_flush()` handles the offset and the clipping, including the case
where a clipped rectangle makes the source rows non-contiguous, so a band that
runs off an edge still lands correctly.

## Testing without hardware

Both the drawing code and the panel logic are hardware-independent:

```sh
make -C tests/host
```

`test_gfx.c` covers the primitives and writes `gfx_demo.ppm` to look at.
`test_panel.c` runs the real init sequence through a recording bus and checks
the window arithmetic against the panel's (0, 12) offset — the two failures
that are hardest to diagnose on a device, because both produce "the screen is
blank" or "the screen is shifted" with no other symptom.
