# SL6806 Firmware Reverse-Engineering — Handoff Notes

Working log for the `dump.bin` analysis. Everything here was verified against the
4 MiB dump unless marked **(inferred)**. Addresses are CPU/run addresses unless a
"file 0x…" offset is given.

---

## 1. Device

- **Chip:** Zhuhai Smartlink **SL6806**. USB VID:PID `301a:2800`, `iManufacturer`
  string "SmartlinkTechnolmgy" (sic). Internal SoC codename **"spark2"** (build
  paths say `spark2-p4`).
- **Product:** cheap MP3/media player + SD card reader. UI product string "P20 Player".
- **Core:** **ARM Cortex-M4F** (ARMv7E-M, Thumb-2 + hardware FPU). Evidence: Thumb-2
  wide encodings, VFP `vldr`/`vmul.f32`, "Usage fault" handler string.
- **USB modes** (both enumerate as `301a:2800`; tell them apart by SCSI inquiry):
  - Card-reader: inquiry `SSTLINK DDVICE 2.02`, serial `20221008000002`
  - Bootloader (boot ROM): inquiry `SMTLINK DEVICE 2.00`, serial `20220320000001`

## 2. Dump / flash tooling

- Tool: `ilyakurdyukov/smartlink_flash` (`smtlink_dump`).
- **To dump reliably:** power off, hold a boot key while plugging in USB (key varies,
  trial and error) to enter bootloader mode, then:
  `sudo ./smtlink_dump init flash_id read_flash 0 4M dump.bin`
- Notes: run with `sudo` (or udev rules); SL6806 flash is **4 MB**; `init` is
  bootloader-mode only (hangs in card-reader mode). `LIBUSB_ERROR_PIPE` = endpoint
  stall = the device isn't in a mode that accepts the command (usually: not in
  bootloader mode, or kernel driver not detached).
- **Recovery / anti-brick:** the USB download mode lives in the chip's **mask ROM**,
  not in flash — so a bad flash is recoverable by re-entering bootloader mode and
  writing `dump.bin` back. Rules: keep `dump.bin` as golden image; verify you can
  re-enter bootloader + `write_flash` BEFORE overwriting anything; rewrite only the
  FIRM partition, leave the HLKJ bootloader (file 0x0) and partition table (file
  0xf000) intact.

## 3. Address map (verified)

- **Flash is memory-mapped (XIP) at `0x00C00000`.** File offset `O` → CPU address
  `0x00C00000 + O`. Confirmed: 11,725 / 13,114 string pointers resolve at this base.
- **SRAM:** ~`0x00800000`–`0x0083FFFF`.
- Ghidra import: Raw Binary, language **`ARM:LE:32:Cortex`**, base **`0x00C00000`**.

## 4. HLKJ bootloader header (file 0x0) — FULLY DECODED

First-stage bootloader image. Contains the `boot sdupdate` logic.

| Off  | Value        | Meaning |
|------|--------------|---------|
| +00  | `HLKJ`       | magic (`48 4C 4B 4A`) |
| +04  | `0x0081FC00` | load base (RAM) |
| +08  | `0x00820001` | entry (Thumb → runs at 0x00820000) |
| +0c  | `0x60`       | header length |
| +10  | `0xEF6C`     | segment length (payload at file 0x60) |
| +14  | `0xC0D4`     | **CRC16-CCITT-FALSE of segment** `file[0x60 : 0x60+0xEF6C]` ✓ |
| +18  | `0x02`       | seg/type count (inferred) |
| +20  | `0xF000`     | RAM size = seglen rounded up (inferred) |
| +5c  | `0x22EA`     | **CRC16-CCITT-FALSE of header** `file[0x00 : 0x5C]` ✓ |

Both CRCs: poly `0x1021`, init `0xFFFF`, no reflect, no xor-out.

**Repack recipe** (after patching bytes inside the segment):
```python
def crc16(buf, crc=0xffff):
    for b in buf:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc<<1)^0x1021)&0xffff if crc&0x8000 else (crc<<1)&0xffff
    return crc
import struct
fw = bytearray(open('dump.bin','rb').read())
# patch inside file[0x60 : 0x60+0xEF6C]; if size changed, write new len to +0x10 first
seglen = struct.unpack_from('<I', fw, 0x10)[0]
struct.pack_into('<I', fw, 0x14, crc16(fw[0x60:0x60+seglen]))  # payload CRC
struct.pack_into('<I', fw, 0x5c, crc16(fw[0x00:0x5c]))         # header CRC (LAST)
```

## 5. Partition table (file 0xf000) — CORRECTED: 5 entries, not 3

Header is `{u32 count=5; u32 3; u32 0x30; u32 0}` then 16-byte entries of
`{char name[4]; u32 offset; u32 size; u32 trailer}`.

| Name | Flash off | Run addr     | Size      | Trailer      | Contents |
|------|-----------|--------------|-----------|--------------|----------|
| FIRM | 0x010000  | `0x00C10000` | 0x1B80D0  | 0x17770000   | application code (**the target**) |
| PICS | 0x1DB000  | `0x00DDB000` | 0x0E447C  | 0x71170000   | image/UI resources (data) |
| FONT | 0x2C0000  | `0x00EC0000` | 0x0CA39C  | 0xBC590000   | glyphs (data) |
| TONE | 0x3F9000  | `0x00FF9000` | 0x001571  | 0xFFA90000   | **(new)** tones/sound effects |
| PSMP | 0x3FC000  | `0x00FFC000` | 0x004000  | 0x00000004   | **(new)** 16 KiB; trailer is 4, not a checksum — plausibly settings/NVRAM, so preserve it when rewriting flash |

FIRM run range = **`0x00C10000` .. `0x00DC80D0`** (note DC, not CC).

## 6. App (FIRM) header (file 0x10000) — the SD-update format

This is the header the bootloader's `sdupdate` validates (`header pass`/`mark pass`/
`time is not same`). Fields (from the bootloader's own debug strings):

- +00 `firmware_header_len` = `0x30`
- +04 `timestamp` = `0x8F4E4434` (build stamp; compared to decide re-flash)
- +10 `loadToRam` = `0x00804C00`
- +14 `runFrom`   = `0x00804C01` (Thumb → entry 0x00804C00)
- +1c length-to-RAM ≈ `0x5862`
- App vector table at file 0x10030: initial SP `0x0082D63C`, reset `0x00804C00`.
- **(inferred)** ~0x5862 bytes are copied to SRAM at 0x00804C00 (vectors + startup);
  the bulk runs XIP from `0x00C10000+`.

**RESOLVED — the SD-update (`.up`) format.** Decoded from the HLKJ bootloader,
which *is* stored verbatim in flash (file 0x60 → `0x0081FC00`; file + 0x81FBA0
= address). Validator at `0x00824110`, file is `0:\update.up` /
`0:\restore.up`:

| Offset | Field | Check |
|---|---|---|
| +0x00 | `"CONFIG"` | `memcmp(file, "CONFIG", 5)` — header magic |
| +0x06 | u32 | `codeOffsetInByte`, added to `partition_start` |
| +0x16 | `"SL6806"` | `memcmp(file+22, "SL6806", 6)` — **the "mark"** |
| +0x20 | u32 | `partition_start`, defaults to `0x3000` if zero |

Flow: read 512-byte header → magic → `header pass` → mark → `mark pass` →
compare timestamp (equal ⇒ skip the update) → length check → erase
`((len)>>12)+1` 4 KB blocks → write → verify.

**The body CRC is a write-verify, not a stored field.** `crc cmp %x %x`
compares two CRC16s the bootloader computes itself (both init `0xFFFF`): one
over the data read from the file, one over the data read back from flash. So
nothing in the payload needs a precomputed checksum.

**Still open:** the boot-time FIRM loader prints
`firmware_header_len ... loadCrc 0x%x`, implying a `loadCrc` check at boot, but
that code was not found — those format strings appear nowhere as 32-bit
pointers in the bootloader image (unlike `CONFIG` and `restore.up`, which do),
so they are dead strings. Whether `loadCrc` is verified at boot is unresolved.

## 7. UI framework: "w10 xframe" over LVGL

MVP-style scene framework. Object model (confirmed from dispatch code):

```c
typedef struct activity {
    const act_vtable_t *vtbl;   // +0x00  = [this+0]
    uint8_t  state_0c;          // +0x0c  set 1 on request path
    uint8_t  flag_0f;           // +0x0f  tested at handler entry
} activity_t;                   // (partial)

typedef struct act_vtable {
    void   *class_info;         // +0x00  loaded as arg before method calls
    /* +0x04..+0x1c unknown */
    int   (*method_20)(activity_t*, uint32_t msg, uint32_t, uint32_t); // +0x20 confirmed
} act_vtable_t;                 // (partial)

// per-scene message handler; STATIC-dispatched (not stored in any pointer table)
typedef int (*act_handler_t)(activity_t *self, uint32_t msg_id, uint32_t a2, uint32_t a3);
// msg ids seen: 0x110 (request/create), 0x207 (sub-dispatch)
```

- Dispatch: `ldr r4,[this]; ldr r5,[r4,#0x20]; blx r5`.
- `_<scene>_act_impl_*` functions are message handlers; each prints its own name
  (`-%s __act_on_request!`), so screens self-identify in the log.
- **Scene registry** `const char* scene_names[]` at **`0x00CDFB68`** (~18 entries;
  54 `.main` names total). Screens: peto_work, music, fm, folder, bluetooth,
  record_work, photo, video, calculator, camerca, alarm_clock, calendar, sec, share,
  date_time, dict, ebook, plus music_menu/music_album/music_play/video_play/
  photo_display/dict_play/fm_menu/share_menu/app_menu/screen_saver/setting_poweroff.
- Music player entry points: `music_play.main` referenced from `0x00CDE6F8`,
  `0x00CDEE94`, `0x00CE62D0`.

## 7b. Display — IDENTIFIED

- **Panel: 240 x 296, drawn at offset (0, 12).** Descriptor in flash at
  `0x00C519FC`, copied to SRAM `0x0081C1CC`. Layout:
  `+00 u16 width=240, +02 u16 height=296, +04 u16 xoff=0, +06 u16 yoff=12,`
  `+08 u8 colour mode=1 (RGB565), +0A u8 rotation=2,`
  `+0C u8 0x32 = QSPI quad-lane write opcode, +0D u8 0x02 = one-lane opcode,`
  `+0E u8 0x0B,`
  `+0F RAMWR 0x2C, +10 RAMRD 0x2E, +11 CASET 0x2A, +12 RASET 0x2B, +13 MADCTL 0x36,`
  `+14 init=0x00D3F46D, +18 sleep=0x00D3F8CD, +1C wake=0x00D3F8F9,`
  `+20 0x00D3F941, +24 0x00D3F925`
  Found by scanning flash for `+17/+18 == 0x2A/0x2B`; exactly one match, and
  its function pointers match the disassembled routines.
- **Standard MIPI DCS panel on a QSPI bus** behind a **hardware LCDC with
  DMA**, not an RGB scanout panel. See §12b for what QSPI means for the
  command encoding. Sleep = DISPOFF 0x28 / 10ms / SLPIN 0x10 / 120ms; wake =
  SLPOUT 0x11 / 120ms / DISPON 0x29 / 10ms. Descriptor `+0x20` is display-on
  (DISPON / 10ms), `+0x24` is display-off (DISPOFF / 10ms).
- **The full init sequence is recovered.** It is not stored as a table: the
  routine at `0x00D3F46C` is ~150 open-coded calls to the two writer helpers
  with the byte to send as an immediate in each. `tools/sl6806-panelseq`
  disassembles it back into a table; the result is 33 commands ending
  INVON 0x21 / SLPOUT 0x11 / 120ms / DISPON 0x29 / 50ms, with COLMOD 0x55
  (RGB565) and MADCTL 0x00. The vendor register set spans 0x61-0x68,
  0xB1-0xB6, 0xDF-0xEC and 0xF1-0xF6. The tables live in
  `variants/p20_player/panel.c` and are checked byte for byte by
  `tests/host/test_panel.c`.
- **The panel controller is an NV3030B — named, not inferred.** The routine
  at `0x00D3F46C` logs its own name, `nv3030b_lcd_init` (`0x00C7C380`), from a
  literal pool at `0x00D3F834` that sits inside its own body. So the sequence
  `tools/sl6806-panelseq` recovers *is* the NV3030B sequence and
  `variants/p20_player/panel.c` is right; what it gained is a part number.
  This supersedes the older reading of "a vendor variant rather than a stock
  ST7789" - it is a different controller family altogether, which is why the
  register set never matched. A 240x320 controller driven as 240x296 at y
  offset 12 is exactly what an NV3030B behind a short panel looks like.
- **The panel descriptor at `0x00C519FC` decodes completely.** `sl6806-panelseq`
  reads the fields it needs; the rest are:

  | Off | Value | Meaning |
  |---|---|---|
  | +0x00 | `0x012800F0` | u16 width 240, u16 height 296 |
  | +0x04 | `0x000C0000` | u16 x offset 0, u16 y offset 12 |
  | +0x08 | `0x00020001` | u16 mode 1, u16 device id 2 |
  | +0x0C | `0x32` | **QSPI pixel-stream opcode** |
  | +0x0D | `0x02` | **QSPI command opcode** - this is the byte the writers pass as `devid`, i.e. the `02` of `02 00 <cmd> 00` |
  | +0x0E | `0x0B` | **QSPI read opcode** |
  | +0x0F..+0x13 | `2C 2E 2A 2B 36` | RAMWR, RAMRD, CASET, RASET, MADCTL |
  | +0x14..+0x24 | five pointers | init, sleep, wake, display_on, display_off |
  | +0x2C | `4` | [?] |
  | +0x30, +0x34 | `0x0081C1FC` | SRAM state, both slots the same |
  | +0x38, +0x3C | `0x00D42174`, `0x00D420AC` | **both are `movs r0,#0; bx lr`** - stubs, so nothing is missing here |

### The bootloader drives a *different* panel, and it is not this one

Worth knowing before comparing the two LCD paths, because they look
interchangeable and are not. The HLKJ bootloader has its own panel driver and
its own descriptor:

| | bootloader | application (FIRM) |
|---|---|---|
| init routine | `st7388_lcd_init` at `0x00828604` | `nv3030b_lcd_init` at `0x00D3F46C` |
| descriptor | `0x0082EB3C` | `0x00C519FC` |
| geometry word | `0x01810140` (width slot = **320**) | `0x012800F0` (240 x 296) |
| first commands | `F0 08`, `F2 08`, `9B 51`, `86 53`, `F2 80`, `F0 00`, `F0 01`, `F1 01`, `B0 56`, `B1 4D`, `B2 24`, `B4 66` | `FD 06 08`, `61 07 04`, `62 00 44 40`, `63 41 07 12 12`, ... |
| descriptor layout | same fields, minus the offsets word at +0x04 | as above |

The two sequences share no registers beyond the standard DCS ones, and the
bootloader's is an ST7789/ST7796-shaped `F0`/`F2` unlock plus `B0`-`B4`. Its
geometry word puts 320 where the application puts 240; read as u16 pairs the
height slot comes out 385, which is not a panel, so the bootloader's struct
either carries one field this note has not placed or the entry is stale
vendor boilerplate. Either way it is not describing the glass FIRM drives.
Only one panel is compiled into the bootloader - a full string scan of the
segment finds `st7388_lcd_init` and no sibling - so there is no table to
select from.

**What this does and does not change for `sl6806_lcdc.c`.** It does not
invalidate the transcription: `HAL_lcdc_module_init` is panel-agnostic (it
copies a 14-byte config into registers and nothing else), and the config the
bootloader passes - `9,9,9,9` into `+0x00`, interface type 2, `+0x20` bits
21:20 = 1 and 11:10 = 3 - is not derived from the descriptor's geometry. Both
paths use the same QSPI opcodes `0x02`/`0x32`/`0x0B` and the same DCS bytes.
What it does mean is that **the bootloader's panel behaviour is not evidence
about this panel**: anything reasoned from "the bootloader's driver works, so
these registers are right" is reasoning from a driver for different glass.
- **The backlight is a PWM channel, not part of the LCD path.** `/dev/pwm_ch3`
  (`0x00C68585`), opened at `0x00D10354` with a 48000 Hz period and a duty of
  `percent * 480`; the default is 60% and the setter is `0x00D102F4`. Strings
  `brightness percent %d time:%ld` and `pwm1_event_callback` are alongside.
  Consequence for bring-up: **a working LCD driver still shows a black screen**
  unless something turns this on.

  **The PWM registers are not recoverable from these dumps.** The device-name
  strings `/dev/pwm_ch0`..`ch5` sit at `0x00C7DE9A`, but nothing in the image
  references them - no literal-pool entry, no ADR - so the driver that
  registers them is SRAM-resident, the same wall §7d describes. The driver
  framework (`open` at `0x00D3D1E8`) walks a runtime linked list, so the
  registration is not a static table either. And unlike the LCD writers there
  is no bootloader copy: the HLKJ image contains no PWM code or strings at
  all. The mask ROM has none either.

  The practical route looked like a GPIO rather than PWM - a backlight enable
  driven high is usually full brightness. **That has now been tested
  exhaustively and it is not one.** Every pad on the part, all six banks, was
  driven high individually on hardware (`examples/PadSweep`); none lights the
  panel, while the stock firmware lights it brightly. Either the enable is
  active low (untested), or the driver wants a pulse train rather than a
  level, or it hangs off the PMU rather than a pad.

### 7g. The board's pad map, recovered from the call sites

`tools/sl6806-padscan` decodes every pad id the firmware passes to the pad
and GPIO routines - 78 of them - which is this board's pin assignment. The
seven plain outputs are the ones that matter for bring-up:

| pad id | bank/pin | evidence |
|---|---|---|
| `0x000138CB` | 1/20 -> 1/7 | the panel reset line; a check that the scan is right |
| `0x0001A0D0` | 1/20 | **`vcomo`** - the console handler at `0x00D0AEE0` maps that string to this id and its low twin `0x0001A090`. VCOM is an LCD supply |
| `0x000508C0` | 5/1 | driven high at `0x00D3CBFE` and low at `0x00D3C9DC`/`0x00D3CC3C`, the display's suspend/resume pair |
| `0x000300C0` | 3/0 | output set high, `0x00D46432` |
| `0x00047080` | 4/14 | the **camera's RESET**; power sequencing at `0x00D44B1C`, §7h |
| `0x00047880` | 4/15 | the **camera's PWDN**; same routine, §7h |
| `0x00018000` | 1/16 | the **touch controller's RESET**, `0x00D4024C`, §7h |
| `0x00016800` | 1/13 | the **touch controller's INT**, active low, `0x00D3FF24`, §7h |

Other groups worth naming: bank 1 pins 1-8 are the LCD's QSPI pads
(function 2) with a matching teardown to function 15; **bank 4 is the camera**
- its sixteen function-2 pads are the DVP bus, with 4/3 the sensor MCLK and
4/12, 4/13 the TWI0 pins the sensor answers on (§7h); bank 3 pins 1-6 are
function 11; bank 1 pins
22-27, 30, 31 go through `gpio_config` and are the likely button group -
these are the ids §7d recorded under the old, wrong 6-bit pin decode.

**LCDC `+0x14` bits 27 and 17 are not clearable, so they are level status, not
latched flags.** Writing bit 27 back the way the vendor's ISR does
(`0x00829C9C`: write the mask, spin until it reads 0) leaves `+0x14` reading
`0x08020000` exactly as before. So the fact that this driver never runs that
ISR is *not* a divergence worth worrying about - the bits are almost certainly
steady-state indicators such as "FIFO empty" or "idle" rather than pending
completions. Tried on hardware; no effect on the panel.

**LCDC `+0x08` bit 4 is command-list mode — measured, not inferred.** Setting
it and then using the register-driven transfer path produces a controller that
never reports completion: every transfer times out. So the bit switches the
controller between being fed by a command list and being fed by its registers,
which is consistent with the vendor setting it only in the command-list
builder. Corollary worth keeping: the controller demonstrably changes
behaviour in response to what we write, so we are talking to a live
peripheral, not a dead address.

**Inverting `+0x20` bit 17 makes a 16-byte data transfer take 21 status polls
instead of 6.** That is the signature of a per-transfer command frame being
emitted or not, which supports reading bit 17 as "emit the frame". Neither
polarity, nor either polarity of bit 18, nor any of the eight combinations of
those with bit 4, produces a picture.

**The pad input buffer is off in almost every alternate function.** Measured
across all eight of bank 1's LCD pads and all sixteen function nibbles, with a
pull-up applied (`examples/PadScope`), and the result is identical on every
pad:

| function | 0 | 2-13 | 14 | 15 |
|---|---|---|---|---|
| input register reads | 1 | 0 | 1 | 0 |

Function 0 is the control - a plain input, where the pull-up is visible.
Function 14 is the only other mode that leaves the buffer alive. Twelve
different peripheral functions all happening to drive the pad low is not a
credible reading, so the buffer is simply switched off outside those two.

Consequence: **there is no pad function in which the LCD controller is
connected and the pin is also readable**, so no software test can watch that
bus. Anyone tempted to write another one should read this table first.

**Do not sweep pads to find a rail.** Driving all 192 wedges the device -
something USB needs is among them - and because the console ring is only read
by USB polling, the freeze also destroys the evidence of which pad did it.
- Reset over GPIO id `0x13800` = bank 1 pin 7 (§7f): high / 10ms / low / 20ms
  / high / 120ms (`0x00D3E1A4`). The pad is configured as `0x000138CB` -
  output, initially high - by the LCD pad setup at `0x00D3E9DC`, which also
  puts bank 1 pins 1..6 and 8 on function 2 as the QSPI data pads.
- `lv_lcd_init` (`0x00D3E34C`) installs 8 ops into a struct at `0x008298B8`;
  the resolution getter returns `[[0x008298B8]+0]` / `+2`.
- `lcdc_set_descriptor` (`0x00D3E728`) builds a DMA descriptor in SRAM at
  `0x00829908` using magic words `0xABAB0005`, `0xCDCD6203`, `0xCDCD0A03`,
  packing coordinates big-endian as CASET/RASET require.
- **CORRECTION: `0x40080000` is the clock & reset unit, not the LCDC.** The
  whole display path in flash touches it at exactly four offsets - 0x64,
  0x74, 0x10C, 0x120 - and never reads or writes a data path. The bootloader
  uses the same block as a divider bank with a bit-31 "update busy" poll.
  Map in `cores/sl6806/sl6806_cru.h`.

**RESOLVED: the LCDC is at `0x400D9000`, and its driver is in flash.** See
§12b - the descriptor goes to LCDC `+0x88` and the controller is started with
`+0x80` bit 0 and `+0x84` bit 0.

## 7h. Touch and camera — IDENTIFIED

This board has both, and they are two different I2C devices on two different
TWI buses. §7g's `0x18000` and the pair `0x47080`/`0x47880` are now attributed.
Nothing here is guessed from a product photo: every number below is an
immediate or a table in `dump.bin`, which is this unit's own flash.

### The touch controller: a Hynitron CST816 family part, TWI 1, address 0x15

Driver at `0x00D3FE00`-`0x00D40300`. The register map is the published
CST816S/CST816T one, which is what identifies the part - the firmware never
names it.

| Operation | Transfer |
|---|---|
| chip id | read reg `0xA7`, 1 byte -> `tp drv chip id = 0x%x` |
| firmware version | read reg `0xA9`, 2 bytes -> `tp drv fw version = 0x%04x` |
| touch data | read **7 bytes from reg `0x00`** (`0x00D4018C`) |
| sleep | write reg `0xE5` = `0x03` (`0x00D4022E`), used by `tp_suspend` |

Bus parameters: `twi_init(1, 0x30D40)` at `0x00D463F0`, so **bus 1 at 200 kHz**,
address **`0x15`** (7-bit - the same argument slot carries `0x68` for the
camera, and `0x15` is odd, so this is not an 8-bit address).

The 7-byte read is decoded exactly like the vendor's, `buf[n]` = register `n`:

```
x = ((buf[3] & 0x0F) << 8) | buf[4]
y = ((buf[5] & 0x0F) << 8) | buf[6]
buf[3] >> 4:   0 = press-down    4 = lift-up    8 = contact
```

Two constants fall out of the path above it, both worth knowing before
trusting a coordinate: **y is clamped to 286** (`0x11E`, at `0x00D3FFD2`), and
**swipes are computed in software, not read from the chip.** The chip's
gesture register (`0x01`, `buf[1]`) is read and ignored; on lift-up the driver
compares |Δx| and |Δy| against a **19-pixel** threshold and fires a direction
callback for whichever axis wins (`0x00D3FFB0`).

`ioctl` cmd 3 sets the active area from one word - high half = horizontal
limit at `+0x30`, low half = vertical at `+0x32` - and logs it as
`=== set lcd para: %x, tp_hor: %d, tp_ver: %d`.

**Pads.** This is the part that matters for §12's pinout item.

| Line | pad id (cfg) | bank/pin | evidence |
|---|---|---|---|
| TP **RESET** | `0x00018000` (`0x000180F0`) | 1/16, output, drive 3 | `gpio_write(id,0)` / 10 ms / `(id,1)` / 50 ms, then immediately the `0xA7` read (`0x00D4024C`) |
| TP **INT** | `0x00016800` (`0x00016F08`) | 1/13, **alt function 14**, pull-up | IRQ armed at `0x00D3FEF0`; handler `0x00D3FF24` |
| TWI1 SDA/SCL | `0x00017618`, `0x00017E18` | 1/14, 1/15, alt function 12, drive 1, pull-up | configured immediately before `twi_init(1, ...)` |

The INT line is **active low**: the handler reads the pad and only queues a
transfer when it reads 0. Alt function 14 is consistent with §7g's measured
pad table, where function 14 is one of only two functions that leave the pad's
input buffer alive - which is exactly what an interrupt input needs.

### The camera: a 1 MP sensor the firmware calls `sc101`, TWI 0, address 0x68

Driver at `0x00D44B00`-`0x00D44D60`, registered through a descriptor at
`0x00C51D34` whose first word points at the string `sc101` and whose slots
hold `0x00D44CBC` (init), `0x00D44B1C` (power on) and five more.

| Operation | Transfer |
|---|---|
| chip id | read regs `0xF7` then `0xF8`, one byte each; must be **`0xDA`, `0x4A`** |
| register style | **paged**: reg `0xF0` selects the high byte, so `F0=32` then `03=78` means 16-bit register `0x3203 = 0x78` |
| init | table of `{reg, val}` byte pairs, `FF FF` terminated, ~40 µs between writes (`0x00D44C50`) |

Bus parameters: `twi_init(0, 0x186A0)` -> **bus 0 at 100 kHz**, address **`0x68`**.

There are **two init tables**, chosen by the init routine's argument:

| table | address | pairs | tail |
|---|---|---|---|
| A | `0x00C7DA51` | 192 | no crop block - full array |
| B | `0x00C7DBD3` | 203 | a `0x32xx` crop window |

Table B's crop decodes cleanly and is what sizes the sensor:

```
0x3200 = 0x0140 (320)   0x3204 = 0x03C7 (967)    x: 320..967  -> 648 wide
0x3202 = 0x0078 (120)   0x3206 = 0x025F (607)    y: 120..607  -> 488 tall
```

A centred 648x488 window (VGA plus the usual 8-pixel margin) inside an array
that must therefore be **1280x720** - 320 + 648 + 312 and 120 + 488 + 112 both
land exactly. So table A is 720p, table B is VGA, and "sc101" is a 1 MP part,
which is what the name says. After the terminator the routine optionally
writes `0x3221 = 0x06`, gated on a global at `0x008299CA` - a flip/mirror bit.

**Pads and the power-up order** (`0x00D44B1C`):

| Line | pad id | bank/pin |
|---|---|---|
| **MCLK** | `0x00041930` | 4/3, alt function 2, drive 3 |
| **PWDN** | `0x00047880` | 4/15, output |
| **RESET** | `0x00047080` | 4/14, output |
| TWI0 SDA/SCL | `0x00046938`, `0x00046138` | 4/12, 4/13, alt function 2, drive 3, pull-up |

```
mux MCLK -> clock channel 6 programmed with 2800 -> PWDN low, RESET low
  -> 5 ms -> RESET high -> 5 ms -> PWDN high -> 20 ms -> read chip id
```

Note the level convention: `gpio_write` takes the **level in bit 6** of the
packed id, so the "drive high" calls are literally `gpio_write(id, 0x40)`.
That is the same encoding §7e describes, used as a value rather than an id.

Downstream of the sensor is a DVP/CSI front end feeding hardware codecs -
`/dev/jenc` (JPEG, writes `1:\photo\YYMMDD-HHMMSS.jpeg`), `/dev/henc` (H.264,
writes `1:\video\..._NN.avi`), plus `/dev/jdec`, `/dev/hdec`, `/dev/g2d`. The
scene is `camerca` (the vendor's own typo) and it has a dedicated `CAMERCA`
key. `drv_henc_open` logs `-bind_dvp_channel CSI_CHAN failed`, so the DVP is a
named channel resource rather than a register block the app touches.

### SETTLED, NEGATIVE: neither driver can be ported from `dump.bin` alone

Both drivers reach the hardware entirely through the mask ROM / SRAM library,
the same wall §7d and §7f describe for GPIO:

```
0x0080FF64  twi_write(bus, addr, buf, len, stop)
0x0080FFD4  twi_read (bus, addr, buf, len)
0x00D9A310  twi_init (bus, hz)                  [in FIRM, wraps the two above]
0x008051F4  pad_configure(packed_pad_id)        one argument, not the 0x811Cxx pair
0x008051EC  module_clock_enable(id)             23 = TWI1, 58 = TWI0
0x00811C78  gpio_read(id)
0x00811C7C  gpio_write(id, value)               (§7d already had this one)
0x00811C84  gpio_irq_setup(id, mode)
0x00811C88  gpio_irq_status(id)
0x00811C8C  gpio_irq_clear(id)
```

The four-byte spacing at `0x00811C78`..`0x00811C8C` says that block is a thunk
table, which extends §7d's three entries rather than contradicting them.

To confirm there is no second path, FIRM was scanned twice for a peripheral
base: every PC-relative literal *and* every `MOVW`/`MOVT` pair across the whole
1.8 MB partition. **Zero `0x4xxxxxxx` constants from either scan** in the
touch, camera or codec driver regions - the one apparent hit at `0x00D42EB4` is
a float constant pool (`0x40040000` is 2.0625). And per §7f none of the
addresses above is resident in bootloader mode; `sram.bin` reads as noise at
all nine.

So the TWI controller base is the blocker for both. Finding it is the same
kind of job that §12b did for the LCDC, and it buys two devices at once.

### But neither device has to wait for it: both buses are pads

The TWI base costs speed, not access. Every line either device needs is a pad
whose id is in the tables above, and the pad controller is a driver
(`sl6806_padctl.h`, out of the mask ROM), so I2C is software:

- `examples/TouchDemo` bit-bangs bus 1 and reads the CST816's seven-byte
  touch report. Seven bytes per touch at ~100 kHz is nowhere near a bottleneck.
- `examples/CameraDemo` does the same for bus 0 and reads the sensor's chip
  id. It probes both SDA/SCL assignments, because the id order and the pin
  order in the table above disagree, so which pad is which is an assumption.

**The camera has one line the touch panel does not: MCLK.** The vendor muxes
it to alternate function 2 and programs clock channel 6 with 2800, and that
channel's registers are unknown - programmed through the same hidden library.
A sensor of this family clocks its register block from MCLK, so with no clock
it can be alive and still never ACK. `CameraDemo` therefore tries the pad two
ways, the vendor's mux (in case something already drives channel 6) and a
software square wave on the pad as an output, which is slow and not
continuous but is not nothing. If the sensor answers under either, the
missing channel is a frame-rate problem rather than an access one.

### HARDWARE RESULT: bus 0 works, and the camera is not what is on it

Run on a P20, 2026-08-13, with `examples/CameraDemo`. Three findings, in the
order they were established.

**1. The TWI0 pads are the other way round from the table above.** Scanning
`0x08..0x77` after the vendor's power-up:

| SDA | SCL | result |
|---|---|---|
| bank 4 pin 13 (`0x46938`) | pin 12 (`0x46138`) | nothing answers |
| **bank 4 pin 12 (`0x46138`)** | **pin 13 (`0x46938`)** | **`0x10`, `0x11`, `0x60` answer** |

Both pads idle high under most of the ROM's pull selectors and both drive low
and release cleanly, so this is not a marginal difference - one assignment has
devices on it and the other is silent. **SDA is `0x00046138`, SCL is
`0x00046938`.** The order §7h lists them in was the order they are configured
in, which is not the same thing.

**2. Those three addresses are the FM tuner - see §7i.** Not the camera, and
not a surprise once found: the board has an FM radio and its driver is on this
bus.

**3. `0x68` answers nothing, on a bus that demonstrably works.** The same
sketch reads the tuner's documented chip id through the same bit-banged code
before drawing any conclusion, so the pads, the assignment, the pull-ups, the
timing, the ACK handling and the byte order are all verified against a known
answer at the moment the camera is called silent.

That removes the entire bus from the list of suspects.

**4. And MCLK has now been measured out of the list too.** The software square
wave was rewritten to toggle the pad through its set/clear registers directly
and to run for an interval rather than for a number of edges, and it measures
**2.8 MHz** on hardware, held for 30 ms before the first transfer and through
every bit of it. The vendor programs channel 6 with **2800**, and 2.8 MHz is a
thoroughly ordinary sensor MCLK where 280 MHz is not - so the argument is
that the unit is kHz and *this sketch is now delivering the vendor's own
nominal clock*. The sensor still does not ACK.

`0x00046938`/`0x00046138` are also confirmed as the one and only bus: the FM
driver configures the very same two pads (`0x00046918`, `0x00046118` - same
pins, same alt function 2, only the drive strength differs) before its own
transfers, so there is no second TWI0 pad option hiding the camera.

**5. The four nets themselves were then measured**, by driving a level,
releasing to an input with no pull, and timing how long the level survives -
a resistor drags a net back in microseconds, leakage takes many milliseconds:

| net | high held | low held | reading |
|---|---|---|---|
| SDA, 4/12 | >10 ms | **1 µs** | external pull-up |
| SCL, 4/13 | >10 ms | **1 µs** | external pull-up |
| RESET, 4/14 | >10 ms | >10 ms | **no external pull** |
| PWDN, 4/15 | >10 ms | **3 µs** | external pull-up |

The two bus pads are the control group - they are known to carry external
pull-ups, since the sweep reads them high with no internal pull - and they
behave exactly as they must, which is what makes the other two readings worth
anything.

**The pull table is not uniform across these four pads, and reading it wrongly
produced a false finding.** The sweep was originally run on the two bus pads
only, and the selector it chose from them - 6 - was then used on all four.
Sweeping all four says why that was wrong:

| pad | selectors that read high |
|---|---|
| SDA 4/12, SCL 4/13 | 6, 7, 8, 9, 10, 11, 13, 15 |
| PWDN 4/15 | 6, 7, 8, 9, 10, 11, 13 |
| **RESET 4/14** | **8, 9, 10, 11, 13** - not 6, not 7 |

On a pad with an external pull-up, *every* selector that is not a pull-down
reads high, because the external resistor does the work - so the bus pads
cannot tell an internal pull-up from no pull at all. Pin 14 has no external
pull-up, and there selector 6 does nothing. Under it, RESET stayed low after
being driven low, and that was written up as "something external holds this
line down". **It does not.** With the vendor's own selector 8, RESET rises in
1 µs like every other pad. Selector 8 is what §7h's own pad ids carry, so the
evidence was there to be used.

Timing how long an internal pull takes to move each net - a proxy for the
capacitance hanging off the pad - gives the same answer everywhere:

| net | pull-up | pull-down |
|---|---|---|
| SDA 4/12 | 1 µs | 1-2 µs |
| SCL 4/13 | 1 µs | 1-2 µs |
| RESET 4/14 | 1 µs | 1 µs |
| PWDN 4/15 | 1-2 µs | 1-2 µs |

So all four nets are small and none is loaded differently from the bus pads
that demonstrably reach a fitted chip. That kills the last software-visible
way to ask whether a module is on the end of the traces: a bare stub and a
trace to a high-impedance input look identical at this resolution.

**PWDN's pull-up is a fitted component**, so the board does carry camera
circuitry; the easy explanation that this unit simply has no camera is weaker
than it looked. RESET floating proves less: a sensor's reset pin is a
high-impedance input, so a trace running to one with no resistor floats
exactly like a trace running nowhere. The measurement separates pulled nets
from unpulled ones, not connected ones from empty ones.

RESET also drives high perfectly well at the vendor's drive strength of 0, so
"the sensor is held in reset" is ruled out too.

### SETTLED: the camera module is fitted and works

**The stock firmware's camera app shows a live preview on this unit.** That is
the vendor's own driver, on the same board, driving the same sensor at the same
address on the same bus this branch has been probing.

It closes the question that was still open and reverses one of the two
candidates outright:

- **"No module fitted" is dead.** The part is there and it works.
- **Sensor power is not the leading explanation any more, it is the
  explanation.** Everything else about the path is now positively confirmed
  from both ends: the pads, the bus, the address and the clock are right,
  because the vendor reaches the same sensor over them. What the vendor does
  and a payload does not is switch something on.

The vendor's `power_on` at `0x00D44B1C` contains no such call - it is pads,
delays and clock channel 6, nothing else (decoded above). So the rail is
enabled further up the stack, before the sensor driver is ever entered, which
points once more at the byte-wide register file: a ~131-entry indexed file of
exactly the kind that carries LDO enables, reached through veneers a payload
cannot call.

What is left, in the order the evidence now supports:

- **Sensor power**, now the leading explanation. Every rail a module needs is
  behind a regulator that nothing in this framework drives, and per the decode
  below the vendor's own power-up has no such call in it either - it is pads,
  delays and the clock, nothing else. So whatever switches the camera's supply
  sits further up the stack than the sensor driver. The likeliest home is the
  byte-wide indexed register file behind the clock driver (below): a ~130-entry
  file of the kind SoCs use for LDO and power-domain control, reached through
  SRAM routines a payload cannot call.

  **And the sensor driver writes it - the enable is not further up the stack
  after all.** An earlier revision of this note said the camera driver only
  read registers `0x03` and `0x16` and wrote none. That was wrong: it came
  from a tool-assisted search that missed both write sites. Disassembling the
  region directly finds two read-modify-writes, and they are the missing step:

  ```
  0x00D44EA6   v = read(0x03);  write(0x03, (v & 0xE7) | 0x08)   ; field [4:3] = 01
  0x00D44F06   v = read(0x16);  write(0x16, (v | 0x80) & 0xFF)   ; set bit 7
  ```

  Register `0x03` is shared - the clock driver masks it with `0x5D` and sets
  bit 1, the camera clears bits 3-4 and sets bit 3 - so one control register,
  different fields.

  **Everything about this camera is now specified except one address.** Pads,
  bus, sensor registers, power-up order, clock channel and now the two enable
  writes are all known to the bit. What is missing is where the indexed
  register file sits in the memory map; with that, a payload could run the
  whole sequence itself.
- ~~**No module fitted.**~~ Ruled out: the stock camera app previews live.

Both readings of which power pad is RESET were tried, and neither helps -
worth recording, because under §7h's attribution the vendor's sequence ends by
driving PWDN *high* immediately before reading the chip id, which is backwards
for a line named power-down.

### The MCLK clock channel: a lead, in flash rather than in the ROM

With the bus eliminated, this is the camera's blocker. The whole power-up at
`0x00D44B1C` now decodes with nothing unaccounted for:

```
pad_configure(0x00041930)          ; MCLK, alt function 2
delay(1)
clk(6, 1, 0)                       ; 0x00CC769C
delay_ms(5)
clk(6, 2, &2800)                   ; the frequency argument
clk(6, 0, 0)
pad_configure(0x00047880)          ; PWDN   \  r4 and r5, in that order -
pad_configure(0x00047080)          ; RESET  /  which confirms §7h's attribution
gpio_write(PWDN, 0); gpio_write(RESET, 0)
delay_ms(5);  gpio_write(RESET, 0x40)
delay_ms(5);  gpio_write(PWDN,  0x40)     ; note: PWDN ends *high*
delay_ms(20)                              ; then the chip-id read
```

Every call is a delay, a pad, or the clock. There is no fourth thing - no
regulator enable, no power-domain call - so if sensor power is the problem it
is on the module and not in this routine.

**The op codes fall out of the power-*down* routine.** `0x00D44AB2` is the
camera's teardown, and it is short enough to read whole:

```
rom_1EC4(80)                       ; unattributed
gpio_write(PWDN, 0); gpio_write(RESET, 0)
delay_ms(10)
clk(6, 1, 0)                       ; the only clk call here
pad_configure(0x00047780)          ; RESET \
pad_configure(0x00047F80)          ; PWDN   > all three parked on function 15
pad_configure(0x00041F80)          ; MCLK  /
```

Power-up runs `(6,1,0)`, `(6,2,&2800)`, `(6,0,0)`; power-down runs `(6,1,0)`
alone. So **op 1 stops the channel, op 2 sets its frequency, op 0 starts it** -
which is the ordering both routines need and neither contradicts.

It also settles the three pad ids §7g had left as "the rest of that selector
group, role unknown": `0x00041F80`, `0x00047780` and `0x00047F80` are MCLK,
RESET and PWDN again, with the function nibble at 15 - the parked state, input
buffer off. Not three more pads.

`clk(channel, op, arg)` at **`0x00CC769C`** is called three times on the way
up: `(6, 1, 0)`, then `(6, 2, &2800)` after a 5 ms wait, then `(6, 0, 0)`.
That dispatcher is ordinary flash and it decodes:

```
channel 0..1  ->  0x00CC75F0
channel 2..6  ->  0x00CC7334     <- MCLK is channel 6
```

`0x00CC7334` is the interesting one. It switches on the op in `r1` through a
`tbh` table, and it reaches the hardware **not** through an MMIO address but
through a pair of routines taking a *register index*:

```
0x00804EAC   read  register <n>        called with 27, 3, 44
0x00804E44   write register <n>, value
```

and it compares the requested value against a fixed set - `2700`, `2800`,
`2900`, `3000`, `3200`, `3300` - selecting different bit patterns for each, so
the argument is a menu rather than a divisor.

**That register file is worth finding on its own.** It is not clock-specific:
`0x00804EAC` is read 90 times and `0x00804E44` written 50 times across FIRM,
with register numbers spread over `0x00`..`0x82` and values masked as bytes
(`uxtb`, `and #0x5d`, `orn #0x7f`). A ~130-entry byte-wide indexed file
reached by index rather than by address is the shape of a PMU / analog
register bank on a serial side-channel, not of an MMIO block - which is
consistent with §7c finding no `0x4xxxxxxx` constants anywhere near this code.
The busiest indices are `0x03`, `0x47`, `0x1B`, `0x13`, `0x30`, `0x16`, `0x0F`.

Both routines are in SRAM, so per §7f they are not resident in bootloader mode
and a payload cannot call them. The *caller* is in flash and fully readable,
the argument space is six values, and the same file is behind 140 other call
sites - so whoever finds how `0x00804E44` reaches the hardware unlocks
considerably more than MCLK.

**SETTLED, NEGATIVE: the callee is not in the flash image.** The obvious next
move is to disassemble `0x00804E44` itself, and it cannot be done from
`dump.bin`:

- The FIRM partition (`0x10000`, length `0x1B80D0`) carries a header in the
  same field layout as HLKJ's - `load=0x00804C00`, `entry=0x00804C01`,
  `hdrlen=0x1000`, `seglen=0x5862` at `0x10010`. `load` matching the SRAM base
  this framework's own firmware mode links to is a good sign the reading is
  right, but no CRC16 over the obvious candidate ranges verifies, so treat the
  layout as [I] rather than [V].
- That segment's address range does cover `0x00804E44`, but the code found at
  the implied offset is floating-point DSP, not a register accessor.
- So the whole 4 MB was searched for *any* alignment at which nine known SRAM
  entry points (`0x00804E44`, `0x00804EAC`, `0x008051F4`, `0x008051FE`,
  `0x00805224`, `0x00805454`, `0x008071F6`, `0x008071FA`, `0x008072E4`) all
  land on a function prologue. **Zero offsets score 7 or more out of 9**, and
  the best anywhere in the image is 3.

The library is therefore not stored uncompressed in flash. The mask ROM was
the obvious next place to look - it is where the pad controller turned out to
be - and that hunt has now been run across all three binaries in the tree.

### SETTLED, NEGATIVE: the SRAM library is in none of the three dumps

**And there is a structural reason, which is the useful part.** The addresses
`sl6806-ramcalls` reports are **not function entry points**. Their spacing
gives it away: of the 251, twenty-six consecutive pairs are exactly 12 bytes
apart and fifteen exactly 4, including the run

```
0x008051E8  0x008051EC  0x008051F0  0x008051F4  0x008051F8
```

A four-byte slot holds one instruction, and the only useful one is `b.w`. So
this is a dispatch region - veneers forwarding to implementations elsewhere -
which is exactly how §7h already read the four-byte block at `0x00811C78`. The
implication is that **the targets are written at run time**, so no byte-level
search of a static image can produce them, and every search below was doomed
before it started:

| search | over | result |
|---|---|---|
| 80 in-segment entry points vs function prologues, every 2-byte alignment | dump.bin, maskrom.bin, sram.bin | nothing above chance (best 19/80, in a high-entropy region) |
| 15 four-byte-run entry points vs `B.W` encodings | all three | nothing above 70% |
| self-referencing literals, solving for a constant load delta | dump.bin, maskrom.bin | no dominant mapping (best count 4) |
| any literal equal to a veneer address or to `0x00804C00` | whole 4 MB | **only** the FIRM header at `0x10010` and its vector table - no installer, no copy loop |
| runs of Thumb-odd ROM pointers that could be a target table | FIRM | one 131-entry run at `0x00C526F4`, which is repeated `0x010001` data, not pointers |

`sram.bin` also settles §7f by measurement rather than inference: all nine
addresses read as high-entropy noise in bootloader mode.

So static analysis of `dump.bin`, `maskrom.bin` and `sram.bin` cannot yield
this library. What would: reading SRAM while the **stock firmware** is running,
which needs SWD rather than the USB path, since the veneers only exist once
the application has installed them.

**Two by-products worth keeping.** The ROM's own variables sit at
`0x00804048`..`0x00804504`, i.e. ROM data occupies roughly
`0x00804000`-`0x00804600`, immediately below FIRM's load address of
`0x00804C00` - so that boundary is not arbitrary. And the mask ROM's
peripheral map, by literal frequency, is independent evidence next to §7c's
FIRM-derived list:

| base | refs in ROM | |
|---|---|---|
| `0x40040000` | 125 | |
| `0x40036000` | 92 | not in §7c's list |
| `0x40030000` | 87 | §7c's "heavily used, 16-bit registers" |
| `0x40020000` | 70 | |
| `0x4010E000` | 65 | not in §7c's list |
| `0x40080000` | 27 | the clock and reset unit |

`0x40036000` and `0x4010E000` are new candidates, and one of them is a
plausible home for the indexed register file.

Note also what a working bus does *not* buy: pixels leave the sensor on the
DVP/CSI front end above, which is undecoded. Reading the chip id proves the
device and the pads; it is not a step towards an image on its own.

## 7i. The FM tuner — IDENTIFIED, and the first device ever read on this board

Found by scanning bus 0 while looking for the camera, then attributed from the
firmware. It is an **RDA5807-family FM tuner on TWI 0**, and it is the third
I2C device on this board rather than a second look at one of the two in §7h.

| | |
|---|---|
| driver | `0x00D3D92C` (chip-id read), wrappers at `0x00D3D6F8` / `0x00D3D70A` |
| bus | **0**, the camera's - both wrappers pass `r0 = 0` to the vendor's twi calls |
| address | **`0x10`** in the driver; `0x11` and `0x60` also ACK on hardware |
| chip id | **`0x5808`**, compared at `0x00D3D988` |

The three addresses are one part: `0x10` is the RDA5807's sequential-access
address, `0x11` its random-access one and `0x60` its TEA5767-compatible alias.
All three answered the scan; only `0x10` appears in the firmware.

The id read, verbatim (`0x00D3D92C`):

```
twi_write(0, 0x10, {0x00, 0x02}, 2, stop)     ; 0x00D3D952, low bit is ENABLE
delay 50
twi_read (0, 0x10, buf, 10)                   ; 0x00D3D962
id = (buf[8] << 8) | buf[9]                   ; 0x00D3D972
id == 0x5808 ?                                ; 0x00D3D988
```

Strings around it: `FM chip id: 0x%x` (`0x00C75C1E`), `fm init err`,
`fm_clk_init over`, `/dev/fm`, and a whole `fm_band` / `fm_search` /
`fm_preset_station` UI.

**CONFIRMED ON HARDWARE, 2026-08-13.** `examples/CameraDemo` reproduces that
read over bit-banged GPIO and gets `0x5808` back - **on the read-only pass**,
without the vendor's enable write, so the part answers cold. The ten bytes, as
returned by a plain read from `0x10`, are the tuner's registers `0x0A`..`0x0E`:

| reg | 0x0A | 0x0B | 0x0C | 0x0D | 0x0E |
|---|---|---|---|---|---|
| value | `0x013F` | `0x0000` | `0x5803` | `0x5804` | `0x5808` |

(`0x0A` is the RDA5807's status register; the three `0x58xx` words at the end
are why the driver takes bytes 8-9 rather than a documented id register.)

**Why this matters beyond the radio.** That is the first time any I2C device
on this board has been read by this framework, and it converts a whole class
of "nothing answered" results into evidence: the pads, the SDA/SCL assignment,
the internal pull-ups, the bit timing, the ACK handling and the byte order are
all correct, because a device answered with the one value its own driver
checks for. Any other silent address on this bus is silent for its own
reasons - which is exactly how the camera's remaining suspects were narrowed
to two, neither of them on the bus.

**DRIVEN ON HARDWARE.** `examples/FmDemo` enables the tuner and sweeps the
band over the same bit-banged bus, and it works: 41 channels tuned, every one
setting tune-complete and reading its own channel number back out of the
status register. That is the first time this framework has *written* to a
peripheral device rather than reading one or driving a pad.

RSSI comes back at 9 across the lower band and 0 across the upper, with no
peaks - which is the aerial missing rather than the driver failing, since on
this device the aerial is the headphone lead. The registers used for the tune
are the published RDA5807 ones (`0x02` enable/mute, `0x03` channel and tune,
`0x0A`/`0x0B` status), not anything from the dump - the vendor driver only
ever reads the id and writes the enable.

An FM driver is now a small job

## 7j. The peripheral map, read on hardware

`examples/RegFileProbe` reads `0x00`..`0x82` at word stride from six candidate
bases in payload mode. Read-only, and the clock and reset unit goes first as a
control - it is known real, so if it did not read back sensibly the probe
would be wrong rather than the chip interesting.

| base | result | |
|---|---|---|
| `0x40080000` | varied, 8+ distinct values | the control - clock and reset unit |
| **`0x40040000`** | **varied, 8+ distinct values** | **live peripheral, confirmed** |
| `0x40036000` | all zero | |
| `0x4010E000` | all zero | |
| `0x40030000` | all zero | FIRM uses it heavily, so this is "gated off", not "absent" |
| `0x40020000` | all zero | |

**Nothing faulted at any address.** An all-zero window in bootloader mode is at
least as easily a gated-off peripheral as an empty one, so none of the zeros
is evidence of absence.

**`0x40040000` is now confirmed on hardware**, which §7c only had as a
candidate - and had partly discounted, since its one apparent appearance near
the touch/camera drivers turned out to be a float constant pool. It is the
mask ROM's most referenced base (125 literals) and it reads structured data:

```
+0x00  00004102 000F0000 0F00000E 0001058E   +0x20  00040200 00010200 00000FFF BB000000
+0x40  00000FFF BB000000 0043031E 00494943   ...
+0x28  DA5580F1 5AA2A94A                     random-looking - efuse or chip id?
+0x110 00040200 00010200 00000FFF BB000000   the +0x10 group repeating
+0x120 40000300 400000C0 00000FF8 BB000000   again, with different values
```

The four-register group `{..., ..., 0x00000FFF, 0xBB000000}` repeating at
three offsets is the shape of a multi-channel block rather than a flat
register file.

### The indexed register file is probably not direct MMIO at all

This probe did not find it, and the reason is worth writing down rather than
continuing to look the same way. Two things argue that the file is reached
*indirectly* - an index register and a data register - rather than as
`base + index * 4`:

- The vendor calls a **function** for every access, even a single read. Direct
  word-strided MMIO would be inlined; an indirect sequence has to be a
  function because it is several ordered writes.
- Values are masked as **bytes** (`uxtb`, `and #0x5d`, `orn #0x7f`) across
  ~131 registers, which is a serial/analog register bank convention, not a
  32-bit MMIO block.

That also fits §7c finding no `0x4xxxxxxx` constants anywhere near the drivers
that use it: they reach it through the SRAM veneers, and the base lives in
whatever code the veneers point at.

**A read-only probe cannot confirm an indirect interface**, because proving it
means writing an index and seeing the data register change - and writing to a
suspected power/clock file is exactly what should not be done casually. That
is the wall this line of attack reaches.

## 7l. RETRACTED: `0x40040000` is not the indexed register file

An earlier revision of this section claimed the file had been found at
`0x40040000`, one byte per register. **That was wrong**, and the way it failed
is worth more than the claim was.

**The evidence that looked good.** Sweeping the mask ROM for byte
read-modify-writes through a `0x4xxxxxxx` literal finds a cluster at
`0x04E00`-`0x05800` working on `0x40040001`, `0x40040004`, `0x4004000A`,
`0x4004000B`, `0x4004000E` and `0x40040060` - each `0x40040000` plus a number
inside the file's `0x00..0x82` index range, one of them a bare `ldrb` / `uxtb`
/ set-bit-3 / `strb` setter in exactly the veneers' idiom. That part is real
and still stands; it is simply a different peripheral with byte registers.

**Why the confirmation was worthless.** Four registers were checked against
their supposed meanings, and three of them - `0x03` reading `0x00`, `0x16`
reading `0x01`, `0x0B` having bit 3 set - are satisfied by very nearly any
window. The fourth carried the argument: `0x2C` read `0x4A`, which was
interpreted as the clock driver's channel bitmask with channels 3 and 5 on and
channel 6, the camera's, off. Sampling it eight times in a row settles it:

```
reg 0x2C:  6A DB 31 C4 B5 6A DB 94
```

It changes on every read. It is a counter or a status register, and `0x4A` was
one sample of a moving target. A single reading of a register that never holds
still was the load-bearing evidence for the whole identification.

**The write test.** Writing the vendor's own four values - clock frequency and
channel into `0x03` and `0x2C`, camera field and enable into `0x03` and `0x16` -
was harmless and conclusive:

```
reg 0x03: 0x00 -> 0x20   reads back 0x00   ignored
reg 0x2C: 0x85 -> 0x95   reads back 0x67   changed, but not to what was written
reg 0x03: 0x00 -> 0x08   reads back 0x00   ignored
reg 0x16: 0x01 -> 0x81   reads back 0x01   ignored
```

Nothing stuck, the device stayed up, USB stayed enumerated and the FM tuner
still answered afterwards. Read-back verification is what turned a wrong guess
into a clean negative instead of a mystery, and it is the reason the write was
worth doing at all.

**What `0x40040000` probably is.** A live peripheral with byte-wide registers,
the mask ROM's single most referenced base, holding some registers steady
(`0x60` reads `0x99` every time, `0x16` reads `0x01`) while others free-run -
and busy during USB download mode, which is the only mode we can observe it
in. A USB controller fits every one of those.

### Static analysis cannot locate the file — three more sweeps say so

After the retraction, three further searches, all negative, all recorded so
they are not repeated:

| sweep | result |
|---|---|
| every ROM byte access through an absolute `0x4xxxxxxx` literal | **17 literals, all within ±16 of `0x40040000`** - the USB cluster and nothing else |
| the same over the HLKJ bootloader (flash `0x60`, loads to `0x0081FC00`) | one hit, `0x40001000+12`, unrelated |
| every ROM byte access with a *register* index inside a small function | 86 candidates and no way to tell them apart - the same fishing that produced the wrong answer above |

A Hough-style vote was also tried: every byte-access literal in the ROM voting
for `literal - index` across all 43 indices the veneers are called with, on the
theory that the true base collects votes from many different literals. Every
candidate it produced sits within sixteen bytes of `0x40040000`, because those
are the only literals of that shape in the image.

**So the accessor does not use absolute per-register addresses, and the file is
not a plain MMIO array.** What is left fits a computed base plus index, or a
serial/indirect protocol - either of which is invisible to a pattern search.

**But it is reachable, and cheaply, by one runtime read.** The veneers live in
SRAM and are written at boot; the only code that is always resident to be
written *into* them is the mask ROM. So `0x00804E44` almost certainly branches
to a ROM address, and a single read of that veneer on a running system - four
bytes, over SWD - decodes to the accessor, which then decodes to the base. That
is the whole remaining problem: four bytes that cannot be read from a dump.

**Still true, and independent of all this:** the camera's two enable writes at
`0x00D44EA6` and `0x00D44F06` - registers `0x03` and `0x16` of the file,
whatever its address - and the clock driver's use of `0x03` and `0x2C`. Those
come from the drivers' own code. What remains unknown is where the file lives.

## 7k. PSMP — the settings partition, decoded

The last partition, `PSMP` at `0x3FC000` (`0x4000` bytes), is a key/value store
of the device's own runtime state. Records are appended, so a key appears many
times and the last one wins - which is why a 16 KB partition holds only 27
distinct keys across 128 records.

```
55 AA | crc32? (4) | flags (2) | index (2) | keylen (2) | key | value
```

The keys, in first-seen order:

```
restore_factory  bt_addr     device_id    freq_drift   bt_inqname
bt_showname      bright      clock_type   bluetooch_status (sic)
bt_name          set_screen_time          screen_saver start_screen_time
on_lang_sel      spk_switch  language     volume       MUSIC
voltage          power_on_step            language_name
bt_relink        btlinknum   btlinkinfo   le_addr
alarm_clock_info FM
```

`device_id` on this unit reads
`BJTXKJMP3---P20-------D30-------------F7B2F5D6674A218152--------`, which is
the only SKU-like string on the device - vendor code, model `P20`, a variant
field `D30`, then a serial.

**Relevant to whether this unit has a camera:** there is a persisted `FM` key
(64 bytes of preset stations) for a device we have proved is fitted, and *no
camera key of any kind*. That is suggestive rather than conclusive - a camera
app need not persist anything - and it sits against the fact that the `camerca`
scene is fully wired into the UI, with 26 references and its own dedicated key
(§7h). The firmware offers a camera; the settings have never recorded one being
used.

## 7c. Peripheral map

- **Peripheral MMIO region is `0x40000000`.** Established by decoding every
  PC-relative literal load in FIRM (29696 of them). Candidate blocks by load
  count: `0x40030000` (16-bit registers, heavily used), `0x40009000`,
  `0x400E2000`, `0x40011000`, `0x40040000`, `0x40020000`, and the pair
  `0x40028000`/`0x40029000` which share an offset pattern 0x1000 apart —
  two instances of one peripheral.
- Scanning raw 32-bit words does NOT work for this: Thumb-2 instruction
  encodings dominate (`0x46202000` is `mov r0,r4; movs r0,#0`). Use
  `tools/sl6806-find-mmio`, which decodes LDR-literal instructions.

**Blocks identified so far.** Doing the same literal scan over the *HLKJ
bootloader* rather than FIRM is what named most of these: the bootloader is a
small self-contained program that touches each peripheral once, so its uses
are legible where FIRM's are buried.

| Base | What | Evidence |
|---|---|---|
| `0x40000000` | pad / pin function mux | `0x00820650` rewrites four byte-lanes of `+0x04` (per-pad function nibbles) and a 5-bit field at `+0x08`, switching one bus between two functions |
| `0x40009000` | timers | channels at 0x100 stride (`+0x108`, `+0x208`) with write-1-clear flags and per-channel callbacks; register triples at 0x20 stride |
| `0x40070000` | DMA | per-channel IRQ status at `+0x24`/`+0x2c`, callback table indexed by channel, request routing at `+0x00`/`+0x20`/`+0x28` |
| `0x40080000` | **clock & reset unit** | dividers at `+0x40`/`+0x48` with a bit-31 busy poll; module gates at `+0x64`/`+0x74` bit 15; see `cores/sl6806/sl6806_cru.h` |
| `0x40081000` + `0x400F6000` | **GPIO / pad controller** | six banks, bases in a mask ROM table at `0x00065004`; see §7f and `cores/sl6806/sl6806_padctl.h` |
| `0x400D9000` | **LCD controller** | the bootloader logs `HAL_lcdc_module_init` from the routine that caches this base; see §12b and `cores/sl6806/sl6806_lcdc.h` |
| `0x400F7000` | storage host (SD/MMC + SPI flash) | `+0x100`/`+0x104` command registers with a bit-31 start/busy, `+0x108` argument, `+0x10C`/`+0x110` response, `sdio(e):rx error` strings nearby |

## 7d. RAM-resident code — explained: it belongs to the mask ROM

Several drivers live in SRAM and are **not** in the flash image at any linear
offset. Enumerating every `BL`/`BLX` in FIRM whose target lands in
`0x00800000`–`0x0083FFFF` gives **251 distinct entry points**, which is the
whole callable surface. The most-used ones:

| Address | Calls | Role |
|---|---|---|
| `0x00805454` | 6743 | logging / printf |
| `0x0080E8D8` | 94 | LCD write data (`last`, byte) |
| `0x008072E4` | 56 | delay, milliseconds |
| `0x00807214` | 39 | delay |
| `0x0080E842` | 39 | LCD write command (`last`, devid, cmd) |
| `0x00811C90` | 23 | GPIO configure (pin id, value) |
| `0x00811CB4` | 18 | GPIO, third entry point |
| `0x00811C7C` | 13 | **GPIO write (pin id, value)** |

`tools/sl6806-ramcalls` regenerates that list from a dump.

**Where they come from: the mask ROM.** The application's own vector table
(file `0x10030`) routes SVCall to `0x0000B9D5`, SysTick to `0x0000BA89` and
IRQ 25 to `0x0000873D` — all inside the 500 KB mask ROM at `0x00000000`. The
HLKJ bootloader independently makes **393 calls to 100 distinct ROM entry
points** (`0x00000640` alone 153 times, evidently the log function). So the
ROM is not just a USB downloader: it is the SDK's shared driver library, and
the SRAM band above the app's own 0x5862-byte load is its resident data and
code. That is why no flash→RAM copy exists to find.

**Consequence: dump the ROM, not more flash.** `0x00000000`–`0x0007D000` is
readable with the same vendor read command that loads payloads, in bootloader
mode, where it is the ROM itself answering. See docs/DUMPING.md. **This has
now been done — see §7f for what the dumps say.**

**Three routes tried and eliminated:**

1. *Dump SRAM off a running device* — **does not work.** In card-reader mode
   the stock app services SCSI and does not implement the vendor read command:
   `read_mem` → `LIBUSB_ERROR_PIPE` (endpoint stall), and the attempt resets
   the device. Plain `inquiry` works, so the device is healthy; the app simply
   refuses the command. (Unrelated but worth recording: fwupd probing was
   causing the device to drop off USB ~1.3 s after enumerating. Stop fwupd
   before doing any USB work with this board.)
2. *Contiguous flash→RAM copy* — **ruled out.** A search over every 2-byte
   offset for a delta mapping the known RAM entry points onto Thumb prologues
   gives one candidate, verified as a false positive (the bytes are XIP code
   at `0x00CE25DE`: 100% of BL targets valid there vs 28% under the
   candidate). No aligned linker-style copy table exists either.
3. *The same search with all 251 entry points* — same answer, and worth
   recording because the false positive is seductive. The best delta maps RAM
   `0x00800000` to file `0x035830` and matches 91/251 entry points against a
   noise floor of 25. It is still wrong: file `0x013F62`–`0x03B4B0` is a
   157 KB run of zeros, so the "matched" region begins exactly where real XIP
   code resumes at `0x00C3B4B0`, and self-relative `BL`s inside any code
   region stay self-consistent under an arbitrary base shift. **A
   BL-target-validity test cannot distinguish a real relocation from a
   shifted base** — only an absolute reference can.

## 7e. The vendor GPIO pin-id encoding

The 54 call sites of the three GPIO entry points pass these ids as
immediates:

```
0x13800 0x18000 0x1B000 0x1B800 0x1C000 0x1C800 0x1D000 0x1D800
0x1F000 0x1F800   0x41F80 0x47080 0x47780 0x47880 0x47F80
```

(`tools/sl6806-ramcalls dump.bin --sites 811c7c` prints them, with the
constant in each register at each call site.)

Every one is a multiple of `0x80`, and dividing by `0x800` gives small
integers. Best reading:

| Bits | Meaning |
|---|---|
| `[10:7]` | configuration nibble; only `0x0` and `0xF` observed |
| `[16:11]` | pin selector: 3, 14, 15, 39, 48, 54–59, 62, 63 |
| `[18:17]` | second selector; 0 for the display pins, 2 for one other group |

Whether `[18:17]` is a bank number or simply more pin bits cannot be told
apart from call sites alone — both readings fit. Recorded rather than
guessed at.

Roles established from the calling code:

| Id | Pin | Role |
|---|---|---|
| `0x13800` | 39 | **panel reset** — driven high/10ms/low/20ms/high by `0x00D3E1A4` |
| `0x18000` | 48 | reset of an I2C device: low/10ms/high/50ms at `0x00D401E6`, immediately before register reads to address 0x15 (touch or camera) |
| `0x1B000`…`0x1F800` | 54–59, 62, 63 | configured with value `0x780` from `0x00D93E00`–`0x00D94800` |
| `0x41F80`…`0x47F80` | 3, 14, 15 | driven with 0 and `0x40` in a power sequence at `0x00D44AB8` |

Note the value argument is `0`/`1` in the reset paths but `0`/`0x40`
elsewhere, so it is more likely a pad-register value than a logic level.

The framework exposes this as an optional back end — see
`cores/sl6806/hal_gpio.h` — so a build that can reach `0x00811C7C` gets
working `digitalWrite()` with no register map at all.

## 7f. The mask ROM and SRAM dumps — what they settle

`tools/sl6806-dumpram` was run in bootloader mode and produced a 512000-byte
`maskrom.bin` (`0x00000000`–`0x0007D000`) and a 262144-byte `sram.bin`
(`0x00800000`–`0x00840000`). Neither is committed here; both are reproducible
with the commands in docs/DUMPING.md.

**Both dumps are genuine.** Three independent checks:

- 93.1% of the 7480 `BL`/`BLX` encodings in the ROM resolve to targets inside
  the ROM. A shifted or corrupt image does not do that.
- Every entry point the HLKJ bootloader calls into ROM lands on a real
  instruction. `0x000640` — the one the bootloader calls 153 times — begins
  `push {r0,r1,r2,r3}` / `push {r4,r5,lr}`, the classic varargs prologue,
  confirming it is the log/printf the call pattern implied.
- In the SRAM dump, 83.4% of the words in `0x00800000`–`0x00801000` are
  ROM-range pointers (chance level 0.01%), i.e. a relocated 256-entry vector
  table, and 6.6% of the words just below the ROM's initial SP `0x00806000`
  are ROM-range return addresses. So `read_mem` addresses SRAM correctly and
  the ROM really is running.

### SETTLED, NEGATIVE: the vendor routines are not resident in bootloader mode

This was the cheap hypothesis worth testing — if `0x0080E842` and
`0x00811C7C` held real code when a payload runs, a working LCD bus and
`digitalWrite()` were a few lines of glue away. **They do not.**

| Address | Expected | Found in SRAM |
|---|---|---|
| `0x0080E842` | `lcd_write_cmd` | `70 4c bd 7f 7e 3f 9f 63` |
| `0x0080E8D8` | `lcd_write_data` | `c3 dd 38 ea cc 42 01 dc` |
| `0x00811C7C` | `gpio_write` | `3d 75 f0 9e a8 1b f9 f5` |
| `0x008072E4` | `delay_ms` | `1e 65 fc 74 b9 82 60 c7` |

Everything above roughly `0x00806000` is uniformly random: entropy 7.96 bits
per byte, 50.08% ones, no repeated 16-byte block in 256 KB, and 0.0% of words
in the driver pages are pointers to anywhere plausible. The dump is faithful
(see the vector table and stack above); the memory is simply empty. Which is
what it should be — in bootloader mode the application has never run, so
nothing has installed its drivers.

**So the "call the ROM routines" route is dead for payloads.** Anything that
jumps to those SRAM addresses from a payload is jumping into uninitialised
memory.

### SETTLED, NEGATIVE: the driver blob is not in the ROM either

Same delta search as §7d.2/7d.3, now over the ROM: 251 known entry points
against 2117 prologue sites. Best delta scores 15/251 against a noise floor of
13–14. Nothing. The SRAM drivers are not stored verbatim in the mask ROM any
more than they are in flash, so they are assembled, decompressed or fetched at
runtime by the application — still unexplained.

### What the ROM does contain

- **No LCD driver.** `0x400D9000` is never referenced. The boot ROM does USB,
  storage and clocks; the display belongs entirely to the application. So the
  LCDC route (§12b) has to go through the register map, not through a ROM
  call.
- **A 52-register system controller at `0x40080000`**, used from `+0x00` to
  `+0x120` on a 4-byte stride, 264 references. This corroborates §7c: it is
  the clock/reset unit, and the application's LCD path only ever touches four
  of those registers.
- **A storage controller at `0x400F1000`**, registers `+0x00`–`+0x5C` — the
  sibling of the `0x400F7000` block the flash bootloader drives.
- `0x40030000`, `0x40040000` and `0x40020000` are addressed as absolute
  16-bit registers rather than base+offset, so they need a different scan.

- **GPIO — FOUND.** (Corrected 2026-08-07; the paragraph that used to be here
  said it was not in the ROM. It is, and the earlier scan missed it because
  the ROM does not address GPIO as base+offset from a literal: it looks the
  bank base up in a table.)

  `0x00000902` splits a packed pin id into a bank base and a pin index, using
  a **six-entry table at `0x00065004`**:

  ```
  0x40081000  0x40081040  0x400F6080  0x400810C0  0x40081100  0x400F6000
  ```

  Pin id: bank `[19:16]`, pin `[15:11]`, pad function `[10:7]`, initial output
  level `[6]`, drive `[5:4]`, pull selector `[3:0]` (indexes a 12-entry table
  at `0x0006501C` from 4). Function 0 is input and 1 is output — ROM `0x718`
  sets function 1 and then drives the pin, which pins that down.

  Registers, offsets from a bank base:

  | Off | Role | ROM |
  |---|---|---|
  | `+0x000 + (pin>>3)*4` | function, 4 bits per pin | `0x6F0` |
  | `+0x010` | input data | `0x97C` |
  | `+0x014 + (pin>>4)*4` | drive, 2 bits per pin | `0x76A` |
  | `+0x024`, `+0x02C` `+ (pin>>4)*4` | pull, 2 bits per pin each | `0x736` |
  | `+0x034` / `+0x038` | set / clear output bit | `0x6DE` |
  | `+0x200 + (pin>>3)*4` | interrupt mode, 4 bits per pin | `0x78C` |
  | `+0x210` / `+0x214` | interrupt enable / pending (W1C) | `0x7D0`, `0x7BC` |

  Entry points: `0x93C` configure a pad from a full id, `0x99E` set level,
  `0x97C` read level, `0xAAE` set function only. The bootloader's thunks are
  at `0x008207B0` and `0x00823C68`..`0x00823C84`.

  Reimplemented in `cores/sl6806/sl6806_padctl.[ch]`. The `ubfx rX, rX, #11,
  #6` at `0x0041DC` really was a false lead — it is in the SD/MMC driver — but
  the field width it suggested was close: the pin field is `[15:11]`, five
  bits, with the bank in the four above it.

### What this changes

The ROM is resident and callable from a payload by construction — the flash
bootloader already makes 393 such calls. That is the remaining constructive
use of this dump: ROM entry points are stable addresses, and their signatures
can be recovered from the bootloader's call sites. What the ROM cannot give
is the display, because it has no display driver.

## 8. LVGL — confirmed **v8.x**

- Smoking gun: build path `...\spark2-p4\src\gui8\lvgl\src\core\lv_disp.c`.
- v8 discriminators: `lv_timer_*` present / `lv_task_*` absent; `lv_disp_*` present /
  `lv_display_*` absent; `src/extra/` + `src/hal/` present (both removed in v9).
- Widgets compiled in: btnmatrix, canvas, dropdown, img, label, roller, textarea;
  plus flex layout and chart. (No btn/bar/slider/table.)
- **Two code clusters:**
  - Core/widgets: `0x00D1F9A4` .. `0x00D3E34C` (71 fns)
  - Refresh/render: `0x00C3D7AC` .. `0x00C40AD4` (4 fns) — lines up with the
    `_cpu1_lcd_notify` / dual-core LCD split **(inferred)**.
- **Key addresses:**
  - `lv_init`            `0x00D21C20`
  - `lv_timer_handler`   `0x00D31F88`
  - `lv_label_set_text`  `0x00D380AC`
  - `lv_disp_get_scr_act``0x00D1F9A4`
  - `lv_refr_area`       `0x00C3D908` (also `_lv_disp_refr_timer`)
  - **`lv_lcd_init`      `0x00D3E34C`  ← vendor display porting layer (highest-value target)**
- Labeling works 100%: `lv_*` `__func__` strings resolve to function starts (75 fns,
  no collisions after alternate-name handling). xframe handlers: 16 fns.

## 9. Other components

- **FFmpeg / libav** embedded for media decode (encoder tags `Lavf58.35.101`,
  `Lavf59.34.101`).
- Full **Bluetooth** (Classic + LE) stack.
- Custom **xfont** layer wrapping LVGL fonts, glyphs from the FONT partition.

## 10. Ghidra setup (12.x)

- Needs **JDK 21** and **Python 3.9–3.14**. Ghidra 12 default Python engine is
  **PyGhidra (CPython 3)**, not Jython — launch via `support/pyghidraRun`.
- Import Raw Binary / `ARM:LE:32:Cortex` / base `0x00C00000`; answer "No" to
  auto-analyze, run the label script first, then optionally auto-analyze.
- Label script: **`sl6806_label_all_v2.py`** (Python 3; defines the structs, labels
  the scene registry, renames LVGL + xframe functions, marks PICS/FONT as data).

## 11. Simulators for testing custom code

- **Unicorn Engine** — best fit: map flash @0x00C00000, RAM @0x00800000, stub/log
  MMIO, run individual functions. Good for unit-testing (e.g. the CRC, a menu build).
- **Renode** — whole-system, model peripherals incrementally in a `.repl`.
- **Ghidra emulator/Debugger** — p-code stepping inside the DB.
- **QEMU** — Cortex-M4 works but no SL6806 machine (write device models yourself).
- Workflow: unit-test in Unicorn → integration test on real HW with mask-ROM recovery.

## 12b. The LCD controller at `0x400D9000` — the way past the RAM wall

The application drives the LCD from SRAM-resident code, which is where the
previous analysis stopped. **The HLKJ bootloader initialises the same
peripheral, and it *is* stored verbatim in flash**, so it disassembles.

The identification is not circumstantial: the bootloader prints the function's
own name. `HAL_lcdc_module_init` (string at `0x0082E171`) is logged by the
routine at `0x00829A28`, whose first act is to cache `0x400D9000` in a driver
struct at SRAM `0x0082EE80`. Every other function reaches the controller
through that cached pointer, which is why the base appears as a literal only
twice in the whole image — and why a literal scan never found it.

Corroboration that this bootloader path is the LCD path: it clock-gates
through exactly the CRU registers the application's LCD code uses, it enables
the same interrupt (74), and it builds its command list with the same magic
words as the application's `lcdc_set_descriptor`.

Register map (offsets from `0x400D9000`), read out of the bootloader:

| Off | Role |
|---|---|
| `+0x00` | four 4-bit fields at [15:12] [11:8] [7:4] [3:0], all 9 for the LCD |
| `+0x04` | flag bits 1, 2, 3, 5, 6 from the config struct |
| `+0x08` | control; bit 31 then bit 30 are the two soft resets (`0x008299C4`) |
| `+0x10` | status/mask; start sets bit 22, clears [16:8], 31, 19 |
| `+0x14` | interrupt flags, write-1-to-clear; `0x70000000` = transfer busy |
| `+0x20` | geometry/format: [3:2] [9:8] [11:10] [15:12] [21:20], bit 4 |
| `+0x24`, `+0x2C` | transfer parameters |
| `+0x28` | bits [15:0] = length − 1 |
| `+0x40`, `+0x44` | two words packed from a 16-entry nibble table |
| `+0x80` | start: bit 0, plus mode bits at 2, 6, 7; [11:8] = 0xF |
| `+0x84` | bit 0 — the trigger |
| `+0x88` | **command-list address** |

**That answers the old open thread.** The handoff is: build the descriptor,
store its address to `+0x88`, set `+0x80` bit 0, set `+0x84` bit 0.

### RESOLVED: it is a QSPI display, and the driver is in flash

The observation that closes this section: the bootloader's command writer at
`0x00821BB2` sends **four bytes for every DCS command** — `0x02`, `0x00`,
`cmd`, `0x00`. That is the standard QSPI display command frame: a one-lane
`0x02` write opcode and a 24-bit address whose middle byte is the command.
Pixel data follows behind a `0x32` four-lane opcode (`0x00827D58` sends
exactly `lcd_write_cmd(last=1, 0x32, RAMWR)` before every frame).

Everything unexplained falls out at once:

- the panel descriptor's `+0x0C = 0x32` and `+0x0D = 0x02` are those two
  opcodes, not "50" and a device id;
- the command list's `a` field is the opcode and its `b` field is
  `cmd << 8` — the DCS command inside the address. The old note could not
  settle this because it compared the bootloader's descriptor against the
  *application's*, which has an extra `x_offset`/`y_offset` pair at `+0x04`
  and so numbers every later field four higher. Aligned, the bootloader's
  `+0x0D`/`+0x0E` are CASET and RASET, exactly as the window records need.

And the whole byte-level driver is in the bootloader, in flash:

| Address | Role |
|---|---|
| `0x008218D4` | send one byte of a command frame, out of `+0x24` |
| `0x00821940` | send data bytes out of the FIFO at `+0x30` |
| `0x00821B88` / `0x00821B9C` | assert / release chip select (`+0x20` bit 18) |
| `0x00821BB2` | `lcd_write_cmd(last, opcode, cmd)` |
| `0x00821C00` | `lcd_write_data(last, byte)` |
| `0x008219A4` / `0x008219FC` | the read paths, out of `+0x34` |
| `0x00829BC8` | the DMA transfer setup |

More of `+0x20` than the table above showed: bit 0 start, bit 1 write, bits
`[3:2]` source (1 = `+0x24`, 2 = FIFO, 3 = both), bits `[9:8]` and `[11:10]`
the two command-field lengths, bit 17 emit-a-frame (cleared for RAMWR so the
pixel stream continues), bit 18 hold CS. `+0x14` bit 31 is transfer-complete,
write-1-to-clear, and bits `[30:29]` are busy.

Transcribed into `cores/sl6806/sl6806_lcdc.c`, polled rather than DMA, with
195 host tests in `tests/host/test_lcdc.c` against a model of the controller.
**Not yet run against a panel.**

The command list is built by `0x00827E18` (bootloader, signature
`(x0, x1, y0, y1)`) / `0x00D3E728` (application). It is a sequence of
variable-length records terminated by `0xFFFFFFFC`, each shaped

```
<tag> <length-1> <a> <b> [inline payload] 0xABAB0005
```

For a windowed pixel write: two records carrying the column and row windows,
byte-swapped into big-endian pairs exactly as CASET and RASET want, then one
record whose length field is the pixel count − 1. The length reading is
pinned down by the window records, whose length word is 3 for a 4-byte
payload.

The tags are structured: `0xCDCD_(0x0A + 8*(type−1))_03` for a transfer up to
`0x10000` elements, `0x8A + 8*(type−1)` for the long form, where type is the
interface type 1–3. Over `0x10000` elements the builder takes a second branch
emitting a longer list, so that is the per-record limit.

The `a`/`b` fields are the QSPI opcode and `cmd << 8`; see above. **Still
undecoded:** the tag's low byte (3 for a record carrying a command, 2 for the
continuation record of a long transfer) and `0xABAB0005` itself.

The 20-byte config struct passed to `HAL_lcdc_module_init` **is** decoded,
from `0x00827FAC` and the application's `0x00D3E944`, which fill it
identically except for the colour mode:

| Byte | Value | Register |
|---|---|---|
| 0 | 1 | `CTRL` bit 0; bit 3 for type 2 |
| 1..4 | 9 | `CFG0` `[15:12] [11:8] [7:4] [3:0]` |
| 5 | descriptor colour mode | `+0x20` `[21:20]` |
| 6, 7 | 0, 3 | `+0x20` `[15:12]`, `[11:10]` |
| 8 | 0 | `+0x20` bit 4 |
| 9, 0x0A, 0x0B, 0x0C, 0x0D | 0, 0, 1, 0, 1 | `CFG1` bits 1, 5, 6, 2, 3 |

Full map in `cores/sl6806/sl6806_lcdc.h`.

## 12. Next actions (pick up here)

1. ~~Trace `lv_lcd_init`~~ **DONE** — §7b. Panel is 240x296 at offset (0,12),
   MIPI DCS, and the full init sequence is recovered and in the tree.
2. ~~Find the LCDC~~ **DONE** — §12b. `0x400D9000`, driver readable in the
   bootloader, descriptor handoff understood.
3. ~~Finish the FIRM/SD-update header~~ **DONE** — §6.
4. ~~Decode the LCDC command-list opcodes~~ **DONE** — §12b. The panel is a
   QSPI display; the `a`/`b` fields are the QSPI opcode and the DCS command.
   The whole byte-level driver turned out to be in the bootloader and is
   transcribed into `cores/sl6806/sl6806_lcdc.c`.
5. ~~Dump the mask ROM~~ **DONE** — §7f. It handed over GPIO (the pad
   controller, via a bank table the earlier scan missed) but not the LCD
   writers, which are in the bootloader instead.
5b. **Run the display on hardware.** This is now the top of the list. The
   stack is complete and tested against a model, and two things in it are
   still guesses: `+0x20` bits 17 and 18, inferred from where the vendor sets
   and clears them, and the pixel byte order, which the firmware does not
   settle because the vendor's framebuffer comes from LVGL. See docs/LCD.md.
6. ~~**Find the PLL**~~ — largely settled in practice. The clock was
   **measured at 64.000 MHz** (2026-08-06, one P20 Player) by timing the
   device's counter against the host's with `tools/sl6806-calibrate`; the
   bracket was ±0.06% and contained exactly one whole MHz. Finding the PLL
   would still give it exactly and for any unit, and it is *not* at the CRU
   base: `0x40080000` has dividers but no multiplier.

   Two hardware facts fell out of that measurement and are worth recording:
   the **DWT cycle counter does not run** on this part (its register reads a
   constant), so timekeeping uses the **24-bit SysTick**, which wraps every
   262 ms at 64 MHz.
7. **A DMA driver.** The display pushes pixels 16 bytes at a time because
   that is the FIFO depth. The vendor uses the DMA controller at
   `0x40070000`: `0x00829768` allocates a channel, `0x008217F0` starts a
   transfer into the LCDC FIFO, and `0x00827C5C` is the completion handler
   that chunks a frame. That is the whole path, and it is readable.
8. **Enumerate vtable slots / message-id enum** via the central
   `__act_on_request` dispatcher (turns `method_20` + siblings into named
   methods across all scenes).
9. **Build a Unicorn harness** to execute functions from the image.
