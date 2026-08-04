# Display: what exists, and what is needed

The display stack is split in two, and only one half can be written without
hardware facts.

**Done and tested** — `cores/sl6806/gfx/`:

- `Framebuffer.c` — RGB565 framebuffer with pixel, line, rect, circle, blit
  and text primitives. Every primitive clips against the buffer bounds.
- `font5x7.c` — the full printable ASCII set.
- `Display.h/.cpp` — the Arduino-facing object. Inherits `Print`, so
  `Screen.print(x)` works like `Serial.print(x)`.

All of it is verified natively by `tests/host/test_gfx.c` (64 checks under
ASan/UBSan), which also writes `gfx_demo.ppm` so the rendering can be checked
by eye.

**Missing** — the panel driver, i.e. one `sl6806_panel_t` in your variant.
That single struct is all that stands between the code above and a picture.

## What is not known

| Fact | Why it is needed |
|---|---|
| Controller (ST7789? ILI9341? vendor part?) | determines the init sequence and command set |
| Resolution and colour depth | framebuffer size, and whether RGB565 is even right |
| Bus: SPI, parallel, or RGB-with-framebuffer | decides what `flush()` has to do |
| Reset / CS / D-C / backlight pins | needed to talk to it at all |
| Init sequence | panels do not come up without one |

If the panel turns out to be RGB-interfaced — the controller scanning a
framebuffer straight out of RAM — then `flush()` collapses to a `memcpy` and
the *only* thing you need is that buffer's address. Worth checking first,
because it is by far the easiest outcome.

## Where the answers are

All of it is in the stock firmware's LVGL 8.x porting layer. From the
reverse-engineering notes, in priority order:

| Symbol | Address | What it gives you |
|---|---|---|
| **`lv_lcd_init`** | **`0x00D3E34C`** | **the vendor porting layer — start here** |
| `lv_disp_get_scr_act` | `0x00D1F9A4` | start of the LVGL core cluster |
| `lv_refr_area` / `_lv_disp_refr_timer` | `0x00C3D908` | the refresh path that calls flush |
| `lv_init` | `0x00D21C20` | display driver registration |

`lv_lcd_init` is where the vendor fills in an `lv_disp_drv_t`. In LVGL 8 that
struct hands you nearly everything at once:

```c
typedef struct _lv_disp_drv_t {
    lv_coord_t hor_res;          /* <- resolution */
    lv_coord_t ver_res;
    lv_disp_draw_buf_t *draw_buf; /* <- framebuffer address and size */
    ...
    void (*flush_cb)(struct _lv_disp_drv_t *, const lv_area_t *, lv_color_t *);
    ...
} lv_disp_drv_t;
```

Read `hor_res`/`ver_res` for the geometry, follow `draw_buf` to the buffer,
then disassemble `flush_cb` — that function *is* the driver you need to
reimplement, and it will show you the bus and the command sequence directly.

## Step by step

1. **Get a real dump.** Nothing here is possible without one. See
   [DUMPING.md](DUMPING.md), and verify it with `tools/sl6806-checkdump`.
2. **Load it in Ghidra**: Raw Binary, `ARM:LE:32:Cortex`, base `0x00C00000`.
   Run the labelling script from the notes before auto-analysis.
3. **Go to `0x00D3E34C`** and find where the `lv_disp_drv_t` is populated.
   Note `hor_res`, `ver_res`, and the `draw_buf` pointer.
4. **Follow `flush_cb`.** Look at what it writes:
   - Writes to a fixed MMIO base with a data register in a tight loop → SPI or
     parallel. Note the base; cross-check it against
     `tools/sl6806-find-mmio`.
   - A `memcpy` into a fixed address → RGB interface with a scan-out buffer.
     That address is all you need.
5. **Note the init sequence.** The byte sequence `lv_lcd_init` sends before
   any pixels is the controller's initialisation. Its first commands usually
   identify the part outright (`0x01` reset, `0x11` sleep-out, `0x29`
   display-on is the ST77xx/ILI9xxx family).
6. **Write the driver** into `variants/p20_player/panel.c`. The file contains
   a worked skeleton.
7. **Check it** with `examples/GfxDemo`, which draws a scene and reads pixels
   back, so you can tell "the driver is wrong" from "the drawing is wrong".

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

Payload mode has the whole 64 KB window for code *and* data, so a full-screen
buffer is out of reach for anything but a small panel. Render a band and push
it repeatedly instead — `Display::begin(buffer, w, h)` takes a buffer smaller
than the panel, and `displayAt()` places it. `examples/GfxDemo` does exactly
this with a 160x40 band.

## Testing without hardware

The drawing code is hardware-independent, so build a UI and test it now:

```sh
make -C tests/host
```

Add cases to `test_gfx.c` and inspect `gfx_demo.ppm`. Anything that renders
correctly there will render correctly on the panel, because the panel driver
only moves finished pixels.
