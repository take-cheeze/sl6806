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
