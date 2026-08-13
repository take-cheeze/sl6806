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

### 1a. Vendor identity, and why there is no datasheet

Searched 2026-08-13. **There is no public SL6806 datasheet**, in English or Chinese:
no entry on alldatasheet / DigChip / the Chinese 规格书 aggregators, no pinout, no
package drawing. Searching the part number finds only unrelated `*6806` parts and
`ilyakurdyukov/smartlink_flash`, which remains the sole open-source technical
reference and itself links no documentation. Assume none will appear; the mask ROM
is the datasheet.

The vendor is very probably **珠海绅聚科技 / Shenju Technology** (English site brands
it "Jointbees", which is the label `smartlink_flash` reports for these players).
Founded 2018, HQ Zhuhai, R&D in Shenzhen and Xi'an; `shenjugroup.com`. In March 2022
they announced the **云P3 ("cloud P3")** chip — matching the firmware's `yp3_` version
strings — as a dual-core AI SoC for MP3 players, in mass production at 1M+ units.
The part number mapping 云P3 → SL6806 is **(inferred)**: no public text states it.
Nothing register-level is available from them, and the P3 line has since been dropped
from their product page (current catalogue is WS310 / W30 / W20 / TWS300 / WS300A);
the announcement survives at `shenjugroup.com/zh-CN/news/82.html`.

Two claims in that announcement corroborate work elsewhere in these notes, and are
worth having on record as *independent* of the dump:

- "主控与蓝牙功能一体" — the Bluetooth is **on-die**, not a companion part. That is
  the strongest outside evidence that the `0x400E2000` window (§16, Bluetooth, and
  its gate in §17) is a real link controller rather than a host-side HCI transport,
  and that its counters are supposed to run.
- 192 kHz / 24-bit playback, full-format decode, and ENC-denoised recording with VAD.
  Consistent with the audio block's shape at `0x40009000` (§16, audio controller —
  note the duplicated section number — and §21): two DMA directions,
  three microphone inputs, a 128-word EQ coefficient RAM. A path this capable is not
  expected to be fused off, which further isolates the "configured and not running"
  failure to a missing functional-clock bit rather than to a missing feature.

The one route left to real documentation is asking Shenju directly through the
contact form on `shenjugroup.com` — possibly under NDA, possibly not at all. No SDK
leak exists on the Chinese board-house forums as of this search.

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
- **SRAM: `0x00800000`–`0x008FFFFF`, one megabyte.** Measured on hardware in
  bootloader mode (2026-08-07): `0x008FFF80` returns data, `0x00900000` and
  everything above it returns zeros, no address below `0x00900000` aliases any
  other, and re-reading an address returns the same bytes. The old figure of
  256 KB was the size of the first `tools/sl6806-dumpram` run, not a measurement.
  The stock firmware relies on this: its LVGL framebuffer is at `0x0087B800`
  (§13). So does this framework now — `ld/sl6806_payload.ld` gives a payload's
  `.bss` and `.heap` everything up to `0x008F0000`, because only the *loaded*
  bytes have to fit in the ROM's 64 KB window. That is ~650 KB of heap where
  there used to be 38, and it is what makes a full 240×296 framebuffer
  (138 KB) possible at all. `examples/BigBuffer` pattern-tests the region
  before using it, with an address-derived pattern so an aliased region fails
  rather than passing by luck.
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
4/12, 4/13 the TWI0 pins the sensor answers on (§7h); §7n names the other
eleven pin by pin, out of the driver that muxes them; bank 3 pins 1-6 are
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
which is what the name says. §7n's DVP block reads that same 1280x720 as its
input size, from an entirely different driver - two independent confirmations
of the array.

**Both tables are extractable and now are.** `tools/sl6806-sensortab` walks
them, resolves the `0xF0` paging and emits a C header; the crop decode above
is reproduced by the tool from the bytes rather than transcribed, which is
what checks it. `examples/CameraDemo` carries the result and replays it into
the sensor after a successful chip id, at the writer's own 40 us pacing. A
payload has to carry them: it runs from SRAM in bootloader mode with no XIP
mapping, so it cannot read its own flash at `0x00C7DA51`. After the terminator the routine optionally
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

> **RETRACTED — the callee *is* in the flash image, and §7m disassembles it.**
> The reasoning below is kept because its failure is instructive and because
> two of its four searches are still valid. What it got wrong is one number:
> it took the FIRM header's `0x00804C00` as the segment's *load* address when
> that word is the *entry*. The segment opens with a 256-entry vector table,
> `0x400` bytes, and the reset handler follows it — so the load address is
> `0x00804800`, which is exactly what §13a says. Disassembled `0x400` lower,
> `0x00804E44` is `push {r4, r5, r6, lr}` and the register file falls out in
> nine instructions. `tools/sl6806-sram --check` scores the three candidate
> bases against every SRAM address the XIP code calls: 10 prologues at
> `0x00804800`, none at either neighbour.
>
> The general lesson is the expensive one. Every search below took "the code
> is not at the offset I computed" as evidence about the *image* rather than
> about the *offset*, and then five further searches inherited the premise
> without re-deriving it. A negative that rests on one unverified constant is
> only as strong as that constant.

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

### ~~SETTLED, NEGATIVE: the SRAM library is in none of the three dumps~~ — half right

**Half of this stands and half of it is the same wrong premise.** What stands:
many of the addresses `sl6806-ramcalls` reports really are four-byte veneers
branching into the mask ROM, and §13's check #5 later confirmed it from the
other side. What does not: the conclusion that therefore *no* static search
could find *any* of them. `0x00804E44` and `0x00804EAC` are not veneers, they
are ordinary functions, and the reason they read as noise below is the `0x400`
offset error above — not the run-time-written-target argument, which is true
of different addresses.

Read the spacing evidence again with that in mind. `0x008051E8`, `0x008051EC`,
`0x008051F0`, `0x008051F4` are 4 bytes apart and *are* veneers — `0x008051F4`
disassembles as `b.w 0x93C`, straight into the ROM. `0x00804E44` and
`0x00804EAC` are 104 bytes apart, which was never veneer spacing, and nothing
here noticed that the argument did not apply to the two addresses the whole
section was about.

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
| chip id | the driver compares **`0x5808`**, at `0x00D3D988` - see the caveat below |

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

**The vendor's id check is a presence check, not an identity check.** It takes
bytes 8 and 9 of the ten-byte sequential read - register `0x0E` - and those are
state-dependent: `0x5808` on a chip fresh out of reset, `0x0000` once
`examples/FmDemo` has enabled and tuned it, both observed on this unit. The
part's real identity is register `0x00`, read by random access at address
`0x11`, and on this board it is **`0x5804`**.

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

RSSI stays at the noise floor - 0 to 5 out of 127 - and, importantly, **its
peaks do not reproduce between runs**. A 500 kHz sweep of the Japanese band
showed 77.5, 79.0, 82.0 and 88.0 MHz standing out of zeros, which looked like
stations; re-running at 100 kHz put nothing at those frequencies and scattered
2s elsewhere. So the structure is noise, not reception, and the aerial - which
on this device is the headphone lead - has not been connected during any run so
far. The tuner is demonstrably being driven; that it can *receive* is still
unproven

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

~~**But it is reachable, and cheaply, by one runtime read.**~~ The plan was to
read the veneer at `0x00804E44` off a running system over SWD, on the reasoning
that its target is written at boot and only the mask ROM could write it. **No
device was needed: `0x00804E44` is not a veneer and was in `dump.bin` all
along.** §7m has it.

**Still true, and independent of all this:** the camera's two enable writes at
`0x00D44EA6` and `0x00D44F06` - registers `0x03` and `0x16` of the file - and
the clock driver's use of `0x03` and `0x2C`. Those come from the drivers' own
code, and §7m confirms every one of them against the accessor.

**One thing this section got right and did not know it.** "A ~130-entry
byte-wide indexed file reached by index rather than by address is the shape of
a PMU / analog register bank on a serial side-channel, not of an MMIO block."
That is precisely what it is, and it was written while the search was still
looking for an MMIO base.

## 7m. FOUND: the indexed register file is a chip on a serial bus

Everything in this section is from `dump.bin` alone. **None of it has been run
on hardware.** It is the last piece §7l said the camera was missing, and it
should be read as a decode awaiting a measurement, not as a result.

### How it was missed for five searches

One number. The FIRM header at `0x10010` carries `load = 0x00804C00`, and §7h
used it as the segment's load address. It is the *entry*: the segment at file
`0x10030` opens with a full 256-entry Cortex-M vector table — `0x400` bytes, 90
non-zero entries, every one an odd Thumb pointer into the segment, into XIP
flash or into the mask ROM — and the reset handler follows it. So the load
address is `0x00804800`, which is what §13a says in the course of explaining
something else entirely, and the two readings sat in this file contradicting
each other without either being checked against the other.

Disassembled `0x400` lower, `0x00804E44` is `push {r4, r5, r6, lr}`.

`tools/sl6806-sram` now maps both regions and `--check` prints the
discriminator. It is worth having because nothing PC-relative can tell the
candidates apart — shift the base and code and literals shift together — so
the test has to be an *absolute* reference. It collects every SRAM address the
XIP image `BL`s to more than once and counts how many begin a function:

```
26 SRAM addresses below the blob are BLed to from XIP code more than once
    load 0x00804400:   0 of  26
    load 0x00804800:  10 of  26   <-
    load 0x00804C00:   0 of  26
```

### The accessors

`0x00804EAC` read and `0x00804E44` write, both in the first stage, both plain
flash. Stripped of the mutex they take when the scheduler is up
(`0x008066AC` / `0x008066D8`, guarded by a byte at `0x0082BDB8` — a payload has
neither), they are one four-register mailbox at `0x400F7000`:

| Offset | Role |
|---|---|
| `+0x104` | command: `[31]` start/busy, `[30]` set on every transfer, meaning unknown, `[15:8]` register index, `[7:0]` target address |
| `+0x108` | byte to write |
| `+0x10C` | byte read back, in `[7:0]` |
| `+0x110` | status; the writer fails if `& 0xF000` |

```
write(idx, val):  [0x108] = val
                  [0x104] = 0x40000060 | (idx << 8)
                  [0x104] |= 0x80000000
                  spin while [0x104] & 0x80000000
                  return ([0x110] & 0xF000) ? 1 : 0

read(idx):        [0x104] = 0x40000061 | (idx << 8)
                  [0x104] |= 0x80000000
                  spin while [0x104] & 0x80000000
                  return [0x10C] & 0xFF
```

**`0x60` and `0x61`.** The low byte differs by exactly one between the two
directions, which is an 8-bit I2C address and its R/W bit: **device `0x30`**.
So the file is a separate chip on a bus this block masters — a PMIC, on the
evidence below — and that is why §7l's vote over `0x4xxxxxxx` literals could
not have hit it at any threshold. There was never an MMIO base to find.

`0x400F7000` is filed in §7c as the SD/MMC and SPI flash host. Not a
contradiction: this is one sub-block at `+0x100`, the same way §15b found
module gates for ids 96–127 at `0x400F1000 +0x20`. That base is busier than
§7c knew.

### The "clock channels" are voltage rails

§7h read `0x00CC769C` as `clk(channel, op, arg)` because the camera passes
2800 and 2800 looks like kHz. Channel 6's setter at `0x00CC7536` decodes its
argument as

```
1700..2450 mV  ->  (mv - 1700) / 50
2650..3300 mV  ->  (mv - 2650) / 50 + 16
```

— 50 mV steps from 1.7 V to 3.3 V in a five-bit field, with a gap in the
middle. That is an LDO and nothing else. **So the camera's `clk(6, 2, 2800)`
is 2.8 V, the standard supply for a sensor of this class, and §7l's guess that
this file "is exactly the kind that carries LDO enables" was right.**

The whole dispatcher, `op` in `r1`:

| op | Meaning |
|---|---|
| 0 | enable: `reg 0x2C |= 1 << (ch - 2)` |
| 1 | disable: `reg 0x2C &= ~(1 << (ch - 2))` |
| 2 | set voltage, per-channel register below |
| 3 | read voltage back, inverting the same scale |

| Channel | Voltage register | Scale |
|---|---|---|
| 2 | `0x0D` `[4:0]` | 1500 mV + n×100 |
| 3 | `0x0E` `[3:0]` | 2650 mV + n×50 |
| 4 | `0x03` bits 1/5/7 | a six-entry menu: 2700, 2800, 2900, 3000, 3200, 3300 |
| 5 | `0x79` `[4:0]` | the split 50 mV scale |
| **6** | **`0x7A` `[4:0]`** | **the split 50 mV scale — the camera's** |

Channel 4 is the odd one: it re-asserts its voltage on enable, and brackets
the change with bit 7 of `reg 0x1B`. Channel 4's menu lives in bits 1, 5 and 7
of `reg 0x03` (the vendor masks with `0x5D`), which is the *same register* as
the camera's field in bits `[4:3]` — two fields, one byte, each driver
preserving the other's. Anything writing this file byte-wise rather than
read-modify-write will break one of them.

### So the camera's power-up is six writes

Pads aside, `0x00D44B1C` plus the sensor driver's own two writes:

```
reg 0x2C  &= ~0x10                 rail 6 off
                                   5 ms
reg 0x7A   = (reg 0x7A & 0xE0) | 0x13     rail 6 to 2800 mV
reg 0x2C  |= 0x10                  rail 6 on
reg 0x03   = (reg 0x03 & 0xE7) | 0x08     camera field [4:3] = 01   (0x00D44EA6)
reg 0x16  |= 0x80                  camera enable                    (0x00D44F06)
```

`0x13` is `(2800 - 2650) / 50 + 16 = 19`. Every mask is the vendor's own.

**This is the answer the README's camera row predicted**: "what the vendor does
and a payload does not is switch a rail". It is that rail, and it is now three
writes away rather than behind an unknown address.

### What is implemented, and what would confirm it

`cores/sl6806/sl6806_regfile.h` and `.c` carry the mailbox, the five rails and
`sl6806_camera_power_on()`. `examples/RegFileProbe` reads the file and decodes
the rails without writing anything; `examples/CameraDemo` performs the six
writes before it goes near the bus.

The confirmation to look for, in order of how much it proves:

1. **The rail voltages read back as recognisable supplies.** `RegFileProbe`
   prints all five through the vendor's inverse scale. A wrong block cannot
   fake 1.8 V and 3.3 V out of bytes that had no reason to produce them, and
   this costs no writes at all. Do this one first.
2. **The writes stick.** Every write in `CameraDemo` is verified by reading it
   back. That is what turned §7l's wrong base into a clean negative instead of
   a mystery, and it is the reason a failure here will be legible.
3. **`0x68` answers.** Registers `0xF7` and `0xF8` reading `0xDA` and `0x4A` is
   the sensor, and would settle the camera question that §7h opened.

A fourth outcome is possible and worth naming in advance: the mailbox may be
gated off in payload mode, in which case every transfer times out and none of
the above means anything. `RegFileProbe`'s verdict says so explicitly rather
than printing 131 dashes and leaving it to be inferred.

### MEASURED, 2026-08-13 — the decode is right and the prediction was wrong

`examples/RegFileProbe` on a P20 in payload mode: **131 of 131 registers read,
zero timeouts.** The mailbox works from a payload.

**The rails are what confirm it**, read back through the vendor's inverse
scale:

| Channel | Register | Reads |
|---|---|---|
| 2 | `0x0D` | **1800 mV**, on |
| 3 | `0x0E` | **3300 mV**, on |
| 4 | `0x03` = `0xAE` | on (menu) |
| 5 | `0x79` = `0x19` | 3100 mV, on |
| 6 | `0x7A` = `0x19` | 3100 mV, on |

1.8 V and 3.3 V falling out of a five-bit field through a split 50 mV scale is
not something a wrong block produces by accident. The registers hold still,
too — eight samples of the eight busiest indices differ in one bit of `0x30`,
once. §7l's free-running `0x2C` was a different peripheral entirely.

**And the writes work.** `examples/CameraDemo` performed the rail sequence and
every one read back: `0x2C` `0x1F`→`0x0F`→`0x1F`, `0x7A` `0x19`→`0x13`, rail 6
reporting 2800 mV afterwards. This is the first time anything in this project
has written the register file.

**But the camera's bits were already set before any of it:**

| | Expected after `power_on` | Found cold |
|---|---|---|
| `0x2C` rail enables | bit 4 | `0x1F` — **all five rails on** |
| `0x03` `[4:3]` | `01` | `0xAE` → **already `01`** |
| `0x16` bit 7 | set | `0x93` → **already set** |
| `0x7A` voltage | `0x13` = 2800 mV | `0x19` = 3100 mV |

So the sensor already had power and both enable bits in a device that had done
nothing but run the boot ROM, and `0x68` was silent anyway. **That retires
"the rail is why the camera does not answer"** — the README's own prediction,
now falsified. Finding the register file is what made it falsifiable, which is
the value of having found it whatever the camera turns out to need.

One oddity worth recording: registers `0x70`–`0x76` read `05 1C 55 20 24 2E CB`,
byte for byte what `0x06`–`0x0C` read. Seven consecutive registers mirrored at
a `0x6A` offset is a shadow bank or a trim copy, and it will explain a
confusing write one day. The full cold dump:

```
index  00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F
 0x00  08 10 09 AE 21 00 05 1C 55 20 24 2E CB 63 6D 7C
 0x10  15 9B 5A 28 34 87 93 14 00 E5 03 41 23 6F 0F 60
 0x20  2F B9 00 00 44 FF 00 01 00 01 48 71 1F 00 00 00
 0x30  12 00 00 30 00 00 00 88 01 08 19 0A 1B 00 3F C0
 0x40  13 00 00 00 00 00 00 00 00 00 10 3D 00 07 00 00
 0x50  00 00 5A 00 DC DC 05 44 03 30 45 06 00 D6 01 00
 0x60  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
 0x70  05 1C 55 20 24 2E CB 00 00 19 19 44 00 00 00 00
 0x80  03 70 0A
```

**And a warning that is not boilerplate.** Rails 2, 3, 4 and 5 go somewhere,
and nothing in the dump says where. On a SoC that includes the core, the SRAM
a payload executes from and the USB PHY the console rides on. Neither sketch
touches anything but rail 6, and neither should any sweep — a rail hunt here
is not the same kind of experiment as a gate-bit sweep, because there is no
recovery from switching off the supply that is running the code doing the
switching.

## 7n. The camera front end — the pixel path is not undecoded any more

From `dump.bin` alone, and **nothing here has been run on hardware.**

Every earlier note in this file says the same thing about the camera's pixel
path: the sensor is reachable but "pixels leave the sensor on a DVP/CSI
parallel bus into hardware JPEG and H.264 encoders, and none of that is
decoded — the firmware reaches the front end as a named channel resource, not
as a register block." The named resource is real. It sits on an ordinary
register block, and the block is findable in four hops from a log string.

### The chain

| | |
|---|---|
| `0x00C7D473` | the string `-bind_dvp_channel CSI_CHAN failed` |
| `0x00D42834` | logs it, having called… |
| `0x00D44740` | …which muxes eleven pads and calls… |
| `0x00D98224` | …whose only MMIO literal is **`0x400E1000`** |

That is the whole search. `sl6806-xref` does the first hop and the rest is
reading. It was never hidden; nobody had followed the string.

### Bank 4 is the camera port, all sixteen pins

`0x00D44740` muxes eleven pads to alternate function 2 before it configures
anything. With the four §7h already had, bank 4 is accounted for end to end:

| Pins | Line | |
|---|---|---|
| 0, 1, 2 | DVP sync and clock — PCLK / HSYNC / VSYNC | [V] pads, [?] which is which |
| 3 | MCLK out to the sensor | [V] §7h |
| 4–11 | DVP data D0–D7 | [V] |
| 12, 13 | TWI0 SDA / SCL | [V] §7h |
| 14, 15 | sensor RESET and PWDN | [V] §7h |

Sixteen pins, one bank, one peripheral. §7g already had "bank 4 is the
camera", from a sweep that saw sixteen function-2 pads move together; what is
new is **which pin is which**, from the driver rather than from a pattern, and
the confirmation that the sweep's grouping was right. Eight consecutive data
lines and three sync lines is what a DVP bus looks like, and the vendor muxes
them in exactly that grouping.

The firmware never names pins 0–2 individually, so which carries PCLK is open.
A scope on a working stock firmware settles it in one capture: PCLK is the
fast one.

### The register block

`0x00D9817C` is `dvp_configure(geometry *)` and every write is a `bfi` into a
field, so the map is exact. Sizes are stored **minus one** and offsets as-is,
consistently, across all six registers.

| Offset | Field | |
|---|---|---|
| `+0x00` | `[11:8]` format (8), then bit 4 set, then bit 1 set | three separate stores, in that order |
| `+0x04` | `[11:0]` width−1, `[27:16]` height−1 | the sensor array: 1280×720 |
| `+0x08` | cleared, then bit 4 set | by the open routine |
| `+0x10` | `[11:0]` crop x | |
| `+0x14` | `[11:0]` crop y | |
| `+0x18` | `[11:0]` crop width−1 | |
| `+0x1C` | `[11:0]` crop height−1 | |
| `+0x20` | `[11:0]` out width−1, `[27:16]` out height−1 | after the scaler |
| `+0x24` | `[13:0]` (crop_w << 10)/out_w, `[29:16]` the same vertically | **there is a scaler** |
| `+0x40` | width−1 / height−1 again, from `0x00D98160` | [?] which stage |

The scaler is the part worth noticing. A 10-bit fractional step, so `0x400` is
1:1 and larger downscales — which is how a 1280×720 sensor feeds a 240×296
panel without software touching a pixel.

The geometry struct the routine reads is recovered field for field in
[`cores/sl6806/sl6806_dvp.h`](../cores/sl6806/sl6806_dvp.h).

### And the wall, which is §15's wall

`0x00D9A734` is `enable(id)`: `0x400E0000 |= 1 << id`, then 10 ms.
`0x00D9A74C` disables. `0x00D9A768` is a reset pulse, and it uses a **second
register, `0x400E0008`** — clear the bit, wait, set it. **The camera front end
is id 6.**

§15 recorded `0x400E0000` as dead from a payload and called it the same wall
as the backlight's. It is. But that negative was reached when the register was
known only as "the thing the application enables peripherals through" — no id,
no second register, and no peripheral downstream to check the result against.
All three of those now exist, so the experiment is no longer the unanswerable
"does this register take writes" but:

```
read 0x400E1004                     remember it
0x400E0000 |= (1 << 6), wait 10 ms
write a size to 0x400E1004, read it back
if it did not stick: pulse 0x400E0008 bit 6, try again
restore
```

A block that holds a written size is enabled; one that does not is not. That
is the read-back discipline from §7l, pointed at a register that has a
downstream witness for the first time.

### MEASURED, 2026-08-13 — the wall is down, and not where the firmware says

`examples/DvpProbe` ran the four mechanisms in order on a P20:

| | Result |
|---|---|
| as found | `0x400E0000` = 0, `0x400E0008` = 0, CRU `+0x118` = 0, the whole block reads zeros and drops writes |
| CRU `+0x118` module clock | the write sticks (reads back `1`) — and changes nothing |
| `0x400E0000` bit 6 | **the bit does not stick.** The register reads back 0 |
| `0x400E0008` bit 6 | likewise |
| **mask ROM module id 46** | **the block takes and holds writes** |

So §15's negative on `0x400E0000` is *confirmed* — it is genuinely dead from a
payload, and not for want of an id or a reset register. It is presumably how
the vendor's supervisor reaches the same gates from code the boot ROM has not
handed control away from. **The way in is a mask ROM module clock, id 46**,
exactly as the ADC turned out to be id 84 (§15b).

That is twice now that walking 0..127 and testing a downstream witness has
succeeded where reading the firmware could not. The ids are in neither the
ROM, the bootloader nor the application; the walk is the method.

### MEASURED, second run — the whole clock tree comes up, and `0x68` stays mute

The first bring-up enabled the block and its gate and produced an awake DVP
with no MCLK, because gates are not clocks. `dvp_open`'s *first* call, at
`0x00D98232`, is to `0x00D9A7FC`, and that is the clock:

```
0x00D9A7AC()                the request/ack walk
0x40080008 = 0xC0000C04     a PLL
poll until bit 28           lock
0x40080008 |= 0x10000       release it to the tree
0x4008011C = 0x31           media module clock, enabled, source 3
```

`0x00D9A7AC` walks ten request bits at `[9:0]` of `0x40000070`, each with an
acknowledgement at `[25:16]`: set the request, spin for the ack.

Measured with all of it applied:

| | |
|---|---|
| `0x40000070` | `0x3FF03FF` cold — **all ten already granted**, nothing to ask for |
| `0x40080008` | `0x801` cold → `0xD0010C04` after: **bit 28 set, it locks** |
| `0x4008011C` | `0x31`, as written |
| DVP block | `CTRL 0x812`, `INSIZE`/`OUTSIZE` `0x2CF04FF` — every write holds |
| `0x68` | silent, under all eight pad/MCLK combinations |

**`0x40080008` is a PLL, and it locks.** §7c and the README both say this unit
has no PLL multiplier — "the clock and reset unit at `0x40080000` holds
dividers and gates, not a PLL multiplier". There is a multiplier at `+0x08`
with a lock bit at 28, programmed by the vendor at camera-open time on a
running system. It is not the core's, but the negative was stated too broadly
and the README's caveat under the measured `F_CPU` should be read with that in
mind.

So the state of the camera is now: rail on at 2.8 V, both sensor enable bits
set, bus proven against the FM tuner in the same run, RESET and PWDN drivable,
front end awake and configured, PLL locked, media and camera module clocks
enabled, MCLK muxed — and the sensor does not ACK its address.

**Every remaining hypothesis is about the sensor rather than the SoC**, and
they cannot be told apart by asking `0x68` anything. The next experiment
therefore stops using I2C: the DVP sync and data pads are the sensor's
*outputs*, and a powered, clocked sensor drives PCLK whether or not anyone has
configured it. `examples/CameraDemo` now samples those pads as plain inputs
and sweeps MCLK's source and divider at CRU `+0x118` — the vendor's source 0
came from a register it had just tested for zero, which is weaker evidence
than it looked. Three outcomes, and they separate the hypotheses:

| Reading | Means |
|---|---|
| something toggles | fitted, powered, clocked — the problem is I2C-side, a different search entirely |
| static but pulled | fitted, no clock reaching it or held off |
| nothing at all | consistent with no module fitted |

### MEASURED, third run — and the reading that is not yet evidence

The scope pass and the sixteen-way MCLK sweep both came back completely flat:
all six sampled DVP lines at `0`, zero transitions, under every combination of
source and divider at CRU `+0x118`, and `0x68` never ACKed. The PLL had also
retained `0xD0010C04` across the power cycle — it reads back locked before
anything is written, so the clock tree stays up.

**That looks like an absent module and it does not yet establish one.** The
vendor's pad words for these eleven pins end in `0x30`: drive 3, **pull
selector 0**. Every pull sweep in this repository exercises selectors 4–15,
because the ROM's pull table is twelve entries indexed from 4 (§7f), so
selector 0 has never been characterised. If it is an internal pull-down, those
pads read `0` whatever is or is not attached, and the flat reading is the pad's
own doing rather than a fact about the board.

The bus pads were not trusted this way — §7h swept their pulls and timed their
charge decay before concluding they reach a fitted chip — and the DVP pads have
to be asked the same two questions:

| Test | If the line reaches a fitted device | If it reaches nothing |
|---|---|---|
| sweep pull selectors 4–15 | stays `0` — something off-chip holds it down | lifts on a pull-up |
| drive high, release, time it | decays, there is a load | holds >10 ms, like RESET does |

`examples/CameraDemo` now runs both on the sync and data lines before the bus
attempts. Until that comes back, "no module fitted" and "module fitted but
mute" are still both open, and the flat scope reading is not an argument for
either.

### CONFIRMED: the module is fitted and works — so this is a software gap

The camera works under the stock firmware on the unit all of the above was
measured on (2026-08-13). That closes the only remaining "is the hardware
there" question and kills the second hypothesis outright: the sensor is
fitted, powered and functional, and everything measured from a payload is a
statement about what the payload is not doing.

It also means the flat DVP lines are not evidence of an absent module. They
are either the pad's own pull selector 0 (untested, see above) or a sensor
receiving no clock.

### The pattern this fits, and it is the backlight's

§14a and §15: module id 68 makes the PWM's registers writable **and its
counter still does not run**. Registers and function are two different enables
on this chip, and a module id buys only the first.

Read the camera the same way and it fits without strain. Module 46 bought the
DVP its register file — which is why every geometry write holds and reads back
— and a block whose registers answer but whose logic is unclocked would do
exactly what was measured: accept `CTRL 0x812`, report it, and drive no MCLK.
**The functional clock is what `0x400E0000` carries**, and that register will
not hold a bit from a payload.

So the camera and the backlight are one problem, as §15 suspected for
different reasons, and the question is not "what enables the camera" but
**"what gates `0x400E0000` itself"**.

A register that will not hold a bit is usually not dead — it is a register in
a block that is gated off. Nothing here has ever tested that, because every
sweep has used `0x400E0000` as a *mechanism* and none has used it as a
*witness*. `examples/DvpProbe` now does: the clock tree, then module 46, then
a retry of the enable (the previous run tested it *before* 46 and never went
back), then all 128 module ids with `0x400E0000`'s own writability as the
test. If an id turns up, two peripherals come unstuck at once.

### RESOLVED — `0x400E0000` is not dead, it is gated, and its gate is a module clock

Measured 2026-08-13, and it is the answer to §15's wall:

```
cold                       0x400E0000 ignores writes, bit 6 will not stick
+ the clock tree           still no
+ sl6806_module_enable(46) DVP registers writable, as before
+ retry 0x400E0000         **bit 6 holds.**  0x400E0000 = 0x40
```

**So the order is the operation, again.** Registers first, function second:

```
sl6806_module_enable(46);      the module clock -> the peripheral's registers
sl6806_periph_enable(6);       0x400E0000 -> the peripheral's logic
sl6806_periph_reset(6);        0x400E0008, the vendor's pulse
```

The two ids are from different spaces — 46 and 6 for the same peripheral — and
`0x400E0000` is itself gated, so writing it from a cold chip does nothing. That
is what every previous sweep did, which is why §15 recorded the register as
dead from a payload. It was never dead; nothing had ever been awake enough
first to open it.

This is the third time the same shape has caught this project out: §15b's
shadow-then-gate-then-poll, §7l's read-back-or-it-did-not-happen, and now
registers-before-function. A peripheral on this chip needs *two* enables from
*two* register spaces in *one* order, and getting any of it wrong produces a
block that reads plausible values and does nothing.

**It should unstick the backlight too.** §14a's PWM has its registers through
module id 68 and a counter that has never run — the same signature exactly.
What is not known is which bit of `0x400E0000` is the PWM's; only the camera's
id 6 is documented (`0x00D98236`). The other 31 have to be walked, with the
PWM's counter as the witness.

`sl6806_module.h` carries both calls and the whole rule.

### The DVP pads, asked properly — and my flat reading was the pad, not the board

Measured with the full bring-up in place, functional clock included:

```
sync pin 0..2, data pin 4, pin 11
  pulls:  4:0 5:0 6:0 7:0 8:1 9:1 10:1 11:1 12:0 13:1 14:0 15:0   none:0
  hold:   high held >10ms, low held >10ms
```

**They lift.** Selectors 8–11 and 13 take every one of them high, so the
`0 0 0 0 0 0` the scope pass reported was pull selector 0 doing exactly what
was suspected, and that reading is void. It measured the pad's own
configuration, not the board.

What it does say: all five behave *identically to RESET* — no external pull
either way, and they hold charge for more than 10 ms. The bus pads, which
reach the FM tuner, decay in 1–2 µs onto their pull-ups. So nothing is
actively driving the DVP lines at the moment they were sampled.

**That is weaker than it sounds, for one reason worth recording**: the pad
tests run *before* the reset sequence, so the sensor may well be in reset
while they happen. A sensor in reset drives nothing, and a trace to a sensor
in reset is indistinguishable from a trace to nowhere. Re-running this after
the reset release, with a pull-up selector rather than selector 0, is what
would make it evidence.

### State of the camera after the functional clock

Rail on at 2.8 V, both sensor enable bits set, bus proven against the FM
tuner in the same run, RESET and PWDN drivable, PLL locked, media clock,
camera module clock, module id 46, **and `0x400E0000` bit 6 with the reset
pulse** — full geometry accepted and read back. The sensor still does not ACK
and the DVP lines still do not move.

So every register either vendor routine writes has been written, and the
front end still produces no sensor clock. The next question is whether the
block has registers *neither* routine touches: `configure` and `open` between
them write ten offsets, and a front end that generates MCLK has a divider for
it somewhere. `examples/DvpProbe` now maps the whole block, `+0x00`–`+0xFC`,
by writing ones and reading back — anything writable outside those ten is
where the missing control would live.

### RESOLVED by mapping: where the frames go

Writing ones into every word of the live block and reading back what stuck
(2026-08-13) found four registers **neither vendor routine touches**:

| Offset | Cold | Writable bits |
|---|---|---|
| `+0x30` | `0` | `0x0000000B` — bits 0, 1, 3 |
| `+0x34` | `0` | `0x00FFFFFF` — 24 bits |
| `+0x38` | **`0x00800000`** | `0x000FFFFC` — bits [19:2] |
| `+0x3C` | `0` | `0x000FFFFF` — 20 bits |

**`+0x38` is a buffer pointer into SRAM.** Its resting value is the SRAM base,
and the bits that take writes are exactly [19:2] — a 1 MB span at 4-byte
alignment, with the `0x008` on top fixed. A register that rests at the base of
the only RAM on the chip and can be moved anywhere inside 1 MB but not outside
it is a DMA destination and very little else. `+0x3C`'s twenty bits fit a
length over the same span, `+0x34`'s twenty-four a second address or a byte
count, `+0x30`'s three bits the control that starts it.

That is the question this section opened with, answered — by mapping a live
block rather than by reading code, which is worth noting because the code that
writes these registers is presumably in the encoder path nobody has located.

**The block is `0x80` bytes and aliases.** `+0x80`–`+0xC0` read back exactly
what `+0x00`–`+0x40` hold, including values this framework had just written, so
the upper half is the same registers decoded twice, not a second channel. The
map's "18 words outside the vendor's ten" is really four new registers and
fourteen aliases.

**And nothing in it looks like a clock divider.** Those four are an output
path, not an input one. So MCLK is owned by something outside this block, or
it only runs once the block is streaming — and the sensor cannot stream
without it, which would be a chicken-and-egg the vendor must break somewhere
that has not been read yet.

### The sensor's outputs are high-impedance — the first reading here that discriminates

Sampled against the vendor's own pull-up (selector 8), with the whole bring-up
in place and after the reset sequence: **all six lines read 1 and never move**,
under all sixteen MCLK settings.

Both earlier versions of this measurement were void — the first left the pads
on selector 0 and read the pad rather than the board. This one is not. A line
held at 1 by a pull-up is a line nothing is driving, so:

| | |
|---|---|
| the module is fitted | the stock camera app works on this unit |
| the rail is on | 2.8 V, read back through the vendor's scale |
| the sensor's outputs are Hi-Z | measured, against a pull-up |

A powered CMOS output is normally driving something. One that is
high-impedance is in power-down, or has its output drivers gated behind an
internal clock it has not got. Given the rail is on and both enable bits are
set, the clock is the remaining explanation — which is where this has pointed
since the LDO decode retired the power hypothesis.

### Two candidates left, both cheap

**The divider is four bits and only four values have been tried.** CRU
`+0x118` carries `[11:8]` as a divider — sixteen values — and every sweep so
far covered 0–3, which is the *source* field's range. Divider 0 may mean
stopped. It is what the vendor passes, but that argument comes out of a
variable the vendor had just tested for zero, and reading a guard's tested
value as a meaningful argument is the same mistake that produced "2800 kHz"
and "source 0". Sixteen dividers × four sources is sixty-four tries.

**MCLK may only run while the block is capturing.** The four registers the map
found are an output path, and nothing has ever told the block to start. If the
sensor clock is gated on the capture engine running, then configuring geometry
and walking away is exactly how to get a silent sensor. `examples/DvpProbe`
now points `+0x38` at a buffer in its own BSS, sets both length registers, and
tries each of `+0x30`'s three writable bits and all of them together.

Both are judged by the same witness: do the sensor's output pins move.

### NEGATIVE on both candidates — and then the ROM gave up a second clock family

Sixty-four source/divider combinations at CRU `+0x118` and all four writable
values of `+0x30` with the DMA pointed at a real buffer: **every DVP line
static at 1 throughout.** `+0x38` read back `0x00829D10`, the exact address
written, which does confirm the DMA-pointer decode even though it changed
nothing.

Going back to the ROM settled what the vendor's own routine does:

| | |
|---|---|
| ROM `0x1E54` | `module_clock_is_enabled(id)` — reads shadow *and* gate, returns whether both are set. A query |
| ROM `0x1EC0` | `b.w 0x1C5C` — an alias of `module_clock_enable` |
| ROM `0x1EC4` | `b.w 0x1CE8` — an alias of the disable |
| ROM `0x99E` | `set_level(id, val)` is `val != 0 ? 1 : 0`, so the vendor's `gpio_write(id, 0x40)` really is **high** — the RESET/PWDN polarity question is closed |

So `0x00D9A7FC` is exactly `if (!enabled(46)) { domains; PLL; enable(46);
media clock; }`, which is what this framework already does. The front-end path
was fully replicated and it is not enough.

### The half nobody had read: `sensor_init`

The camera bring-up is in two halves. `bind_dvp_channel` sets up the front
end, and this project had read only that. **`sensor_init` at `0x00D44CBC`
enables two more clocks before it touches the bus:**

```
0x00D44CD2   module_clock_enable(80)     via ROM 0x1EC0
0x00D44CD8   rom_clock_enable(58)        via SRAM veneer 0x008051EC
             then TWI0 pads, twi_init(0, 100k), power_on, chip id
```

**ROM `0x20EC` is a second clock family.** The veneer at `0x008051EC` branches
to it, and it is an 85-entry `tbh` table where each id maps to one CRU
register and one bit:

```
id 58  ->  0x400800C0 |= 1
id 59  ->  0x400800C4 |= 1        (and +0xC8, +0x104 nearby, same shape)
```

CRU `+0xC0` has never been written by anything in this repository, and neither
has module id 80. Together they are the only things the vendor does for the
camera that a payload has not — after the rail, the register clock, the
functional clock, the PLL, the media clock, the camera module clock and the
full geometry all came up correctly and the sensor stayed mute with its
outputs in high impedance.

**This also means the module-gate space is not the whole clock tree.** §15b
established four gate/shadow register pairs and 128 ids; ROM `0x20EC` is a
separate 85-id family on top of that, and no sweep in this repository has
touched any of it. That is worth more than the camera: every peripheral
previously written off as ungateable should be re-tested against it.

### The whole second family, extracted

`tools/sl6806-romclocks` pulls the table out of `maskrom.bin` and emits
[`cores/sl6806/sl6806_romclk.h`](../cores/sl6806/sl6806_romclk.h). Every
implemented arm is the same five instructions — load a base, load an offset,
OR one immediate, store, return — so the extraction is exact rather than
heuristic. **56 of the 85 ids are implemented**; the other 29 branch to the
default arm at `0x24BE`.

| Ids | Reach |
|---|---|
| 17–58, 61–75 | CRU `+0x80`–`+0x120`, bit 0 each — a per-peripheral clock enable window **this project had never written a byte of** |
| 0, 2, 8, 9, 41, 42, 53 | CRU `+0x30`, various bits |
| 71 | CRU `+0x1C` bit 16 |
| 60, 77–84 | `0x400F1000` `+0x00`–`+0x5C` |

Only two are attributed. **58** is the camera sensor's, from `sensor_init`.
**73** sets CRU `+0x118`, which `0x00D44688` also programs as the camera's
module clock — so the two families overlap, and a register reachable one way
is reachable the other.

That overlap is the useful part for anyone else: §15a read `+0x100`–`+0x13C`
as module clocks with source and divider fields, and ids 52, 54, 61, 63, 66,
72–75 set bit 0 of exactly those registers. The two decodes agree, which is
the first independent check either has had.

**MEASURED: enabling id 58 and module 80 changes nothing.** CRU `+0xC0` reads
back `0x1`, the module gate acknowledges, and the sensor stays mute with its
outputs in Hi-Z. So the vendor's own two halves are now both fully
replicated. `examples/CameraDemo` sweeps the remaining 55 one at a time,
pinging `0x68` after each — the enables accumulate, so an ACK bisects rather
than isolates, which is the cheap version of the right experiment.

### COMPLETE NEGATIVE — every clock mechanism applied, sensor still mute

All 56 ROM clock enables written one at a time with a ping after each: **no
ACK, and the device survived all of them.** With that, the state of the camera
from a payload is:

| | Measured |
|---|---|
| the bus | an FM tuner on those two pads returns its own driver's chip id, same run |
| the rail | 2.8 V, read back through the vendor's inverse scale — and already on cold |
| the sensor's enable bits | `0x03[4:3]` and `0x16` bit 7, also already set cold |
| the pads | both control lines drive; both bus lines pull |
| the module | fitted — the stock camera app works on this unit |
| the DVP | register clock, functional clock, PLL locked, media clock, geometry accepted and read back |
| the camera module clock | all 64 source × divider settings |
| the ROM's second family | all 56 enables |
| the sensor's outputs | **1 against a pull-up — high impedance, not driving** |

A fitted, powered sensor whose outputs are Hi-Z is one that has never started.
And every mechanism this chip is known to have has now been applied to it.

**So this is a complete negative, not a missing experiment.** The difference
between a payload and the working stock firmware is no longer expressible in
any register anyone has found — which means more probing of the same kind has
nothing left to vary.

### Where that leaves it

Two honest options, and they are of very different sizes.

**Observe the working configuration.** The only thing that would settle this
in one step is reading the CRU, `0x400E0000` and the DVP block *while the
stock camera app is running*, and comparing. §7d.1 records that `read_mem` is
refused in card-reader mode, so this needs SWD — the same conclusion §7l
reached about the register file, for the same reason, and that one turned out
to be answerable from the dump instead. This one may not be.

**Run it in the application's context.** `MODE=firmware` puts the same code
where the vendor's runs, which is the one variable that has never been
changed. It is also the only mode in this project that can leave a device that
does not boot, and the camera is not worth that to most people.

Everything else about the camera is done: the sensor is decoded register for
register, its init tables are extracted and replayable, its rail is
controllable, its front end is mapped and can be enabled and configured, and
where its frames would land is known. What is missing is one clock that
nothing in three binaries explains.

### Still missing for an image

~~**Where the frames go.**~~ Answered. What remains is MCLK, and it is not in
any register file this project can reach. Configure and open set geometry and enables and never
touch a destination address. So the output is either programmed elsewhere —
the encoders, or the DMA at `0x40070000` whose command-list format §12b
decodes — or consumed by the JPEG/H.264 blocks directly. That is the next
question and it is a different one.

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
- **And the literal scan has a blind spot worth knowing.** A block whose
  per-channel base is computed — one literal plus `ch << 5` — appears once
  and then never again, because every later access goes through a cached
  table. Both blocks in §14 hid that way, and so did the LCDC (§12b). If a
  peripheral is "missing", look for the table it is cached in.

**Blocks identified so far.** Doing the same literal scan over the *HLKJ
bootloader* rather than FIRM is what named most of these: the bootloader is a
small self-contained program that touches each peripheral once, so its uses
are legible where FIRM's are buried.

| Base | What | Evidence |
|---|---|---|
| `0x40000000` | pad / pin function mux | `0x00820650` rewrites four byte-lanes of `+0x04` (per-pad function nibbles) and a 5-bit field at `+0x08`, switching one bus between two functions |
| `0x40009000` | ~~timers~~ **the audio controller** | RETRACTED, see §16. Every word of the original evidence — "channels at 0x100 stride (`+0x108`, `+0x208`) with write-1-clear flags and per-channel callbacks; register triples at 0x20 stride" — is right, and describes this block's two DMA directions, their interrupts, and the three playback volume channels. The timers are `0x40099000`, as this same section says four subsections down |
| `0x40099000` | timers | FIRM's `HAL_timer_*`; named while eliminating candidates for the PWM |
| `0x40070000` | DMA **control only** | per-channel IRQ status at `+0x24`/`+0x2c`, callback table indexed by channel, request routing at `+0x00`/`+0x20`/`+0x28`. The data path is **not** here — see `0x40001000` below |
| `0x40001000` | **DMA channel registers** | 8 channels at `+ch*0x40`, `{ctrl, src, dst, len}`; §14b |
| `0x40084000` | **PWM** | 6 channels at `+0x20 + ch*0x20`; channel 3 is the backlight; §14a |
| `0x40080000` | **clock & reset unit** | dividers at `+0x40`/`+0x48` with a bit-31 busy poll; module gates at `+0x64`/`+0x74` bit 15; see `cores/sl6806/sl6806_cru.h` |
| `0x40081000` + `0x400F6000` | **GPIO / pad controller** | six banks, bases in a mask ROM table at `0x00065004`; see §7f and `cores/sl6806/sl6806_padctl.h` |
| `0x400D9000` | **LCD controller** | the bootloader logs `HAL_lcdc_module_init` from the routine that caches this base; see §12b and `cores/sl6806/sl6806_lcdc.h` |
| `0x400F7000` | ~~storage host (SD/MMC + SPI flash)~~ **SPI flash host; `+0x100` is the register-file mailbox** | PARTLY RETRACTED, see §23. The SPI flash half stands. The SD/MMC half does not: `+0x100`..`+0x110` are §7m's mailbox to the register-file chip, and the `sdio(...)` strings are in the bootloader (file < `0x10000`), not near this base. The SD host is `0x40003000` |
| `0x40003000` | **SD/MMC host** | §23. Written down once per image, into a driver handle (`0x00822958`, mask ROM `0x0003D398`); the driver itself is in the mask ROM. See [`sl6806_sd.h`](../cores/sl6806/sl6806_sd.h) |

## 7d. RAM-resident code — ~~explained: it belongs to the mask ROM~~ SUPERSEDED

> **This section's conclusion is wrong; see §13.** The SRAM-resident code *is*
> in the flash image, at file `0x03B430`, and the application's first-stage
> loader reads it in. The routines below (`0x0080E842`, `0x00811C7C` and the
> rest) all disassemble out of `dump.bin` today —
> `tools/sl6806-sram dump.bin 0x0080E842 --dis 160`. What survives from this
> section is the *enumeration* method and the call-site tables, which are
> still correct and still useful; what does not survive is "these belong to
> the mask ROM" and the three "ruled out" searches, which failed because the
> copy is a flash *read call*, not a memcpy.

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
memory. That part still holds — and §13 explains why the memory was empty:
those addresses are filled by the *application's* loader, which never runs in
bootloader mode. The difference §13 makes is that the code can now be read out
of flash and transcribed, exactly as the bootloader's LCD driver was.

### SETTLED, NEGATIVE: the driver blob is not in the ROM either

Same delta search as §7d.2/7d.3, now over the ROM: 251 known entry points
against 2117 prologue sites. Best delta scores 15/251 against a noise floor of
13–14. Nothing.

That result is correct — the drivers are not in the ROM. The inference drawn
from it, "so they are assembled, decompressed or fetched at runtime — still
unexplained", is the part §13 closes: they are **fetched at runtime, verbatim,
from flash**. The same delta search *over flash* did find them; it was
dismissed as a false positive. See §13 for why that dismissal was wrong.

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
  - Core/widgets: `0x00D1F9A4` .. `0x00D3E34C` (71 fns), running XIP.
  - Refresh/render: `0x00C3D7AC` .. `0x00C40AD4` — **not a dual-core split.**
    This is `lv_refr.c` + `lv_draw_label.c` *stored* in flash and *run* from
    SRAM: it is inside the blob §13 describes, and its run addresses are
    `0x00807F7C`..`0x0080B2A4`. Subtract `0x435830` from any address in this
    cluster to get the address the rest of the firmware calls it by.
- **Key addresses:**
  - `lv_init`            `0x00D21C20`
  - `lv_timer_handler`   `0x00D31F88`
  - `lv_label_set_text`  `0x00D380AC`
  - `lv_disp_get_scr_act``0x00D1F9A4`
  - `lv_disp_drv_register` `0x00D2F578`
  - `lv_disp_drv_init`   `0x00D2F52C`
  - `lv_disp_draw_buf_init` `0x00D2F55C`
  - `lv_disp_flush_ready` `0x00D2F7EC`
  - `_lv_disp_refr_timer` **SRAM `0x008080D8`** (stored at flash `0x00C3D908`)
  - `lv_lcd_init`        `0x00D3E34C`  ← vendor display porting layer
  - `lv_port_disp_init`  `0x00D3B928`  ← where the driver is actually filled in
  - LVGL flush_cb        **SRAM `0x0080B21C`** (stored at flash `0x00C40A4C`)
- Labeling works 100%: `lv_*` `__func__` strings resolve to function starts (75 fns,
  no collisions after alternate-name handling). xframe handlers: 16 fns.

**How the display is configured — see §13** for the whole path, the draw
buffer, the colour format and the byte order.

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
198 host tests in `tests/host/test_lcdc.c` against a model of the controller.
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
5b. ~~**Run the display on hardware.**~~ **DONE, and it works** (2026-08-07).
   `examples/GfxDemo` puts shapes and text on the glass in the right colours.
   The two guesses are confirmed by that: `+0x20` bits 17 and 18 are right,
   and so is the byte order §13d derived. The blocker was never the bus
   driver — it was the backlight (§14a), which nothing turned on, so every
   earlier "no picture" run was measuring a dark lamp.
6. ~~**Find the PLL**~~ — largely settled in practice. The clock was
   **measured at 64.000 MHz** (2026-08-06, one P20 Player) by timing the
   device's counter against the host's with `tools/sl6806-calibrate`; the
   bracket was ±0.06% and contained exactly one whole MHz.

   **FOUND, and the "not at the CRU base" claim was wrong.** It is at
   `0x40080008`. `0x00D9A7FC` writes `0xC0000C04`, spins until bit 28 (lock)
   is set, then sets bit 16, and only afterwards enables the first module.
   In bootloader mode the register reads `0x00000801` — stopped — which is
   why the whole `0x400E****` domain is dark there (§14a). The vendor writes
   `0x4008011C = 0x31` next; that looks like a clock-source select and has
   deliberately not been tried, because reparenting the core or USB onto a
   fresh PLL from a payload would end the session.

   What this does *not* yet give is the multiplier arithmetic, so the 64 MHz
   figure is still the measured one, not a derived one.

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
10. ~~**Re-run every "not in the image" search against the blob**~~ **DONE**
   for the two that mattered — §14. The PWM (backlight) is at `0x40084000`
   and the DMA data path at `0x40001000`.
11. **Turn the backlight on.** §14a gives the registers and the vendor's own
   period and duty. This is now the top of the list: it is a handful of
   register writes, it does not wedge USB the way a pad sweep does, and it is
   the cheaper of the two explanations for a dark panel.
12. **Find the bulk path's byte swap** (§14c). The remainder path swaps in
   software; the DMA path must be getting it from a mode bit somewhere.
13. **Read `0x00D3D094`** to get the device-record layout, which would settle
   whether the PWM record's `0x00030000` really is the backlight's pad.
14. **Run `examples/BtProbe` on hardware.** §16 found a single, exclusively
   referenced candidate peripheral base for Bluetooth, `0x400E2000`, from
   the code next to the application's HCI command dispatcher — but nothing
   in it has been read on a real device yet. See docs/BLUETOOTH.md.

## 13. The SRAM-resident half of the application is in flash — and LVGL

Everything in this section came from `dump.bin` alone, except the memory-size
measurement, which was taken on hardware in bootloader mode.

### 13a. The blob: where the "missing" drivers live

`tools/sl6806-sram` prints the mapping and re-derives it from the image:

```
loader pool at file 0x010F30
  reads flash file 0x03B430 .. 0x052EF4  (0x17AC4 bytes)
  into SRAM       0x00805C00 .. 0x0081D6C4
  sram addr  = file offset + 0x7CA7D0
  flash addr = sram addr   + 0x435830
```

The boot ROM loads only the first-stage from the FIRM header (file `0x10030`
→ `0x00804800`; the reset handler is at `0x00804C00`, which is why the header
names that address). The first stage then calls the flash reader at
`0x00805A30` with those four literals — destination `0x00805C00`, length
`end - dst`, source `0x00010030 + 0x0002B400` — and the remaining 94 KB of the
application lands in SRAM. Then it zeroes `.bss` from `0x0081D6C4` up to the
stack at `0x0082D63C` and calls `0x00805240`.

**Why three previous searches missed it.** §7d looked for a *memcpy*: a copy
loop, or an aligned linker-style copy table. There is neither — the transfer is
a flash read call, the same one the file system uses, and its arguments are
four ordinary words in a literal pool. §7d.3 *did* find the right answer as its
best delta ("RAM `0x00800000` to file `0x035830`", 91/251 entry points against
a noise floor of 25) and rejected it on the grounds that the matched region
begins exactly where the 157 KB zero run ends. That is not evidence of a false
positive — a loaded image obviously starts where the padding stops, and a 3.6×
margin over the noise floor was the signal, not the artefact.

**Four independent confirmations**, any one of which is sufficient:

| # | Check |
|---|---|
| 1 | The loader call itself: literals `0x00010030`, `0x00805C00`, `0x0081D6C4`, `0x0002B400` at file `0x010F30`, consumed by the call at SRAM `0x008056B2`. |
| 2 | `_lv_disp_refr_timer` is registered with `lv_timer_create` at SRAM `0x008080D8`; the `__func__` string `"_lv_disp_refr_timer"` is loaded at flash `0x00C3D956`. Difference: `0x435830`, to the byte. |
| 3 | The panel descriptor. §7b already recorded "in flash at `0x00C519FC`, copied to SRAM `0x0081C1CC`" — difference `0x435830`. It was never a special-case copy; it is one datum inside the blob. `lv_lcd_init` stores `0x0081C1CC` into its ops table. |
| 4 | Semantics. `0x0080E842` now disassembles, and it is a structural twin of the bootloader's `lcd_write_cmd` at `0x00821BB2` — same busy poll on `+0x14 & 0x60000000`, same clearing of `+0x08` bit 4, same `+0x20` bit 18 for CS, and the same `cmp #0x2C` special case that clears bit 17 for RAMWR. |
| 5 | The GPIO entry points land on a table of one-instruction branches into the mask ROM, at exactly the addresses §7f named by number: `0x00811C78 → 0x97C` (read level), `0x00811C7C → 0x99E` (set level), `0x00811C90 → 0xAAE` (set function). Two independently-derived address spaces agreeing to the byte is not a coincidence a shifted base can manufacture. |

That fifth check is worth a second look, because it settles something else.
The application's GPIO layer is **nothing but thunks into the ROM pad
controller** — `0x00811C98` and `0x00811CB4` call ROM `0x902` to split the
packed pin id and then ROM `0x76A` (drive) and `0x736` (pull), the latter
indexing the 12-entry table from 4 exactly as §7f describes. So §7e's
tentative pin-id bit assignment is superseded by §7f's for good: the firmware
and `cores/sl6806/sl6806_padctl.h` are decoding the same field layout.

**What this unlocks.** Every address in §7d's "RAM-resident code" table is now
readable, including `0x00811C7C`/`0x00811C90` (GPIO), `0x0080E842`/`0x0080E8D8`
(the LCD writers), `0x0080E5C0` (the FIFO writer) and `0x0080B21C` (LVGL's
flush). §7f's "the vendor routines are not resident in bootloader mode" is
still true and still means a payload cannot *call* them — but it can now
transcribe them, exactly as `sl6806_lcdc.c` transcribes the bootloader's copy.

**One loose end.** The FIRM header declares `0x5862` bytes for the first stage,
which reaches `0x0080A062` and so overlaps the blob's destination. The loader's
own code sits below `0x00805C00` and survives, so the overlap is harmless, but
whether the ROM really transfers all `0x5862` bytes or stops earlier is
untested.

### 13b. LVGL: version and configuration

Version is **v8, pre-8.2**. Beyond the `gui8\lvgl` build path and the v8/v9
discriminators in §8, the struct layout dates it: `lv_disp_drv_init`
(`0x00D2F52C`) memsets `0x44` = 68 bytes, and `lv_disp_drv_register`
(`0x00D2F578`) tests **bit 0** of the bitfield at `+0x08` for `full_refresh`
and clears it with `bfc r3,#0,#1`. In 8.2 and later `direct_mode` occupies bit
0 and `full_refresh` moves to bit 1. Assert/log line numbers in
`lv_hal_disp.c`, useful for matching an exact tag against upstream: 103 and 116
(`LV_ASSERT_MALLOC`), 124 (the `full_refresh requires at least screen sized
draw buffer(s)` warning).

`lv_disp_drv_t` fields recovered from the code that writes them:

| Off | Field | Value here |
|---|---|---|
| `+0x00` | `hor_res` (u16) | 240 |
| `+0x02` | `ver_res` (u16) | 296 |
| `+0x04` | `draw_buf` | inside the config struct, `0x00827B64` |
| `+0x08` | bitfield | bit 0 `full_refresh`, bit 1 `sw_rotate`, bit 2 `antialiasing`, bits [4:3] `rotated`, bit 5 `screen_transp`, bits [15:6] `dpi` |
| `+0x0C` | `flush_cb` | `0x0080B21C` |
| `+0x24` | a callback slot | `0x0080B1EC`, installed only in mode 1 |
| `+0x3C` | `color_chroma_key` | `0x07E0` |
| `+0x40` | `user_data` | |

`lv_disp_drv_init`'s defaults are the stock ones — `hor_res`/`ver_res`
320×240, `antialiasing` set, `screen_transp` clear — with `dpi` = **60**
(upstream default is 130) and `sizeof(lv_disp_t)` = `0x158` = 344.

**`LV_COLOR_DEPTH` is 16 and `LV_COLOR_16_SWAP` is 0.** The chroma key settles
it: upstream defines `LV_COLOR_CHROMA_KEY` as `lv_color_hex(0x00FF00)`, which
is `0x07E0` with no swap and `0xE007` with swap. The firmware stores `0x07E0`.
So the framebuffer holds RGB565 as native little-endian `uint16_t` — low byte
first in memory. **This closes the gap §12.5b named** — "the pixel byte order,
which the firmware does not settle because the vendor's framebuffer comes from
LVGL". It comes from LVGL, and now we know what LVGL put there; §13d shows what
the driver does with it. docs/LCD.md had already reached the same conclusion
from the bootloader's single-pixel write; this is the independent confirmation
from the application side.

The driver is registered from **`lv_port_disp_init` at `0x00D3B928`**, called
once from the display bring-up at `0x00D08ED4` after `lv_init`. It takes a
config struct at SRAM `0x00827B60`:

| Off | Meaning | Value |
|---|---|---|
| `+0x00` | mode | **1** |
| `+0x04` | the `lv_disp_draw_buf_t` it fills in | |
| `+0x28` | `buf1` | **`0x0087B800`** |
| `+0x2C` | `buf2` | 0 — single buffered |
| `+0x30` | `hor_res` (u16) | 240 |
| `+0x32` | `ver_res` (u16) | 296 |
| `+0x34` | buffer height in lines | 296 |

So the draw buffer is `hor_res × 296` = 71,040 pixels = **138.75 KB at
`0x0087B800`**, one full screen, single buffered. Mode 1 installs the `+0x24`
callback; mode 3 would set `full_refresh`; mode 2 skips the whole thing.

A *second* draw-buffer object at `0x00827B3C` is re-initialised at runtime with
three geometries, under a `tbb` switch at `0x00D08D88` — 240×80 (19,200 px) and
80×296 (23,680 px) both at `0x0086B800`, and 240×296 (71,040 px) at
`0x0087B800`. Same panel, three orientations: this is the firmware's rotation
mechanism, and it is where to look if we ever want rotated output.

**The framebuffer address is why §3 had to be re-measured.** `0x0087B800` is
outside the 256 KB the notes assumed; the part has 1 MiB.

### 13c. The path a pixel takes

```
lv_timer_handler            0x00D31F88   XIP; its call site is 0x00D08E74
  _lv_disp_refr_timer       0x008080D8   SRAM (flash 0x00C3D908)
    ... render into 0x0087B800 ...
    flush_cb                0x0080B21C   SRAM (flash 0x00C40A4C)
      -> op dispatch        0x00D3E440   -> ops[+0x18] at 0x008298B8
        lcd_blit            0x00D3E2E4
          window write      0x00D3EBD4   (x, y, w, h, buf, flag)
            set window      0x00D3E518   CASET/RASET
            begin           0x00D3E5A4   RAMWR
            DMA             0x00D9982C   bulk path
            FIFO            0x0080E5C0   remainder path
      lv_disp_flush_ready   0x00D2F7EC
```

`flush_cb` is thin. It first dereferences a global pointer at `0x00829820` and,
if it is non-NULL and its byte at `+0x03` is set, calls `lv_disp_flush_ready`
and returns without drawing — a "suppress output" path. Otherwise
it packs the `lv_area_t` into a 36-byte request — `{u8 type=4, u16 x1, u16 y1,
u16 w, u16 h, ... u8 flag=1}`, computing `w`/`h` in a way that tolerates
reversed coordinates — and hands it to the op dispatcher. Then, and this is
the interesting part: **if `drv->[+0x24]` is set it does not call
`lv_disp_flush_ready`**. The callback at `0x0080B1EC` does, after issuing a
type-5 (wait) request that blocks until the transfer has drained. So the flush
is asynchronous, and the acknowledgement is deferred to whenever LVGL next
calls that slot. The slot is at offset 36 in `lv_disp_drv_t`, which in the v8
layout is one of `clean_dcache_cb` / `wait_cb` depending on the exact tag —
**not pinned down**, and it needs the vendor's `lv_hal_disp.h` to settle.

`lv_lcd_init` (`0x00D3E34C`) fills a table at `0x008298B8` — the panel
descriptor in the first word, then eight function slots:

| Off | Function | Role |
|---|---|---|
| `+0x00` | `0x0081C1CC` | the panel descriptor (data, not code) |
| `+0x04` | `0x00D3E2A4` | LCDC bring-up — the routine §12b transcribed |
| `+0x08` | `0x00D3E18C` | teardown |
| `+0x0C` | `0x00D3E1A4` | panel reset — the GPIO pulse in §7g |
| `+0x10` | `0x00D3E21C` | called as `(10, 0x00D3B840)` from `lv_port_disp_init` |
| `+0x14` | `0x00D3E188` | |
| `+0x18` | `0x00D3E2E4` | **blit** — what `flush_cb` reaches |
| `+0x1C` | `0x00D3E1FC` | |
| `+0x20` | `0x00D3E1DC` | |

It refuses to run twice: if `+0x04` is already set it logs (line 236 of its
source file) and returns 1. On success it calls `0x00D3E2A4` (the LCDC
bring-up) and then an optional hook at panel-descriptor `+0x28` — which is
**0 on this board**, so nothing runs there. That extends §7b's descriptor map
by three words: `+0x28 = 0`, `+0x2C = 4`, `+0x30 = 0x0081C1FC`.

### 13d. Byte order on the wire — settled

The window writer at `0x00D3EBD4` has two paths, chosen by whether the
remaining byte count is a multiple of 8 (RGB565) or 24 (RGB888):

- **bulk**: hand the buffer to the DMA at `0x00D9982C` with the QSPI quad
  opcode `0x32` and the DCS command RAMWR, both read out of the panel
  descriptor at `+0x0C` and `+0x0F`.
- **remainder**: read each pixel with `ldrh`, **`rev16`**, and push it out
  through the FIFO writer.

`rev16` is the answer. LVGL keeps RGB565 little-endian in the framebuffer
(13b), and the driver byte-swaps on the way out, so the panel receives
**high byte first** — which is what MIPI DCS wants. Any driver we write that
feeds `0x400D9000` from an LVGL-style RGB565 buffer must do the same swap, in
software or by whatever descriptor bit the DMA path uses. The bulk path's swap
has not been located; it is presumably a mode bit in the DMA or LCDC setup, and
that is worth finding before trusting the fast path.

Also worth noting from the same routine: the transfer is refused outright while
a previous one is in flight (`state->[+0x04] == 2`), and the colour mode comes
from panel descriptor `+0x08` — 1 for RGB565 (2 bytes/pixel), 2 for RGB888
(3 bytes/pixel).

### 13e. The backlight, re-opened

§7b concluded that the PWM driver "is not recoverable from these dumps". It is
worth re-running that search now that 94 KB more of the application is
readable. Two concrete leads:

- A device record at flash `0x00C7DE88`: `{0x00D452A1, 0x00030000,
  ptr-to-"/dev/pwm_ch3", 00 01 02 03 04 05}`. `0x00D452A0` is code; `0x00030000`
  decodes as bank 3 pin 0 under §7f's pin-id encoding, which §7g already lists
  as an output driven high at `0x00D46432`.
- The open at `0x00D10354` logs `-pwm open /dev/timer failed except!` on
  failure, so the backlight PWM is built on the **timer block at
  `0x40009000`** (§7c), not on a dedicated PWM peripheral. That is a much
  narrower target than "find the PWM registers", and the timer block's layout
  is already partly known.

Both have now been followed through — **§14 has the registers.** The second
lead was half wrong: the backlight is a PWM peripheral, just not the one §7c
found, and not at `0x40009000`.

## 13f. What state a payload actually starts in

Measured from the host in bootloader mode on a cold chip (2026-08-07). Read
this before assuming any peripheral is available: **almost nothing is.**

CRU at `0x40080000`, every non-zero register:

| Off | Value | Reading |
|---|---|---|
| `+0x000` | `0xD0010802` | core PLL, **locked** (bit 28) |
| `+0x008` | `0x00000801` | CPU PLL, **stopped** — §12.6 |
| `+0x010` | `0x02103AB4` | |
| `+0x018` | `0x02206060` | |
| `+0x01C` | `0x00010000` | |
| `+0x030` | `0x00000070` | |
| `+0x040` | `0x00000009` | divider A (§`sl6806_cru.h`) |
| `+0x048` | `0x00000051` | divider B |
| `+0x060`/`+0x070` | `0x00000002` | |
| `+0x064`/`+0x074` | `0x00000008` | module gates. **Only bit 3.** The LCDC's bit 15 is *off* until something calls `Screen.begin()` |
| `+0x068`/`+0x078` | `0x01000160` | more gates; bit 4 here is the PWM (§14a) |
| `+0x0A0` | `0x00000101` | gate-shaped, **unexamined** |
| `+0x0B0`, `+0x0DC`, `+0x0FC` | `0x00000001` | gate-shaped, **unexamined** |
| `+0x0EC` | `0x00000700` | gate-shaped, **unexamined** |
| `+0x100` | `0x00001701` | |
| `+0x108` | `0x007C7C00` | |
| `+0x10C` | `0` | the LCD module clock, until the LCD driver sets `0x911` |
| `+0x11C` | `0` | the clock enable §14a writes `0x31` to |

Power domains at `0x40000070` read `0x03FF03FF` — all ten requested and all
ten acknowledged — so `0x00D9A7AC`'s handshake is already done. That is the
one thing that *is* set up for you.

> ⚠ **Do not assume state survives between runs.** It sometimes does — a
> payload upload does not by itself reset the clock tree, and one run saw the
> PWM still gated and configured from the previous one. But a replug (which is
> how you get back into bootloader mode) resets everything, and the table above
> is what you get afterwards. Treat every run as cold, write every step
> unconditionally, and print a before/after readback. A stale chip and a
> negative result look identical otherwise, and one run was lost to exactly
> that.

**The consequence worth internalising:** a peripheral that reads as zeros is
gated, and a peripheral that reads structured values but ignores writes is
*also* gated — `0x40096000` (the ADC) does the second. The only reliable test
is write-then-read-back. Three runs of the backlight hunt were spent reading
correct-looking values as though they meant the block was working.

## 14. The PWM (backlight) and the DMA, both found

Two blocks that no previous scan located, because neither is addressed as
`base + offset` from a literal — both compute a per-channel base from one
literal and a shift, so `tools/sl6806-find-mmio` had nothing to see.

### 14a. PWM at `0x40084000` — this is the backlight

`0x00D99C34` initialises one channel and is where the base falls out:

```
r0 = 0x40084020 + (ch << 5)              ; the channel's registers
[0x0082B3F8 + (ch + 4) * 4] = r0         ; cached in a table
[0x0082B3F8 + (ch >> 1) * 4 + 4] = 0x40084010 + (ch >> 1) * 4
[0x0082B3F8] = 0x40084000
```

So: **block base `0x40084000`, six channels at `0x40084020 + ch * 0x20`**, a
global register at `0x40084000`, and a per-pair register at
`0x40084010 + (ch >> 1) * 4`. Everything else reaches a channel through the
table at `0x0082B3F8`, which is why the base appears exactly twice.

Channel registers, from the accessors in the blob at `0x00811E48`–`0x00811EC0`:

| Off | Role |
|---|---|
| `+0x00` | control. bit 4 = **run**; bit 8 = update trigger, and the same bit is polled until clear; bit 28 polled as busy; `0x40` written at init. ~~`0x00811EC0` writes `src \| (div << 8)`~~ — **corrected, see §18: that write goes to the pair register, not here** |
| `+0x04` | **`(period << 16) \| duty`** — written by `0x00811D04`, which returns without writing unless `period >= duty` |
| `+0x10` | bit 0 = enable; `0x00811E9A` writes `(x << 16) \| (mode << 1) \| bit0`, and forces bit 0 low first if bits [3:1] change |
| `+0x14`, `+0x18`, `+0x1C` | three more `(hi << 16) \| lo` pairs |

Entry points, all in the blob: `0x00811CE4` run/stop (`+0x00` bit 4),
`0x00811CF4` enable (`+0x10` bit 0), `0x00811D04` set period/duty,
`0x00811D2C`/`0x00811D3C` trigger and wait.

**The backlight is PWM channel 3, and the numbers are exact.** `0x00D102F4`
is `set_brightness(percent)`:

```c
if (pct > 100) pct = 100;
req.op     = 3;
req.period = 48000;              // a count, not a frequency
req.duty   = pct * 480;          // rsb r3,r0,r0 lsl #4 ; lsls #5  ->  pct*15*32
ioctl(pwm_dev, 0, &req);         // 0x00D3D374
```

`0x00D10354` opens `/dev/pwm_ch3` through the driver framework
(`open` = `0x00D3D1E8`), with an initial period/duty of 48000/24000 and a
completion callback at `0x00D102D4`, then immediately calls
`set_brightness(60)` — which is where §7b's "default is 60%" comes from.

The driver itself is registered from a table of ~21 device records at
`0x00D3CF40`, each handed to `0x00D3D094`. The PWM record is at flash
`0x00C7DE88`; its constructor `0x00D452A0` allocates 80 bytes and installs
ops at `0x00D452F0`, `0x00D45394` (configure), `0x00D4534C` (stop) and
`0x00D45200`.

**What is still missing is the pad.** The record carries `0x00030000`, which
decodes under §7f as bank 3 pin 0 — and §7g independently lists
`0x000300C0`, the same pad as an output driven high at `0x00D46432`. That is
suggestive, not established: the record's field order has not been read out
of `0x00D3D094`, so `0x00030000` may be something else entirely. It also sits
badly with §7b's result that driving every pad high lights nothing — although
a backlight that wants a pulse train rather than a level would explain both.

**TRIED ON HARDWARE, 2026-08-07 — no light, and the reason is upstream.**
`examples/Backlight` ran all six channels. Every register read back zero,
*including the module gate itself*: `modctl 0x0 -> 0x0`. The write did not
stick, so nothing downstream of it could, and the gate bit was never the
question.

Reading the same addresses from the host, over the vendor read command, in
the same bootloader mode, says the same thing and settles what it means:

| Block | Reads |
|---|---|
| `0x40080000` CRU | live — `020801d0 00000000 01080000 …` |
| `0x400D9000` LCDC | live — `99990000 48000000 01000000 …` |
| `0x400E0000` | all zeros |
| `0x400E2000` | all zeros |
| `0x40084000` | all zeros |

So MMIO is fine, the addresses are not obviously wrong, and the whole
`0x400E****` region is simply **unclocked** — for the host as much as for a
payload, which rules out anything payload-specific.

**And the firmware says why: the PLL is off.** `0x00D9A7FC`, the first thing
the vendor's module bring-up does, starts a PLL and spins on a lock bit
*before* the first call into `0x400E0000`. That register reads `0x00000801`
in bootloader mode, lock bit clear. See §12.6, which this also answers.

**The bit-2 claim is retracted.** It was read off `0x00D99C14` calling
`0x00D9A74C(2)` — but that teardown belongs to the module whose init ends at
`0x00D99C0C`, not to the PWM channel init that happens to start `0x20` later.
Across the eleven call sites of `0x00D9A734` the numbers used are 0, 1, 2, 3,
4 and 6, and none is tied to the PWM. It is now a thing to find by experiment.

**Then the gate was found by sweeping: CRU `0x40080068` / `0x40080078`, bit 4.**
With it set the block answers — CTRL took `0x7F` and read back `0x1000007F`,
period/duty took `(48000 << 16) | 28800` exactly. So §14a's register map is
right and every write lands.

**And the panel still does not light, because the counter has no clock.** Read
back from the host afterwards, channel 3 holds its period and duty exactly,
and **CTRL bit 28 is stuck set** — the busy flag `0x00811E74` spins on before
every write. In a working system that must clear or the vendor's own setter
would hang. Programmed is not running. The pair registers at `0x40084010` are
zero, but nothing in flash or in the blob ever writes them, so they are not it.

**The pad was a real missing step and was not enough on its own.** The vendor's
configure op calls ROM `0x93C` with the id its constructor stores at `dev+0x48`
— `0x00010200`, bank 1 pin 0 function 4. Muxing it changed nothing while the
counter was stopped, but it is required and no earlier work had it: this is why
`examples/BacklightHunt` could not have succeeded, since it drove candidates as
function 1 and skipped bank 1's low pins as the panel's bus (true of pins 1–8,
not pin 0).

**Open: `0x4008011C`.** The vendor writes `0x31` after the PLL locks and `0x30`
on the way down — one bit apart, so bit 0 is an *enable*, not the clock-source
reparent it was first taken for, and an enable cannot carry the core or USB off
with it. It is the last unwritten step of `0x00D9A7FC` and the leading
explanation for both a stalled counter and a permanently dead `0x400E0000`.

> **MEASURED 2026-08-13, and half of that is now answered.** `0x4008011C = 0x31`
> was written on hardware as part of the camera bring-up and read back as
> `0x31`, with the PLL locked — and `0x400E0000` still would not hold a bit.
> So `+0x11C` is **not** what gates that register. What does is a mask ROM
> module clock: after `sl6806_module_enable(46)`, `0x400E0000` bit 6 sticks
> and reads back. §7n has the sequence. The stalled PWM counter is still open,
> but it is now open on a different question — which bit of `0x400E0000` is
> the PWM's, nobody having looked — rather than on whether the register can be
> written at all.

> ⚠ **CRU state survives a payload upload.** Uploading does not reset the clock
> tree, so a PLL left locked by one run is still locked for the next. One run
> was lost to a sketch that returned early on "already locked" and thereby
> skipped the step it existed to test. **Probes must write every step
> unconditionally**, and print a before/after readback, or a stale device looks
> like a negative result.

**The module walk was run properly and came back empty — a real negative.**
All 128 ids, in the mask ROM's order, with the acknowledgement waited for:
CTRL bit 28 never cleared. So the counter does not want a second module clock,
and this is the first negative in this section that was reached with the right
method rather than the wrong one.

**The pair clock register was swept too, and is not the answer.** Sixteen
sources against six dividers into `0x40084014`: the counter stayed stopped. The
sweep did measure the register's shape, which is worth keeping — it only
retains `0x10F`, so `src` is `[3:0]` and the divider is a single bit at `[8]`,
not the eight-bit field `div << 8` in `0x00811EC0` implied.

Also checked and eliminated: FIRM's `HAL_timer_*` is at `0x40099000`, a
different block, so the PWM is not the timer and its clock is not the timer's.

### RESOLVED — the backlight works. The missing bit was `0x40084014` bit 8

Verified on hardware 2026-08-07, after twelve runs that were not. The recipe:

1. `sl6806_module_enable(68)` — CRU `+0x78` shadow, then `+0x68` gate, bit 4,
   then poll the gate. Order matters (§15b).
2. Mux the pad `0x00010200` — bank 1 pin 0, function 4.
3. `CTRL` ← `0x40`, then OR `0x3F`.
4. Period and duty: `(48000 << 16) | percent * 480`.
5. `MODE` bit 0.
6. **`0x40084010 + (ch>>1)*4` bit 8** — the pair's clock enable.

Step 6 is the whole story. **Nothing in flash or in the SRAM blob ever writes
that register**, so no amount of reading the vendor's code could have produced
it; it was found by holding each of the 32 settings the register accepts and
watching the panel. The light came on at `0x100` and stayed on through `0x10F`,
so bits `[3:0]` — what `0x00811EC0` assembles as a source select — make no
difference to whether it runs. The writable mask is `0x10F`, so the "divider"
implied by `div << 8` is that single enable bit.

**The instrument was the problem, not the map.** CTRL bit 28 stays set while
the backlight runs perfectly. Three separate "the counter never starts"
conclusions in this section came from using it as a success test, including one
that closed the investigation. `0x00811E74` does spin on it before writing
period and duty, so it means *something* — but it is not a busy flag, and
nothing should test it.

Driver in `cores/sl6806/sl6806_pwm.[ch]` (`sl6806_backlight_begin`,
`sl6806_backlight_set`), 16 host tests in `tests/host/test_pwm.c` aimed
squarely at the pair-register write, because a reader checking this driver
against the disassembly would find no vendor code for it and reasonably
conclude it was spurious.

**It lights, and it does not dim — the counter is not running.** Duty 0 and
duty 100 are indistinguishable, CTRL bit 8 (the commit the vendor's own
accessors pulse) never self-clears once set, and CTRL bit 28 is permanently
set. So bit 8 of the pair register is driving the pad to a **static level**,
not making a waveform. This is an on/off backlight until the counter's clock is
found, which is enough to unblock the display and not enough to dim it.

> ⚠ **Every sweep in this section that used CTRL bit 28 as its success test is
> void** — including the walk over all 128 module ids, which is otherwise the
> most thorough negative here. They were interrogating a bit that never
> answers. A valid test is `sl6806_pwm_counter_ticking()`: read the channel
> block twice and see whether anything moved. A running counter ticks; a
> stopped one does not. `examples/Backlight` will redo the walk with it if you
> press `h`.

**The walk was redone with that detector and came back empty** — all 128 ids,
twice, nothing ticked. That is the first negative in this section reached with
a test that can answer, so it stands. One caveat worth stating: the detector
watches `+0x00`..`+0x1C`, so strictly it shows no register visibly moves rather
than that the counter is stopped. Taken with duty having no effect, the
conclusion holds.

**So the backlight is on/off and that is where it rests.** It is enough to
unblock the display, which is what it was blocking.

*Bring-up failed on the second upload* with "the module clock refused", on a
board that was visibly working. That was a bug in the driver, not the chip:
it wrote `CTRL = 0x40` and compared the read-back for equality, and CTRL
carries read-only status (bit 28 among them), so on a warm chip it read
`0x10000040` and the check failed. It now probes period/duty, which has no
status bits. **Any writable test on this chip has to pick a register that is
purely writable** — the same trap in a different costume.

### Superseded — the state before the panel lit

**The backlight came on**, blinking, while the pair-register sweep ran, and
went dark for good once it finished (2026-08-07). First light out of this board
from a payload.

Two things follow, and the first invalidates the run above.

**CTRL bit 28 is not a usable "counter stopped" indicator.** It stayed set
throughout, while the backlight was visibly blinking. So every "still busy" in
that log, and its closing "no clock source started the counter", measured
something else. Whatever bit 28 means — `0x00811E74` does spin on it before
writing period/duty — it is not "the counter is not running", and no negative
in this section that used it as a test should be trusted.

**And the sweep destroyed its own result.** It wrote the pair register back to
zero at the end of each divider row, so a setting that lit the panel was
extinguished a moment later. "Blinks, then dark" is precisely that.

The register's writable mask is `0x10F`, so the real search space is 16 sources
against a **one-bit** divider — 32 settings, not the 96 the sweep believed it
was covering. `examples/Backlight` now holds one setting per tick, never resets
the register, and freezes on any keypress, so the operator is the detector and
the log line names the answer.

### Superseded — the state when this was thought closed

Everything reachable has been tried, most of it twice and the second time
correctly. Established and verified on hardware:

| | |
|---|---|
| PWM block | `0x40084000`, channels at `+0x20 + ch*0x20` |
| Module id | 68 — CRU `+0x68` gate, `+0x78` shadow, bit 4 |
| Backlight channel | 3 |
| Pad | `0x00010200` — bank 1 pin 0, function 4; confirmed by reading the bank's function register back as `0x12222224` |
| Registers | CTRL takes `0x7F`, period/duty takes `(48000 << 16) \| duty` exactly |
| CTRL bit 28 | busy, and permanently set |

Eliminated, each by measurement: the PLL (locks, changes nothing); the ten
power domains (already acknowledged before a payload starts); `0x4008011C`
(takes `0x31`, changes nothing); all 128 module ids in the ROM's own order with
the acknowledgement waited for; every bit of the eleven gate-shaped CRU
registers; every module-clock register `+0x100`–`+0x13C`; the pair clock
select; the pad; the register contents.

So the counter's clock is not in the address space a payload can reach, and no
further register guess is worth a run. **The next move is ground truth**: read
the CRU and the PWM while the stock firmware is running and diff against §13f's
cold state. `read_mem` is refused in card-reader mode (§7d.1), so that needs a
way in first — the likeliest being a patched FIRM, which §4 and §6 make
repackable and which the mask ROM makes recoverable.

**Background — the gate is module id 68.** §15b's `module_clock_enable` says ids 64–95
gate through CRU `+0x68` with `+0x78` as the shadow, so "bit 4 of `0x68`" is
module 68. The sweep that found it wrote both registers at once, which happens
to be survivable for this peripheral and is not in general — the ADC's correct
bit was recorded as dead by exactly that mistake. `examples/Backlight` now
enables module 68 properly and then walks 0–127 for a second id, with CTRL bit
28 going clear as the test. Everything below this line predates that.

**Parked after seven runs. The counter clock was not found by sweeping.**
`0x4008011C` took `0x31` and changed nothing — modctl still zero, busy still
set. Then, with the known gate bit held, every other bit of all three CRU gate
pairs was tried on top of it and **nothing cleared busy**. Two assumptions were
checked from the host rather than assumed: bank 1's function register reads
`0x12222224`, so pin 0 really is on function 4, and reading the channel twice
gives identical words, so nothing counts.

What is eliminated: the PLL, the ten power domains, the `0x4008011C` enable,
all 96 CRU gate bits, the pair registers (nothing in flash or the blob writes
them), the pad, and the register contents. What is left is a functional clock
that nothing in the reachable address space turns on.

**The next move is ground truth, not more register guesses.** Diff a working
configuration against ours — read the CRU and PWM blocks while the *stock
firmware* runs. `read_mem` is refused in card-reader mode (§7d.1), so that
needs another way in, and §13 supplies the likeliest one: the application's
SRAM image can be loaded out of flash by a payload, so the vendor's own driver
stack could be brought up and called rather than reimplemented.

### 14b. DMA — the channel registers are at `0x40001000`

§7c lists `0x40070000` as "DMA". That block is real but it is only the
request router and interrupt controller; the data path is somewhere else.
The bootloader's channel allocator at `0x00829768` gives it away:

```
r2 = 64 * ch + 0x40001000        ; smlabb
[handle + 8]  = r2               ; the channel's registers
[handle + 12] = ch
```

**Eight channels at `0x40001000 + ch * 0x40`.** Handles are 16-byte records
in a table (`0x0082EE00` in the bootloader), with `+0x0D` as the in-use flag.

Channel registers, from the blob's API at `0x0080DA74`ff:

| Off | Role |
|---|---|
| `+0x00` | control. bit 30 = start, bit 29 = cleared to arm / cleared again to abort, bit 31 = enable, bit 25 = a mode bit set by the allocator |
| `+0x04` | **source** |
| `+0x08` | **destination** |
| `+0x0C` | `[17:0]` = length in bytes, `[31:18]` preserved config. Reading it back gives the remaining count (`0x0080DAFC`) |

Eighteen bits of length means **256 KB per transfer** — a whole 240×296
RGB565 frame (138.75 KB) goes in one.

API in the blob: `0x0080DA74` start, `0x0080DAC8` abort, `0x0080DADE`
enable/disable, `0x0080DAFC` remaining.

The `0x40070000` block is the other half:

| Off | Role |
|---|---|
| `+0x00` | request routing, 4 bits per channel |
| `+0x20`, `+0x28` | request routing, 2 bits per channel each |
| `+0x24` | interrupt status for the write direction, **write-1-to-clear** |
| `+0x2C` | interrupt status for the read direction, W1C |

Each channel occupies bits `2n` and `2n+1` of a status word. The two ISRs are
`0x00806394` (`+0x24`) and `0x008063E0` (`+0x2C`); both read the status,
write it straight back, then dispatch on bits 0, 2, 4 and 6 to callback slots
at `0x008273D0 + 8` .. `+36`. The bootloader has a byte-identical pair at
`0x00821DBC` — same silicon, same SDK code, so either copy can be read.

Routing is allocated by `0x00CC6E34`, which walks four slots and then sets a
regular pattern: channel `c` uses `+0x00` bits `4c` and `4c+4` (one for each
direction), and bits `[2c+1:2c]` of `+0x20` and `+0x28`.

### 14c. How the display uses the DMA

`0x00D9982C` is the application's LCDC transfer, and it is worth comparing
against `cores/sl6806/sl6806_lcdc.c` because it differs from the polled
transcription in ways that matter:

- it waits on `+0x14 & 0x60000000` before touching anything;
- `+0x24` gets the QSPI opcode and `+0x2C` the DCS command, both read from
  the panel descriptor (`0x32` and RAMWR);
- **`+0x28[15:0]` is the pixel count minus one, not the byte count** — the
  element width comes from `+0x20[21:20]`;
- for the pixel stream both command-field lengths are zero, so `+0x20[3:2]`
  is set to 2 (FIFO only) and **no command frame is emitted at all**; RAMWR
  was already sent separately by `0x00D3E5A4`. A driver that re-sends RAMWR
  with every chunk is not doing what the vendor does;
- `+0x08` bit 4 is explicitly cleared — register mode, not command-list mode,
  which matches the measurement in §12b;
- then `0x0080DA74(handle, LCDC + 0x30, src, len)` hands the buffer to a DMA
  channel writing into the LCDC FIFO, and `+0x20` bit 0 starts it.

The remainder that does not divide by 8 goes out through `0x0080E5C0` with
the `rev16` per pixel described in §13d. **Where the bulk path gets its byte
swap is still unknown** — presumably a mode bit in the DMA or in
`+0x20[21:20]`, and worth finding before trusting it.

## 15. The keys, and the same wall as the backlight

**What is established, from the dump alone.** The key manager at `0x00D1DA20`
opens two devices and hands each a map; both maps are in the SRAM blob (§13):

| Device | Map | Contents |
|---|---|---|
| `/dev/key_io` | `0x0081C044` | 16-byte records `{pad_id, 0, key_id, 0x101}` — bank 1 pin 17 → key `0x3E`, bank 1 pin 12 → key `0x3C` |
| `/dev/kadc_ch0` | `0x0081C02C` | 12-byte records `{level, key_id, 0}` — `0x0200` → key `0x42`, `0x0E60` → key `0x40` |

Both opens pass a count of 2, so those are all the entries. Four keys, plus
power through the PMU.

**The ADC is at `0x40096000`**, from the only MMIO literal in its HAL
(`0x00D994F8`). Channels are `0x10` apart from `+0x20` (`0x00D993A0` dispatches
ten of them). This board's key channel is 0, on **bank 1 pin 9, function 15** —
the kadc constructor `0x00D3DB50` stores pad `0x00014800` with pad value
`0x780`. `adc_init` is `0x00D994EC`: `+0x10`, `+0x18`, `+0x0C` ← 0,
`+0x04` ← `0x0002A800`, `+0x00` ← `0x80180000`.

**MEASURED, NEGATIVE — the GPIO keys are not the buttons.** Neither
`/dev/key_io` pad moves when a button is pressed. Pin 12 idles at 1; pin 17
sits at a steady 0, which confirms §7g's note that something external drives
it. Nor does any of §7g's "likely button group" (bank 1 pins 22–27, 30, 31).
Given the firmware also has `pwm_is_jack_exist` and `pwm_is_sd_exist`, these
two are most likely detects rather than user keys — **untested**, and cheap to
test by inserting a jack or an SD card and watching `examples/Buttons`.

### RESOLVED — the keys read, and here is the whole recipe

Working on hardware 2026-08-07. In order:

1. **Enable module id 84** the way the mask ROM's `module_clock_enable`
   (`0x00001C5C`) does — CRU `+0x78` (shadow) first, then `+0x68` (gate), bit
   20, then poll `+0x68` until the bit reads back. Order matters; see §15b.
2. **Mux the pad**: `0x00014F80` — bank 1 pin 9, function 15 (analog).
3. **`adc_init`**, transcribed from `0x00D994EC`: `+0x10`, `+0x18`, `+0x0C` ← 0,
   `+0x04` ← `0x0002A800`, `+0x00` ← `0x80180000`.
4. **Enable the channel**, which `adc_init` has just switched off and which
   nothing else turns back on: `+0x00 |= (1 << ch)` (`0x00D994BC`) and
   `+0x0C |= (1 << ch)` (`0x00D9948C`).
5. **Read the result** at `+0x24` for channel 0 — i.e. `+0x04` inside the
   channel block at `+0x20 + ch*0x10`.

Measured plateaus, jittering about 5 LSB:

| ADC | Meaning |
|---|---|
| ~`0xFE9`–`0xFEE` | nothing pressed |
| ~`0xD54`–`0xD5B` | key `0x40` |
| ~`0x1B`–`0x2C` | key `0x42` |

**The vendor's map is a list of ascending thresholds, not exact values.**
`0x0081C02C` holds `{0x0200, 0x42}` then `{0x0E60, 0x40}`, and all three
measured plateaus land where "below `0x200` → `0x42`, below `0x0E60` → `0x40`,
else nothing" predicts. `examples/Buttons` decodes it that way.

Still open: which physical button is which. The ids are `0x40` and `0x42`;
mapping those to volume up and down needs one press each with someone
watching, or the key-id enum out of the scene framework.

**Superseded — the ADC is readable but not writable.** It reads
structured values in a cold chip (`+0x00 = 0x00100000`, `+0x04 = 0x00000800`,
`+0x08 = 0x000001E0`, `0x10` in the first two channel words) and **ignores
every write**: `adc_init` leaves the block byte-identical. It is not an alias
— `0x40095000` reads zeros and `0x40097000` is a different live block — so it
decodes on its own and is simply gated.

Tried and failed to open it: all 32 bits of each of the eleven gate-shaped CRU
registers (`+0x60`, `+0x64`, `+0x68`, `+0x70`, `+0x74`, `+0x78`, `+0xA0`,
`+0xB0`, `+0xDC`, `+0xEC`, `+0xFC`), and every module-clock register
`+0x100`–`+0x13C` with enable set and each plausible source.

**This is the same wall as §14a**, and it is worth stating as one problem
rather than two. The application enables most peripherals through
`0x400E0000` (`0x00D9A734`), and ~~that register is dead from a payload~~ —
**superseded, §7n: it is gated, not dead, and a mask ROM module clock opens
it.** The CRU gates are a second, partial mechanism — they cover the LCDC
completely, and the PWM's registers but not its counter. Nothing reachable
turns on the rest.

> The wall was one problem, and the shape of the answer is the same for both
> halves: registers and function are separate enables, from separate id
> spaces, in one order. §7n. What that leaves open for the backlight is which
> bit of `0x400E0000` belongs to the PWM — the camera's id 6 is the only one
> attributed — which is a 32-bit walk with the counter as witness, and nobody
> has run it.

**So the useful next step is not another register sweep.** It is either to get
the vendor's own driver stack running — §13 shows the app's SRAM image is in
flash and loadable by a payload — or to observe a working configuration
directly, which needs a way to read memory while the stock firmware runs
(§7d.1 says `read_mem` is refused in card-reader mode).

### 15a. The module clock register shape — decoded

Worth recording because it was learned the hard way. `0x00D44690` writes
`0x40080118` as: `[7:4]` ← one argument, `[11:8]` ← another, then bit 0 set.
So CRU `+0x100`–`+0x13C` are per-module clocks with

    bit 0 = enable,  [7:4] = source,  [11:8] = divider

which fits both known values: the LCD's `+0x10C` = `0x911` (enabled, source 1,
divider 9) and `+0x11C` = `0x31` (enabled, source 3). A peripheral needs *both*
a gate bit and a module clock — the LCD driver sets both, and every sweep
before this one only ever touched gates.

### 15b. The mask ROM has a module_clock_enable, and it changes the map

`0x00001C5C` is `module_clock_enable(id)` and `0x00001CE8` is its disable.
They settle what the gate registers actually are, and they are not what §14a
and §15 assumed:

| Module id | Gate | Shadow |
|---|---|---|
| 0–31 | CRU `+0x60` | CRU `+0x70` |
| 32–63 | CRU `+0x64` | CRU `+0x74` |
| 64–95 | CRU `+0x68` | CRU `+0x78` |
| **96–127** | **`0x400F1000 +0x20`** | **`0x400F1000 +0x30`** |

Three things follow, and each invalidates part of an earlier sweep.

**There is a fourth register pair, at `0x400F1000`.** §7c files that base as a
storage controller — it is that as well, but `+0x20`/`+0x30` are module gates
for ids 96–127. No sweep in this repository has ever touched them, so a
quarter of the module space was unreachable, and "all 96 CRU gate bits" in
§14a was 96 of 128.

**The `0x60`/`0x64`/`0x68` registers are the gates and `0x70`/`0x74`/`0x78`
are their shadows** — not two independent banks, which is how the backlight
hunt treated them.

**The order is part of the operation.** The ROM writes the shadow, then the
gate, then spins until the gate reads the bit back. Every sweep here wrote
both at once and moved on without waiting for the acknowledgement.

`examples/Buttons` now walks ids 0–127 through a transcription of that
routine. It is transcribed rather than called because the ROM's poll is
unbounded: an id nothing implements would spin forever inside the boot ROM's
USB handler and take the device off the bus.

**IT WORKS. The ADC is module id 84** (2026-08-07) — the third pair, CRU
`+0x68` gate / `+0x78` shadow, **bit 20**. With it enabled the block takes
writes and holds them: `+0x00` reads back `0x80180000` and `+0x04` reads
`0x0002A800`, exactly what `adc_init` wrote.

Read that against §15's negative, which tried *that very bit* and failed. The
difference is entirely the order — the old sweep set gate and shadow together
and moved on, where the ROM sets the shadow, then the gate, then waits for the
gate to read the bit back. **Shadow first, then gate, then poll: the order is
the operation**, and a sweep that skips the acknowledgement will report a
working bit as dead.

The walk also has to be paced. The first attempt ran all 128 ids inside
`setup()` with a 100000-iteration poll each, spent over a second in the boot
ROM's USB handler, and took the device off the bus — with no log, because the
console is a ring in the device's own RAM read over the link that died. Four
ids per `loop()` call and a 200-iteration poll fixes both halves.

**This is worth re-running for the PWM too.** The backlight's functional clock
was hunted across three register pairs with the wrong write order and without
the fourth pair, so §14a's negative is not as complete as it reads.

## 16. Bluetooth — link-manager code and a candidate register block

**Confirmed:** the application embeds a complete Bluetooth stack, not a
stub. String evidence: a full `BT_EVENT_*` enum (stack open/close, inquiry,
pairing, A2DP/AVRCP/HFP/SPP/OPP open/close/stream), profile name strings
(`l2cap`, `rfcomm`, `a2dp`, `avrcp`), and 27 persisted keys in the `PSMP`
partition including `bt_addr`, `bt_name`, `bt_relink`, `le_addr` (§7k) — this
unit has paired with something. No external-chip transport is visible: no
vendor strings (Airoha/Jieli/Beken/Realtek/CSR/RDA), no AT-command text, no
`hci_h4`/`hci_uart` layer. The naming (`lm`/`llm` — link manager / lower
link manager) and a 64-entry HCI opcode dispatch table that logs
`"-hci cmd0x%x"` for unimplemented opcodes read as an HCI stack terminating
on this CPU against local hardware, not a UART bridge to a separate radio.

**Candidate register block: `0x400E2000`.** Already listed as an
unidentified block in the whole-firmware scan (§7c). What ties it to
Bluetooth: it is the **single** literal load of that exact constant anywhere
in the 1.8 MB FIRM image (`tools/sl6806-xref dump.bin 0x400E2000`), and the
instruction that loads it, `0x00D98C9E`, sits inside a module-registration
routine a few hundred bytes from the HCI dispatch table (`0x00DA0000`
.. `0x00DA9BCA`, found via
`tools/sl6806-xref dump.bin --string="-hci cmd0x%x" --context 12`).

That routine (`0x00D98C9C`) does a clear-bit31 / delay(10, via the
already-documented delay veneer `0x00807214`) / set-bit31 reset pulse on
`+0x228`, writes `0xFFFFFFFF` to `+0x200` during the window, registers
`{base=0x400E2000, config_ptr}` into an SRAM descriptor at `0x0082B3A8`,
calls `0x00D9A7FC` (r0=42), `0x00D9A734` (r0=0) and `0x00D9A768` (r0=0), sets
bit 24 of `+0x214`, then fans a caller-supplied config struct out into
roughly a dozen narrow bitfields at `+0x10, +0x14, +0x20, +0x44, +0x48,
+0x4C, +0x50, +0x54, +0x58, +0x70, +0x78, +0x7C`. A separate accessor
cluster (`0x00D98B18`..`0x00D98C58`) reads `+0x200`, `+0x218` and `+0x228`
back through the same descriptor; `+0x218`'s top nibble gates a small table
lookup, the shape of a status/mode field.

Reset-then-config-fanout is the same two-part shape every other confirmed
peripheral bring-up in this codebase has (CRU divider setters, LCD panel
init), which is the basis for treating this as real hardware rather than a
data table. Full offset list and citations: `cores/sl6806/sl6806_bt.h`.
Writeup with the reproduction commands and what to check on hardware next:
`docs/BLUETOOTH.md`.

**§14a/§15 land on top of this and explain why a payload will currently see
nothing.** Both `0x00D9A7FC` and `0x00D9A734` are named there, independent
of this section: `0x00D9A7FC` is "the first thing the vendor's module
bring-up does" — it starts the PLL at `0x40080008` and spins on its lock bit
— and `0x00D9A734` is the routine through which "the application enables
most peripherals through `0x400E0000`", confirmed dead from a payload, with
call-site arguments 0/1/2/3/4/6 elsewhere (this routine's argument here, 0,
falls inside that range). §14a's own host-side read of `0x400E2000` in
bootloader mode already came back **all zeros**, alongside `0x400E0000` and
`0x40084000` — consistent with this block sharing the same unlocked PLL and
`0x400E0000` gate as everything else behind that wall, not evidence against
it being real hardware.

> **Superseded, §17:** `0x00D9A768` is not unexamined any more — it is
> `sl6806_periph_reset`, a clear/delay/set pulse on `0x400E0008`, and it was
> already in `cores/sl6806/sl6806_module.c` under that name from the camera
> work. `0x00D9A7FC` is likewise more than "starts the PLL": it is the whole
> group bring-up, guarded on module 46. And this block's own bit in
> `0x400E0000` — left open here — is **bit 0**.

**Not established:** what any bit means (the mixed 7/8/10/12/14/16/18-bit
field widths are consistent with radio/link timing parameters but that is a
shape argument, not a decode); whether one register window is the whole
story for a part with an actual radio; and — now the same open item as
§14a/§15's PWM and ADC work — what unlocks the PLL and the `0x400E0000` gate
without reparenting the core or USB clock out from under the session.

**Superseded on the gate, 2026-08-13:** `0x400E0000` is not dead. It is a
register in a block that is itself gated, and a mask ROM module clock opens
it — `sl6806_module_enable(46)` then `sl6806_periph_enable(6)`, measured, with
the bit reading back. §7n. So `examples/BtProbe`'s zero read is worth
re-running with a module-id walk in front of it, using `0x400E2000`'s own
writability as the witness the way §7n used the DVP's. The PLL locks from a
payload too, and neither it nor the gate carried the core or USB away.
`examples/BtProbe` reproduces §14a's zero-read from a payload rather than
the host command, and is the sketch to re-run once that unlock work (Next
actions item 11) lands.

---

## 16. The audio controller — `0x40009000`, found by following a device name

Static, from the dump, 2026-08-13. Nothing in this section has been run on
hardware; `examples/AudioProbe` is the sketch that would change that, and it
takes five minutes.

### How it was found

Not by scanning for base addresses — §7c's literal scan had already seen this
block and misfiled it (below). By following the vendor's own device names.

The application keeps a device registry with string names, two of which are
`/dev/audio0` (`0x00C7595C`) and `/dev/audio1` (`0x00C75950`). The routine
that opens them, `0x00D72418`, logs

```
-audio_driver_open _volume=====%d cfg1.is_headphone:%d
```

Everything below it reaches hardware through a four-entry table of driver
objects at SRAM `0x0082B430`. Entry 1 is the audio stream device: its ops are
installed by `0x00D9A0A4`, and its ioctl `0x00D9A024` forwards every command
it does not handle to `0x00D96824` — a 145-way `tbh` jump table whose literal
pool is nothing but addresses in `0x40009000..0x400092BC`.

```sh
tools/sl6806-xref dump.bin 0x00C8EABD          # the open's log string
tools/sl6806-xref dump.bin 0x00C7595C          # "/dev/audio0"
```

Two independent confirmations before a single register is decoded:

**Three microphones.** `0x00D3C688` logs `[driver_audio] AMIC0_GAIN set to:%d`
(and AMIC1, AMIC2) and reaches the same dispatcher with commands `0x2D..0x32`.

**The rate split.** `0x00D979E0` computes `rate % 8000` and picks between two
constants handed to `0x00807300`: `24,576,000` (= 48000 × 512) and
`22,579,000` (≈ 44100 × 512). A modulo that separates the 48 kHz family from
the 44.1 kHz one, and two master clocks at 512 fs, is not something a
non-audio peripheral does.

### RETRACTED: §7c's "`0x40009000` = timers"

§7c's evidence was "channels at 0x100 stride (`+0x108`, `+0x208`) with
write-1-clear flags and per-channel callbacks; register triples at 0x20
stride". Every word of that is accurate:

- `+0x108` and `+0x208` are the playback and capture DMA control registers;
- their write-1-clear flags are bits 8 and 10, the two interrupts each
  direction raises;
- the per-channel callbacks are the four function pointers the IRQ 30 handler
  at SRAM `0x0080D8F8` dispatches to;
- the triples at 0x20 stride are the three playback volume channels at
  `+0x134`.

The same section already carried the disconfirmation and nobody joined it up:
*"FIRM's `HAL_timer_*` is at `0x40099000`, a different block"*. And the
literal scan over the **HLKJ bootloader** — the scan §7c says named most of
that table — returns nothing at all for `0x40009000`, so the attribution never
had a second source. This is the second time in these notes that a correct
structural description has been attached to the wrong peripheral (the first
was `0x40040000`, §7l); both times the giveaway was that only one program in
the image ever touched it.

### The clocks, and why this is the reachable one

```
0x400F70D8 |= 2                 0x00D9674A, before either clock
module_clock_enable(37)         0x00D96754, ROM 0x1EC0 -> 0x1C5C
romclk_enable(19)               0x00D9675A, 0x008051EC -> ROM 0x20EC
                                   -> 0x40080088 |= 1
IRQ 30, priority 2              0x00D967D0..0x00D967E4
```

**Neither of those is `0x400E0000`.** Every other peripheral this project has
left unfinished — the PWM's counter (§14a), the camera's MCLK (§7n) — is stuck
behind that register and the PLL in front of it. Audio's two clocks are a mask
ROM module id and a mask ROM romclk id, both of which `sl6806_module.h` and
`sl6806_romclk.h` already drive from an ordinary payload. That makes this the
cheapest untested peripheral on the board by a wide margin.

The sample-rate PLL is a *different* PLL from `0x40080008`: `0x00807300` in the
SRAM blob programs CRU `+0x10` and `+0x14`.

```c
while (CRU[0x14] & (1 << 11)) ;                     /* busy */
CRU[0x10] = (CRU[0x10] & 0xFC1F0000) | 0x3000 | (select << 5);
CRU[0x14] = (CRU[0x14] & 0x3800) | (1 << 31) | (1 << 10) | (ratio << 14);
```

`select`/`ratio` are `3`/`0x3126` for 24.576 MHz and `2`/`0x186C2` for
22.579 MHz. The ratio is 17 bits at `[30:14]`; bit 31 above it is the enable.

### The data path

`0x0080D9BC` (playback) and `0x0080D964` (capture) are the same routine one
page apart:

```c
play(handle, buf, len):
    if (len & 3) return -1;                        /* rejected, not rounded */
    *(u32 *)0x4000910C = buf;
    field(0x40009108, 0xFFFF0000, len);
    *(u32 *)0x40009104 = ((len * 3 / 4) << 16) | 1;
    /* and, from 0x00D97A5E: */
    *(u32 *)0x40009108 |= 0x10;
```

The three-quarter point in `+0x104` is the refill interrupt's threshold. The
handler reads the control register and writes the same word straight back —
so the flags are write-1-to-clear — and dispatches bit 8 and bit 10 to two
different callbacks. Which is the watermark and which is end-of-buffer is
inferred from the existence of the three-quarter point, not read.

Full register map, with per-register provenance, is in
[`cores/sl6806/sl6806_audio.h`](../cores/sl6806/sl6806_audio.h) and
[docs/AUDIO.md](AUDIO.md), kept as one source of truth rather than duplicated
here.

### What is still open

1. **The output route.** `+0x08` is two identical 16-bit halves (proved by
   `0x00D93F54` and `0x00D94028` being the same function with every constant
   shifted 16), and it has four settings selected by the driver state byte at
   `+0x113`. The strings nearby are `-play status to spk play` and
   `-audio switch earphone`. Which number is the speaker is unknown.
2. **Whether the speaker has an amplifier enable.** §7k's PSMP dump has a
   `spk_switch` key. If that is a GPIO it is not in the variant's pad map.
3. **The sample format.** The config struct's channel and width bytes come
   from a caller this analysis did not follow.
4. **The coefficient RAM** at `+0x400..+0x5FC`, 128 words, cleared at init
   under a *second* clock domain (module 32, romclk 45, and bit 2 of
   `0x40000020`) that the init takes and gives back. The application logs
   `hardware EQ config to %d // 0: None, 1: pop, 2: rock, ...`.

## 17. The `0x400Exxxx` group's gate, decoded — and what it says about §7n

Static, 2026-08-13, from `0x00D9A7FC` and the two module registrations around
`0x00D98C9C`. This supersedes the "still unexamined" wording at the end of
§15's Bluetooth notes.

```c
if (module_clock_is_enabled(46))   return;      /* ROM 0x1E54 — a once-guard */
power_request();                                /* 0x00D9A7AC */
*(u32 *)0x40080008 = 0xC0000C04;
while (!(*(u32 *)0x40080008 & (1 << 28))) ;
*(u32 *)0x40080008 |= 0x10000;
module_clock_enable(46);
delay(10);
*(u32 *)0x4008011C = 0x31;
delay(10);
periph_enable(5);                               /* 0x400E0000 bit 5 */
```

and the power request, which is at the **pad-mux** base and is the only code
in the dump that touches either register:

```c
for (i = 0; i < 10; i++)
    if (!(*(u32 *)0x40000070 & (1 << (16 + i)))) {
        *(u32 *)0x40000070 |= (1 << i);
        while (!(*(u32 *)0x40000070 & (1 << (16 + i)))) ;
    }
*(u32 *)0x40000074 = 0;
```

Ten request bits in `[9:0]`, ten acknowledgements in `[25:16]`. The teardown
(`0x00D9A7E4`) sets `0x40000074` bit 0, writes `0x40000070 = 0`, waits for
zero.

Four things this settles:

1. **Module 46 gates the group, not the camera.** §7n measured that
   `sl6806_module_enable(46)` makes `0x400E0000` hold bits and recorded 46 as
   "the camera front end". It is the register clock for the whole
   `0x400Exxxx` window — which is why one module id appeared to belong to a
   peripheral it shares nothing else with, and why the same id turns up in
   Bluetooth's bring-up.
2. **`0x400E0000` bit 5 belongs to the group**, taken by the shared path
   before any peripheral's own bit. Nothing else claims it.
3. **`0x4008011C`'s value is `0x31`** — §14a's open item, now sighted a second
   time from a completely different peripheral. `0x30` on the way down, one
   bit apart, which is an enable and not a reparent.
4. **`0x00D9A768` is `sl6806_periph_reset`** — `0x400E0008 &= ~bit; delay;
   |= bit`. It was already in `sl6806_module.c`, named from the camera work;
   §15's Bluetooth paragraph calling it unexamined was out of date with the
   tree.

And two new `0x400E0000` bit attributions, from the two registrations that
call the sequence: **bit 0** for the `0x400E2000` cluster (`0x00D98CAE`) and
**bit 1** for the `0x400E2300` cluster (`0x00D989E0`), each followed by a
reset on the matching bit of `+0x08`. That takes the attributed bits from one
to four, which is worth having before the walk item 4 of the README's "where
to help" describes: a walk with four known-good answers in it can be checked.

`cores/sl6806/sl6806_bt.c` performs the whole sequence, bounded at every poll,
and `examples/BtProbe` runs it between two passes over the window.

### MEASURED, 2026-08-13 — both blocks, on a P20 Player

Two runs, `examples/AudioProbe` and `examples/BtProbe`, in payload mode. The
device stayed on the USB bus throughout both.

**The audio controller is real.** Cold, all 32 probed registers read
`0x00000000`. After `sl6806_module_enable(37)` and romclk 19 — two writes,
no PLL, nothing near `0x400E0000` — thirty of them come up at once:

```
+0x018  0x00000200      +0x200  0x02300700    +0x224  0x00010801
+0x07C  0x24924924      +0x208  0x00000800    +0x228  0x0000A000
+0x080  0x24924924      +0x20C  0x00800000    +0x254/8, +0x284/8, +0x2B4/8 same
+0x100  0x00000700      +0x134  0x00010801
+0x108  0x00000800      +0x138  0x0000A1FF
+0x10C  0x00800000      +0x154/8, +0x174/8 same
```

Three readings fall out, and none of them was predicted closely enough to be
confirmation bias:

- **Playback levels reset to `0x1FF` (maximum) and capture levels to `0`.** A
  DAC comes up open and an ADC comes up shut. This confirms both that the
  block is audio and that §16's TX/RX assignment is the right way round.
- **Bit 16 resets *set* on all seven channels**, and it is the one bit the
  vendor's init clears, on exactly the three playback channels. §16 marked it
  `[I]` from the clear alone; this makes it `[M]`.
- **Both DMA address registers reset to `0x00800000`**, the base of SRAM.

Unpredicted: `+0x108` and `+0x208` both reset with bit 11 set, which is
neither interrupt flag; `+0x100` resets with `[10:8]` set; `+0x07C` and
`+0x080` reset to `0x24924924`, i.e. every 3-bit field at 4.

**And the write test in that run was wrong.** It reported "drops writes" for a
block that had visibly just woken, because it wrote `0x5A5A5A50` to a DMA
address register and compared for equality. That is not a valid SRAM address
here. This is §14a's PWM mistake in a new costume — *a write test is only a
test if the value is one the register is allowed to keep* — and it cost
`ToneDemo` the run, since `sl6806_audio_begin()` was vetoing on it. Both are
fixed: the probe prints old/written/read-back for four registers, and begin()
no longer vetoes.

**The `0x400Exxxx` gate opens from a payload.** `sl6806_bt_begin()` ran the
whole §17 sequence:

```
0x40080008 = 0xD0010C04     written 0xC0000C04; bit 28 LOCK came back set
0x400E0000 = 0x00000021     bits 5 and 0, both held
0x400E0008 = 0x00000001
```

Two open items from §14a close here. **The PLL locks** — §14a read it stopped
at `0x00000801` in bootloader mode and could not get past it. And
**`0x4008011C = 0x31` does not reparent the core or USB clock** — the write
§14a called "plausibly safe, but untried"; the console survived it twice.

Four registers in the Bluetooth window came up from zero — `+0x078`
= `0x0C019A14`, `+0x07C` = `0x06060502`, `+0x1B4` = `0x00003301`, `+0x21C`
= `0x00400000` — and **they persist across a re-upload**, which a second run
built with `-DBTPROBE_READ_ONLY=1` demonstrated by finding all four in its
"cold" pass. Worth knowing before anyone reads a cold pass as cold.

The four counters at `+0x228`..`+0x234` stayed at zero for all eight samples.
The window is powered and partly readable; the link controller is not running.
Nothing configured it, so that is the expected result rather than a negative.

**What this unblocks.** The README's long-standing "find the PWM's bit in
`0x400E0000`" item was blocked on a register that would not hold a bit from a
payload. It holds them now, and four of its bits are attributed (camera 6,
Bluetooth 0, the `0x400E2300` cluster 1, and 5 for the group), so a walk over
the remaining 28 with the PWM counter as the witness has known-good answers to
check itself against.

### MEASURED, second run — the DMA moves a buffer, and the DAC has no path

`examples/ToneDemo`, same day. Every buffer submitted through
`sl6806_audio_play()` completed:

```
buffer 1: completion flag set after 1 ms, ctrl=0x12BC0910
```

`0x12BC` is 4796, the buffer length in bytes, so **the length register holds
the descriptor exactly** and the completion flag raises itself. Five hundred
buffers in a row, no failures. The digital path is confirmed end to end.

No sound, and the same transcript says why in the line above: `DAC (+0x008) =
0x0`. Nothing in the framework had ever written the output route. The
corroborating number is the timing — 4796 bytes of 48 kHz 16-bit stereo is
**25 ms** of audio and it completed in under 1 ms, which is what a DMA
draining into a disconnected output stage does.

`sl6806_audio_route()` now transcribes the vendor's four cases from
`0x00D93F54` / `0x00D94028`:

```
route 0   v = (v & ~0x0041) | 0x2080 | 3   and 0x40009080[14:12] = 0
route 1   v =  v            | 0x2040 | 3
route 2   v = (v & ~0x2000)          | 2   and 0x40009014 |= 3
route 3   v =  v            | 0x2040 | 3   and 0x40009014 |= 3
```

then a 2 ms wait, the 5-bit trim at `+0x08 [12:8]`, and the two DAC enables
again. Channel 1 is every constant shifted left 16, which is what made the
two-half reading of `+0x08` certain in the first place.

Smaller results from the same pair of runs:

- `+0x10C` and `+0x20C` **keep an SRAM address** written to them.
- `+0x138`: wrote `0x1AA`, read `0x800001AA`. The 9-bit level field is where
  §16 says; bit 31 is hardware-set and unexplained.
- `+0x104`: wrote `0x00100001`, read `0x00100000`. The watermark field holds
  and **bit 0 does not stick** — write-only or self-clearing. The vendor
  writes it the same way; do not test for it.
- **Bit 11 of `+0x108` clears when a descriptor is armed** and returns after
  the transfer. That is an idle flag, and the cheapest witness that a transfer
  is in progress.
- Module 37 and romclk 19 **survive a re-upload**, so a probe's "cold" pass is
  only cold after a power cycle. The same is true of the `0x400E0000` gate.

**Next**: which of the four routes is the speaker and which the headphone
jack. `ToneDemo` walks all four and prints a mean completion time per route; a
route that pushes the mean towards 25000 µs is one being paced by a real
sample clock, and that is a stronger answer than an impression of a sound.

### MEASURED, third run — and the bit clock, which was never read

All four output routes wrote cleanly — `+0x08` went from `0x00000000` to
`0x20832083` and friends — and every one still retired a 25 ms buffer in a
**mean of 10 µs**. 4796 bytes in 10 µs is 480 MB/s on a 64 MHz Cortex-M4:
not a transfer, a descriptor being read and discarded.

That run carried a methodological bug worth recording next to §14a's. The
sweep OR'd each route on top of the last, because the vendor's setter is a
read-modify-write and the sketch never cleared `+0x08` between attempts —
route 1 should have been `0x2043` and read `0x20C3`. **Only route 0 was
actually tested.** A sweep over a read-modify-write register has to reset it
between settings, and this is the second time in these notes that a sweep
measured something other than what it thought.

**What was missing is the vendor's stream start, `0x00D9662C`** — a different
routine from the init at `0x00D96710`, and one nothing in this project had
read:

```c
module_clock_disable(2); delay(10);
module_clock_enable(2);  delay(10);
romclk_set(44, 8);       /* ROM 0x2B1A */
romclk_enable(44);       /* 0x40080094 |= 1 */
0x40009400 |= 0x80;
/* and one of: 0x40009080[11:9] = 6, 0x4000907C[26:24] = 6, 0x4000907C[5:3] = 6 */
```

`ROM 0x2B1A` is in the mask ROM's **third** clock family, the setter
dispatched from `0x289C` — id and value, as against the enable-only family at
`0x20EC` that `sl6806_romclk.h` tabulates and the module ids at `0x1C5C`.
That makes three distinct clock mechanisms in this ROM, and only two of them
were known. Its whole body for id 44:

```
0x40080094[10:8] = value - 1
```

**And the arithmetic identifies it.** The vendor passes 8:

    24,576,000 / 8 = 3,072,000 = 64 x 48000
    22,579,000 / 8 = 2,822,375 = 64 x 44100

Both master-clock families land on exactly 64 fs, the bit clock for 32-bit
stereo frames. A divider producing 64 fs from either is the audio bit clock.

The three 3-bit source fields the same routine touches have a reset value of
4 — `+0x07C` and `+0x080` both come up `0x24924924`, which is every 3-bit
field at 4 — and the vendor moves exactly one of them to 6. Which one is
playback is unknown; `examples/ToneDemo` sweeps all three against the four
routes and times all twelve.

Deliberate deviation in `sl6806_audio_clock_start()`: the vendor cycles module
clock 2 off and on, this only enables it. Turning off an unidentified module
clock from the code doing the turning off is what `sl6806_module.h` has a rule
about. If the clock needs the cycle, that is the first thing to try.

### MEASURED, fourth run — twelve for twelve, and the third functional clock

All twelve route/clock-source combinations, and every register holding exactly
what was written to it:

```
PLL sel  (0x40080010) = 0x00103060    [6:5] = 3, as written
PLL rat  (0x40080014) = 0x8C498C00    ratio [30:14] = 0x3126, bit 31, bit 10
bitclk   (0x40080094) = 0x00000701    divider 8, enabled
DAC      (+0x008)     = 0x20832083 / 0x20432043 / 0x00020002 per route
ctrl     (+0x108)     = 0x12BC0910    the descriptor, every time
```

and **a mean of 10 µs, identically, in all twelve**. Neither the route nor the
bit clock nor the source mode changes anything.

This is the third time this project has met the same shape, and
`cores/sl6806/sl6806_module.h` describes it in its own opening paragraph:

> A module clock gives a peripheral its REGISTERS. It does not give it its
> FUNCTION. That distinction cost this project two peripherals and a lot of
> bench time, and both failures looked identical from the outside.

§14a's PWM: every register write accepted, counter never ran. §7n's camera:
full geometry configuration accepted and read back, no MCLK. Audio now: every
register accepted, no pacing. For the camera the answer was `0x400E0000`
bit 6; for the PWM it was a clock enable nothing in the vendor's firmware
writes.

**Only four of `0x400E0000`'s 32 bits are attributed** — camera 6, Bluetooth
0, the `0x400E2300` cluster 1, and 5 taken by the group bring-up — and until
this same day the register would not hold a bit from a payload, so nobody
could walk it. `examples/BtProbe` changed that.

`examples/AudioWall` walks it with the audio buffer's completion time as the
witness: set one clear bit, play four buffers, time them, restore the
register, move on. It only ever *sets* bits and never clears one that was
already set — turning off a functional clock something else is using, the USB
the console rides on for instance, is the one way that walk could take the
device with it.

It also asks something the playback direction cannot: **does the DMA engine
touch memory at all?** A capture buffer is pre-filled with a pattern, handed
to the RX descriptor, and checked afterwards. If the pattern survives, the
completion flag is not a transfer, and §16's "the descriptor path runs" is
weaker than it reads. Capture is the only direction where the engine can be
caught in the act, which is why `sl6806_audio_capture()` exists at all.

**Refactor while here:** the group bring-up moved from `sl6806_bt.c` to
`sl6806_module.c` as `sl6806_periph_group_begin()`, with the power handshake's
constants alongside it. Module 46 gates the whole `0x400Exxxx` window rather
than Bluetooth, and audio now needs the same gate for the walk — keeping it in
the Bluetooth driver had stopped being honest about what it is.

### MEASURED, fifth run — RETRACTION, and what audio was actually missing

`examples/AudioWall`. Three results, in order of how much they change.

**1. RETRACT "the descriptor path runs".** The capture test — pre-fill a
buffer, hand it to the RX descriptor, wait, count what changed:

```
after 100015 us: 0 of 64 words changed
```

**The engine does not touch memory.** So §16's completion flag is not
completion: a descriptor is accepted and retired, which is exactly as
consistent with a transfer that never happened. `SL6806_AUD_IRQ_DONE` is
probably an error or abort flag; an instant raise on a block with no data path
fits every observation. What survives is narrow and still real: the length
register holds what it is given, and `+0x10C`/`+0x20C` hold an address.

**2. All 32 bits of `0x400E0000`, no effect.** Every one gave an identical
witness. `0x400E0000` is not audio's gate. (The walk's own witness was
degraded — the baseline read 50005 µs, which is `(100000 + 100000 + 10 + 10)
/ 4`: two of every four buffers timed out and two returned instantly, an
alternation the earlier `ToneDemo` runs never showed. Worth understanding
before the walk is trusted as a *sensitive* negative, though a bit that fixed
the block would still have moved it.)

**3. WITHDRAWN WITHIN THE HOUR: "the audio block does not move its own
data".** `0x00D97ED8` claims one of eight DMA channel slots, enables module
clock 33 and configures `0x40001000 + ch*0x40` — the block §7c describes as
`{ctrl, src, dst, len}` and §14b found. It is the only code in the audio
HAL's address region that loads `0x40001000`, and that was taken as evidence
that the audio FIFO is fed by the general DMA controller.

Its one caller in FIRM is `0x00D3EAC0`, whose literal pool holds
`lcdc_dma_write`, `lcdc_set_descriptor` and `lv_lcd_init`. **It is the LCD
controller's DMA setup.** `0x00D97ED8` is a shared HAL routine and its only
identified user is the LCDC; "it sits in the audio region" was proximity, not
evidence — the same mistake in kind as §7c filing `0x40009000` as the timers,
made in the same session that corrected it.

So what audio is missing remains **open**. Ruled out by measurement: the
output route, the bit clock, and all 32 bits of `0x400E0000`. Still true and
still not evidence about audio: there is a DMA controller at `0x40001000`
behind module clock 33, and at least the LCDC uses it.

The way to settle whether audio goes through it is to find what fills the DMA
config struct on the audio path. `0x00D3EAC0` builds a recognisable one for
the LCDC — request id 9, source width 32, destination width 16, burst sizes 8
and 4, and a completion callback — so an audio equivalent would be legible if
it exists.

**Method note, third of these now.** The witness was chosen well — a capture
into a pre-filled buffer is the one test that can catch a DMA engine in the
act, and it overturned a conclusion three runs of playback-only evidence had
built up. §14a's lesson was "do not trust a status bit as a success test";
this adds two more. **A completion flag is not a transfer** - only a test that
observes memory can say that. And **a literal's address region is not its
owner**: the DMA claim above survived about forty minutes, and would have
survived into a hardware run if the caller had not been checked.

## 18. The PWM's register spec, re-inspected — and one row of §14a corrected

Static, from the blob's accessors. Prompted by the audio work turning up a
third mask ROM clock family nobody had used, on the theory that the PWM's
stalled counter might have the same cause. It does not — but the inspection
found a misattribution worth fixing.

### `0x00811EC0` does not write the channel's control register

§14a's channel table files `src | (div << 8)` under `+0x00`, the control
register. `cores/sl6806/sl6806_pwm.h` files the same write under the *pair*
register at `0x40084010 + (ch >> 1) * 4`. They cannot both be right, and the
two readings collide on bit 8 — which §14a needs as the update trigger and
`sl6806_pwm.h` measured as the pair clock enable.

The accessors settle it. All of them fetch their target from one cached
pointer table at SRAM `0x0082B3F8`, and they index it differently:

```
0x00811D30   update trigger    r0 = T[ch + 4]   -> 0x00811E62, sets bit 8
0x00811D40   poll update       r0 = T[ch + 4]   -> 0x00811E6C
0x00811D50   src | div << 8    r0 = T[ch + 1]   -> 0x00811EC0
```

Three slots apart, so **different registers**. And the layout falls out of the
arithmetic: six channels and three pairs give `T[1..3]` = the three pair
registers and `T[4..9]` = the six channel controls, with `T[0]` the global at
`+0x00` of the block. The `src | (div << 8)` caller is therefore indexed *by
pair*, not by channel, which is exactly what a per-pair register is.

**So `sl6806_pwm.h` is right and §14a's table row is wrong.** The row is
struck through above. This also retro-explains the measured writable mask:
the pair register only retains `0x10F`, so `src` is `[3:0]` and the "divider"
is the single bit 8 that turned out to be the pair clock enable — the bit
twelve hardware sessions went without.

### And the negative that prompted the look

The PWM code in the blob (`0x00811CE4`–`0x00811EC0`) makes **no call to any of
the three mask ROM clock families**. Scanning the blob and stage 1 for calls
to `0x008051E8` (the setter, ROM `0x289C`), `0x008051EC` (the enable family,
ROM `0x20EC`) and `0x008051F0` (its disable) puts every single site inside one
region, `0x00806800`–`0x00807100`, which is the vendor's **system clock init**
— a routine that disables a long list of clocks and then enables the ones the
product needs.

Two things follow. The PWM does not ask for a clock of its own, so the audio
bit clock's explanation does not transfer to it. And more generally: **a
payload never runs that clock init.** The boot ROM brings up enough for USB
and SRAM; `0x00806800`+ is where the vendor's firmware configures the rest of
the tree, and nothing in this framework has ever executed it or transcribed
it. That region is worth a section of its own, and is a better lead for the
PWM counter than anything §14a has left.

## 19. The vendor's system clock init, read — and it is not the answer either

Static, from the SRAM blob. Followed up because §18 found that every call site
for all three mask ROM clock families lives in one region, `0x00806800`–
`0x00807100`, which a payload never executes — a promising-looking explanation
for both the stalled PWM counter and the unpaced audio DMA.

**It is a clock-tree *reconfiguration* path, not a bring-up.** The shape:

```
romclk_off  17 65 18 19 56 57 20 44 51 48 37 22 36 23 24 50 25 26 34 27 35
            28 29 30 32 40 38 52 54 61 63    (and a second, shorter sweep)
module_off  39, 37, 88
set         id 5 = 1, id 4 = 1, id 6 = 1, id 64 = 1
...
module_on   39, 37, 88
romclk_on   0, 63, 39, 21, 2, 38, 41
set         id 3 = 1, id 4 = 1, id 5 = 1, id 6 = 1
```

Disable a long list, change some dividers, put it back. That is a
frequency-change or suspend/resume routine, and it reveals no enable that this
framework is missing. Ids 3..6 and 64 are core/bus dividers set to 1.

Two things worth keeping out of it:

- **Module 37 (audio) and 39 and 88 are cycled here as a group**, which is at
  least consistent with 37 being a real module id for a real block.
- **romclk 44 — the audio bit clock divider — is in the disable sweep**, so
  the id is genuine and known to the vendor's clock manager.

Neither changes what a payload has to do. **This lead is closed.**

### And a caveat on §5's capture test, which was overstated

The run recorded "0 of 64 words changed" as showing the DMA engine cannot
touch memory. `sl6806_audio_begin()` performs the vendor's *playback*
bring-up: it sets `+0x100` bit 0 and never opens the capture direction the way
the mode-1 open does. The codec dispatcher's only writes to `+0x200` are bits
6, 7, 14 and 15 — no analogue of the TX enable — but "RX was never opened the
way the vendor opens it" is still true, so the test corroborates rather than
proves.

**The load-bearing evidence is the timing, and it stands alone.** 4796 bytes
in 10 µs is 480 MB/s; 100 MB/s would be 48 µs and 25 MB/s 192 µs. A 64 MHz AHB
does not move half a gigabyte a second. Whatever `+0x108` retires in 10 µs, it
is not 4796 bytes of memory.

## 20. The DMA controller is the LCD controller's, and nobody else's

Static, from the call graph rather than from address ranges — which is the
point, given §19's caveat and the retraction in the commit before it.

§18 raised the possibility that the audio FIFO is fed by the general DMA
controller. Following every entry point kills it:

| Entry point | Callers |
|---|---|
| `0x00D97ED8` channel setup (claims a slot, module clock 33, `0x40001000 + ch*0x40`) | `0x00D3EAC0`, whose literal pool holds `lcdc_dma_write`, `lcdc_set_descriptor`, `lv_lcd_init` |
| `0x0080DA74` start (§14b) | `0x00D998CC` and `0x0080E7FE`, both on the object at SRAM `0x0082B3BC` |
| `0x0080DAC8` abort, `0x0080DADE` enable, `0x0080DAFC` remaining | none |

And `0x0082B3BC` is settled rather than inferred: `0x00D99692` is
`strd r3, r0, [r2]` with `r2 = 0x0082B3BC` and `r3 = 0x400D9000`, so the
object is `{ base = LCDC, cfg }`.

**The general DMA controller has exactly two call paths in the entire image
and both are the LCD controller.** Nothing in the audio path touches it. That
is a disconfirmation, not an absence of evidence.

It also says something positive about the audio block: **it moves its own
data.** Address, length, watermark and start all live in its own register
window, which is not the shape of a FIFO fed by an external engine. Its own
engine is what is not running.

### The elimination table for audio, as it stands

| Candidate | Status |
|---|---|
| Output route at `+0x08` | ruled out, measured, 4 settings |
| Bit clock at `0x40080094` | ruled out, measured, with 3 source modes |
| A functional clock in `0x400E0000` | ruled out, measured, all 32 bits |
| The general DMA controller | ruled out, call graph |
| The vendor's system clock init | ruled out, §19 - it is a reconfiguration path |
| **The EQ sub-block's clocks, held** | **untried** |

The last row is the hypothesis with the most behind it. The vendor's init
takes module clock 32, romclk 45 and bit 2 of `0x40000020`, clears the
coefficient RAM at `+0x400..+0x5FC`, and gives all three back — and its stream
start then sets `0x40009400` bit 7, a register inside the range it had just
been treating as RAM. If that sub-block is in the playback path it needs those
clocks held while streaming, and nothing in the vendor's code holds them
because the application programs an EQ preset through a path this analysis has
not followed.

`sl6806_audio_eq_hold()` does it; `make SKETCH=examples/ToneDemo
EXTRA_FLAGS=-DTONEDEMO_EQ_HOLD=1` tries it, with the same completion-time
witness. If that does not move the number either, the next thing to read is
the application's EQ path — `HAL_eq_open`, `hardware EQ open success` — which
is the one part of the audio subsystem nobody has followed.

### MEASURED — the EQ hold moves the register window, and that is the lever

`examples/ToneDemo` with `-DTONEDEMO_EQ_HOLD=1`, holding module clock 32,
romclk 45 and bit 2 of `0x40000020`. **The first thing this session to change
the audio block's behaviour at all**, and every change points away from a fix:

| | before | after |
|---|---|---|
| `+0x400` | `0x00000000` | `0x003378B1` |
| `+0x108` | `0x12BC0910` | `0x00BC1F7A` |
| `+0x008` route 0 | `0x20832083` | `0x00832083` |
| mean completion | 10 µs | 3 µs |

The two register changes are one change. The length field at `+0x108 [31:16]`
is written `0x12BC` and reads back `0x00BC`; `+0x008`'s channel-1 half loses
bit 13, which is register bit 29. **Bits [31:24] stop accepting writes across
both registers.** A length truncated from 4796 to 188 bytes is why the
transfer got faster rather than slower — 3 µs is a shorter transfer, not a
better one.

So §20's last candidate is ruled out: holding those enables is harmful, and
the vendor's init borrows them for the length of a memset for a reason.

**What it buys instead** is the only lever the block has responded to in five
runs. Something among those three moves the register window, and
`examples/AudioWindow` separates them: all eight combinations, a known value
written into the length field each time, reporting whether the top byte
survives and what `+0x400` reads.

The interesting outcome would be the pad-mux bit alone. `0x40000020` is the
pad/pin function mux (§7c), not a clock, and a mux bit that changes which
register bits are implemented is a **window switch** — which would make
`+0x400` a second view of the same address space rather than a coefficient
RAM, and would explain both why the vendor holds it so briefly and why
`+0x400` reads `0x003378B1` while it is held.

This needed `sl6806_module_disable()` — ROM `0x1CE8`, whose order is the
reverse of the enable's: **gate first, then shadow**. Turning a clock off is
the dangerous direction, so the sketch only switches off an id it switched on
itself.

## 21. DECODED: bit 2 of `0x40000020` switches the audio block's register window

Measured 2026-08-13, `examples/AudioWindow`. The first outright decode in five
hardware runs rather than another elimination.

```
m32 rc45 pad2 |  length reads  |  +0x400
 0   0    0   |  0x12BC ok     |  0x80
 0   0    1   |  0xBC  TRUNC   |  0x3378B1
 0   1    0   |  0x12BC ok     |  0x80
 0   1    1   |  0xBC  TRUNC   |  0x3378B1
 1   0    0   |  0x12BC ok     |  0x80
 1   0    1   |  0xBC  TRUNC   |  0x3378B1
 1   1    0   |  0x12BC ok     |  0x80
 1   1    1   |  0xBC  TRUNC   |  0x3378B1
```

Module clock 32 and romclk 45 make no difference in any of the eight
combinations. **Bit 2 of `0x40000020` alone** decides whether the length field
at `+0x108 [31:16]` keeps a 16-bit value or only its low byte, and whether
`+0x400` reads back the `0x80` we wrote there or `0x003378B1`.

`0x40000020` is the **pad/pin function mux** (§7c). A mux bit that changes
which register bits are implemented is a window switch, not a clock — which is
why §19's clock-family sweep and §20's call graph were both looking in the
wrong place for it.

This explains the vendor's init exactly. `0x00D94AA6`..`0x00D94AEA` takes
module 32, romclk 45 and this bit, clears `+0x400..+0x5FC` one word at a time,
and hands all three back. Only the third of those is load-bearing: it is how
you reach the coefficient RAM, and while it is held the ordinary registers are
not all present. Holding it during streaming — which §20 proposed and this
disproves — truncates the DMA length from 4796 bytes to 188.

**It survives a re-upload.** `AudioWindow`'s baseline line read `0xBC` before
its own first row cleared the bit, because the previous `ToneDemo` EQ build
left it set. That is the same persistence the module clocks and the
`0x400E0000` gate have, and it is more dangerous here because the symptom is a
silently shortened transfer rather than a register that reads oddly.
`sl6806_audio_begin()` now clears it before anything else, and
`sl6806_audio_coeff_window()` is the deliberate way in.

### The question it opens

A window switch implies a second view of the address space, not just an
exposed RAM. If any register **outside** `+0x400..+0x5FC` reads differently
with the bit set, that is a register this framework has never seen — and a
control register nobody has touched is the right shape of explanation for a
block that accepts every descriptor and moves no data.

`AudioWindow`'s second half reads the whole map in both windows and prints
only the differences. Read-only, one register per `loop()` call.

### CORRECTED, same day — it is module 32 AND the pad bit, not the pad bit alone

§21's table was taken with module clock 32 silently already on, inherited from
the `ToneDemo` EQ build, and its `m32=0` rows never turned it off — so they
measured `m32=1` while reporting `m32=0`. A second run from a known state:

```
m32 rc45 pad2 |  length reads  |  +0x400
 0   0    1   |  0x12BC ok     |  0x80        <- differs from the first run
 0   1    1   |  0x12BC ok     |  0x80        <- differs from the first run
 1   0    1   |  0xBC  TRUNC   |  0x3378B1
 1   1    1   |  0xBC  TRUNC   |  0x3378B1
```

**The coefficient-RAM window needs module clock 32 and bit 2 of `0x40000020`
together.** Either alone leaves the ordinary registers in place. romclk 45
really does nothing to the window; it presumably clocks the EQ engine rather
than the aperture. Everything else in §21 stands — the vendor's init takes all
three for the length of a memset, holding the window during streaming
truncates the DMA length, and both halves survive a re-upload.

**Third strike for inherited state.** In this one block it has now produced:
an `AudioProbe` "cold" pass that was not cold; a `ToneDemo` run whose length
field was truncated before the sketch wrote anything; and a decode that named
the wrong cause. All three came from a sketch assuming the machine was in the
state it left the factory in.

So, alongside §5's *"a completion flag is not a transfer"* and §20's *"a
literal's address region is not its owner"*: **establish state, do not inherit
it.** `sl6806_audio_begin()` closes the window before anything else, and
`examples/AudioWindow` prints the state it starts from.

`tests/host/test_audio.c` needed one fix for the same reason: it asserted
"shadow before gate" by matching bare register names, and begin()'s new window
close puts another module's gate write in the trace first. It now matches on
module 37's own value, `0x20`. The test had been passing for a slightly wrong
reason and started failing for the right one.

## 22. The coefficient window is a 24-bit RAM over the whole aperture

Measured 2026-08-13, `examples/AudioWindow` second half: all 61 mapped
registers read in both windows.

**Every single one differs**, including ones that read `0` with the window
closed, and **every switched value fits in 24 bits** — largest seen
`0xFB6E49`.

Two arithmetic checks name it. With the window open the reads are the closed
values masked to 24 bits:

```
+0x008   closed 0x20832083   open 0x00832083    = closed & 0xFFFFFF
+0x108   closed 0x12BC0800   open 0x00BC1F6A    0x12BC0800 & 0xFFFFFF = 0xBC0800
                                                so the length field reads 0xBC
```

So §21's "the length field truncates to 8 bits" was never a narrowed field.
**The whole aperture is replaced by a memory 24 bits wide.** A
read-modify-write of the length hits RAM, and `0x12BC << 16` does not fit in
24 bits.

**The fingerprint that settles it:** `+0x10C` reads `0x00827A24` with the
window open. That is the buffer address `examples/ToneDemo` printed in the
EQ-hold run — `buffer at 0x827A24` — and `+0x10C` is exactly where
`sl6806_audio_play()` writes it. The write went into the window and is still
there, several sessions later. That is memory, and it kept what it was given.
An SRAM address in `0x800000..0x8FFFFF` needs exactly 24 bits, so it stored
without loss.

### This voids the EQ-hold run

Every register write that run made — the four routes, the DAC enables, the
descriptor, the source modes — went into this RAM instead of into the block.
It measured nothing about the audio path. Its "3 µs mean" is the time to
retire a descriptor the block never received, and its register read-backs were
reads of RAM. The only durable content of that run is the evidence above.

That is the fourth reading this session to need withdrawing, and the third
caused by state rather than by reasoning. It is also the one that produced the
decode, so the run was not wasted — but nothing in it should be cited as
audio behaviour.

### What it does and does not settle

24 bits wide is what an audio DSP's coefficient memory is, which fits the
vendor clearing `+0x400..+0x5FC` through this window and nothing else. The
aperture is wider than those 128 words: `0x000..0x5FC` all read distinct
values, with no aliasing at `0x200` or `0x400`.

The question the second half was built to answer — is there a control register
nobody has seen behind the switch? — is answered **no**. The aperture is
memory while the switch is open and the block's own registers while it is
closed. Nothing hides there.

So the elimination table stands as it did, one row longer:

| Candidate | Status |
|---|---|
| Output route at `+0x08` | ruled out, measured |
| Bit clock at `0x40080094` | ruled out, measured |
| A functional clock in `0x400E0000` | ruled out, measured, all 32 bits |
| The general DMA controller | ruled out, call graph |
| The vendor's system clock init | ruled out, §19 |
| A hidden register behind the window | ruled out, §22 |

The block accepts a descriptor and retires it in 10 µs having moved nothing,
with every register holding exactly what the vendor's own code puts there.

### Two more static leads closed, 2026-08-13

**`0x00D94B0C` is a no-op in the playback path.** It was the one function in
the audio stream setup nobody had read. It takes a three-bit mask and, per
bit, sets bit 0 / bit 4 of `0x40009030` and polls bit 29 of `+0x50`, `+0x54`,
`+0x58` — a power-up-and-wait for three analogue channels, i.e. the microphone
front end. The playback path calls it from `0x00D97A4C` with **r0 = 0**, after
a loop that exits when the state byte reaches zero, so it returns immediately.
Nothing there for playback.

**No LDO rail belongs to audio.** Every caller of the register-file write
accessor (`0x00804E44`) in the whole of FIRM lives in `0x00CC7xxx`, which §7m
identifies as the register-file/LDO driver itself. Nothing in the audio HAL or
in `driver_audio` ever asks for a rail. So the "the analogue side has no
supply" idea has no support in the vendor's code — the codec's supply, if it
has a separate one, is not switched by software.

With those two closed, the static reading of the audio subsystem is
substantially exhausted. What is left unread is the application's EQ path
(`HAL_eq_open`, `hardware EQ open success`, `audio_crab`), which is a
software-side abstraction over the coefficient RAM §22 decoded, and is
unlikely to contain a missing hardware enable.

## 23. FOUND: the SD/MMC host is `0x40003000`, and §7c's row is wrong

Everything in this section is from `dump.bin` and `maskrom.bin`. **None of it
has been run on hardware.** Driver in
[`cores/sl6806/sl6806_sd.h`](../cores/sl6806/sl6806_sd.h), full write-up in
[docs/SD.md](SD.md), 72 host tests.

### The row that was wrong, and the three ways it was wrong

§7c files `0x400F7000` as the "storage host (SD/MMC + SPI flash)" on this
evidence: *"`+0x100`/`+0x104` command registers with a bit-31 start/busy,
`+0x108` argument, `+0x10C`/`+0x110` response, `sdio(e):rx error` strings
nearby"*. Each clause is a true statement about something, and none of them is
about the SD host.

- `0x400F7000+0x100` is the mailbox to the chip that carries the power rails.
  §7m decoded it while hunting the camera's LDO and did not come back to
  correct §7c. It has a command word with a bit-31 start, an argument and a
  response — which is why it read as an SD host to a search looking for
  exactly that shape. Two sections of this file have described the same four
  registers as two different peripherals since 7m was written.
- The `sdio(...)` strings are at file `0xE6B5`–`0xEB60`, **below** `0x10000`.
  They are in the HLKJ bootloader, not in FIRM. "Nearby" was measuring
  distance in a file where two programs sit end to end.
- The SPI flash half of the row is untouched by this: the bootloader really
  does drive `0x400F7000` for flash, which is what put the base in the map in
  the first place.

### Where it actually is

Twice in four megabytes, both times a literal feeding a store into a driver
handle:

```
mask ROM   0x0003D398    0x40003000
bootloader 0x00822958    0x40003000   (HAL_sd_disk_init, 0x00822768)
```

Every other access goes through the handle. §7c's own blind-spot paragraph
predicted this — *"if a peripheral is missing, look for the table it is cached
in"* — and the literal-frequency scan that ranked `0x40009000` and
`0x400E2000` could not have ranked a base that appears once per image.

### The driver is in the mask ROM, again

Same shape as §7f's GPIO: flash carries a disk layer (mid-buffers, a flush
timer, a mutex, `sdio(i):`/`hal_sd(i):` logging) and every register access is
a call into the ROM around `0x00004000`. The whole stack is there — command
layer, identification, CSD parse, a polled FIFO drain, and a bring-up
function that does pads and clocks in one place (`0x0003D3A0`). docs/SD.md
tabulates the eleven entry points.

### What the bring-up says about the clock families

```
pad_configure(0x00016110)   bank 1 pin 12, function 2, drive 1, no pull
pad_configure(0x0001711B)   bank 1 pin 14, function 2, drive 1, pull 11
pad_configure(0x0001691B)   bank 1 pin 13, function 2, drive 1, pull 11
module_clock_enable(36)     15b's family - the registers
romclk_enable(17)           0x40080080 bit 0 - the function
install IRQ 44
```

So **module id 36 and ROM clock id 17 belong to the SD host**, which is two
more rows for tables that were mostly legend-free: §15b's 128 module ids had
four attributions before this, and `sl6806_romclk.h`'s 56 entries had two.
Both were found the same way — by reading the one function that turns a
peripheral on, rather than by walking.

Three pads is a one-bit bus. The same three ids appear with function 15 at
`0x0003D2F0`, the routine that parks them, so function 15 is the off state.

Note what is *not* in that list: `0x400E0000`. The SD host is not in the
`0x400Exxxx` group (§17), and its functional clock is the second family
rather than the third. If the block turns out to be configured-and-not-running
the way the PWM (§14a) and the camera (§7n) are, that is where to look next —
but unlike those two, there is no positive reason here to expect it.

### The register map, and a correction to how these blocks are read

Offsets from `0x40003000`: CMD at `+0x04` with bit 31 as start/busy, ARG at
`+0x08`, BLKSIZE `+0x0C`, DLEN `+0x10`, DTIMER `+0x14`, DCTRL `+0x2C`, STA
`+0x34` write-1-clear, RESP0..3 at `+0x38`..`+0x44` with **`+0x44` holding
bits [127:96]**, a second status at `+0x48` carrying the FIFO flags and the
echoed command index, CLKCR at `+0x4C`, and the FIFO as a single port at
`+0x200`.

Two things in that are worth carrying to the next peripheral:

1. **The response registers are MSB-last.** `+0x44` is bits [127:96], not
   `+0x38`. Established not by guessing but by the ROM's own CSD parse
   (`0x3E5E`) reading CSD_STRUCTURE — which is bits [127:126] — out of what it
   had stored from `+0x44`. When a block has four response registers, the
   parse of a known structure is what fixes their order.
2. **A single-port FIFO with a "half full" bit is a polled path.** `0x4806`
   drains 512 bytes in groups of eight words, each group gated on STA bit 5
   and each acknowledgement asking for the next eight. This is the first
   block in this chip found to have a complete PIO path, and it is why the SD
   driver needs neither DMA nor interrupts — where the audio block (§16) has
   only descriptors.

### The command word table is irregular, and that is informative

ROM `0x40F4` maps an index to a 16-bit word, and it is not `0x2540 | index`.
CMD0 is `0xA400`, CMD2 and CMD9 carry a long-response bit, CMD12 swaps bit 13
for bit 14, and the read commands clear bit 10 where the write commands set
it. Reading the fields off the table gives a plausible layout — response
expected, long response, data phase, direction, multi-block, abort — with one
hole: bit 8 is set for everything but CMD0 and CMD1, which reads like "check
the response CRC" until you notice ACMD41 answers R3 exactly as CMD1 does and
has the bit set. docs/SD.md has the full table.

### One place the transcription deliberately departs from the ROM

Every one of the ROM's response waits tests a wide error mask before it tests
the timeout bit, and the timeout bit is inside the wide mask — so an
unanswered command comes back as the generic error 32, and the ROM recovers
the real cause afterwards by reading the status register a second time and
looking at bit 8 by hand (`0x4388`, to tell an MMC from an SD card). The
driver tests the timeout first, and clears the ROM's whole acknowledge mask
on every path rather than the one bit it happened to test. Same bits read;
the difference is that "nobody answered" survives as itself rather than
needing a status register to still be intact when a caller looks. That
distinction is the whole of how an empty slot is told from a controller that
is not running, and it is the first thing `examples/SdProbe` reports.

### [M] The first hardware run, 2026-08-13 — the ROM fails at the same instruction

`examples/SdProbe` on a P20 Player, no card in the slot:

```
module 36 reads ENABLED
CTRL   0x40000000      CMD    0x0000A400
DTIMER 0xFFFFFF40      DCTRL  0x00000000
STA    0x00000000      STA2   0x00000106
CLKCR  0x20070008
result: 3 (no response)      -- CMD0 never completed
```

That is the **cold** dump, before this framework's driver touched anything.
Read it against what the transcription says writes each value:

| Register | Cold | Written by |
|---|---|---|
| CTRL | `0x40000000` | ROM `0x3D9A`, `edge(0) \| CLKEN` |
| CMD | `0x0000A400` | the CMD0 word from ROM `0x40F4`, start bit cleared |
| DTIMER | `0xFFFFFF40` | ROM `0x3E22`, verbatim |
| CLKCR | `0x20070008` | the identification value, divider 7 |

So the mask ROM brought this block up itself before the payload started, sent
CMD0, waited out its 65536-iteration poll and gave up — leaving exactly the
state the driver then reproduced, which is why the "after bring-up" dump is
identical to the "cold" one.

Two conclusions, pointing opposite ways:

- **The decode is confirmed by the device.** Five registers hold the five
  values §23 predicts. A wrong base cannot produce `0x20070008` and
  `0xFFFFFF40` and a CMD0 command word by accident, and this is the same
  class of evidence as the register file's rails decoding to real supply
  voltages (§7m).
- **The transcription is not what is wrong.** The vendor's own driver, in
  mask ROM, running before anything else on the chip, fails at the same
  instruction. Nothing in `cores/sl6806/sl6806_sd.c` can fix that.

Which puts the SD host exactly where the PWM (§14a) and the camera front end
(§7n) are: configured and not running. `examples/SdWall` walks `0x400E0000`
with "does CMD0 complete" as the witness — the smallest thing the controller
can be asked to do, needing no card, no data path and no response.

**One measurement the first probe did not take**, and the reason `SdProbe`
now takes it before anything else: whether a write to this block *sticks*.
Every register above already held the value the driver was about to write, so
"unchanged after bring-up" is compatible both with a live block and with a
gated one dropping writes silently — which is how the ADC was written off for
four runs (§15b). The probe now writes two nonces to `+0x08` and reads them
back before and after bring-up.

Also worth noting for the walk: **CTRL bits 0 and 1 read back as zero** even
though both the ROM's bring-up and the driver's set them along with the reset
bit. Either they are a self-clearing kick or they are not writable, and the
probe now says which.

### [M] `0x400E0000` is a seven-bit register, and none of its bits is the SD host's

`examples/SdWall`, 2026-08-13, with "does CMD0 complete" as the witness:

```
0x400E0000 = 0x00000020     after sl6806_periph_group_begin()
bits 0,1,2,3,4,6 tried      no CMD0 completed
bits 7..31                  DO NOT HOLD
```

The second line is the finding. **`0x400E0000` implements bits [6:0] and
nothing above them** — every write to bits 7..31 is dropped. Four of the seven
are already attributed (0 Bluetooth, 1 the `0x400E2300` cluster, 5 the
group's own, 6 camera), and the walk tried all seven.

Two consequences beyond the SD host:

- **The SD host has no functional-clock bit here.** That is this chip's usual
  answer *eliminated*, not untested, which is a better position than the audio
  block is in.
- **§14a's and §16's open item shrinks from 28 bits to three.** Every walk of
  this register so far, including `examples/AudioWall`'s, has been walking 25
  bits that do not exist. The unattributed ones are **2, 3 and 4**.

The bootloader's four differences from the ROM's bring-up — `CTRL[31:30]` =
`0xC0000000`, the run-speed CLKCR, both together, and clearing bit 0 of
`+0x18` — were tried in the same run and none of them completed a command
either.

### A caution about "cold" dumps, and a correction to the section above

**Uploading a payload does not reset the chip.** The boot ROM keeps running
and drops the new image into SRAM, so every register a previous sketch touched
is still touched when the next one starts.

That undercuts part of what the first hardware run was read as saying. The
SdProbe dump was interpreted above as showing the *mask ROM's* SD bring-up
values, and it is equally consistent with an earlier payload in the same power
session having left them — the fingerprint is identical, because the payload
runs the ROM's sequence. What survives from that reading is the part that does
not depend on provenance: **five registers hold the five values §23 predicts,
so the decode of `0x40003000` is confirmed**. What does not survive is "the
mask ROM's own driver fails at the same instruction", which is plausible and
unproven.

There is a clean discriminator, and it costs one power cycle. The ROM's SD
bring-up tears itself down when it fails (`0x0003D5CC` calls `0x0003D2F0` on a
non-zero return, which parks the three pads on function 15, disables module 36
and clears `0x40080080` bit 0). So after a *fresh* boot into bootloader mode,
with `examples/SdProbe` as the first payload run:

| module 36 reads | What it means |
|---|---|
| ENABLED, registers holding `0x20070008`/`0xA400` | the ROM's init ran and returned success — which with no card in the slot it should not have |
| disabled | the ROM tried and tore down, and the values seen on 2026-08-13 were a previous payload's |
| disabled, registers zero | the ROM never touches the SD host in bootloader mode, and the block is simply cold |

### The SD clock register, decoded — and the ROM leaves most of it alone

ROM clock id 17 is `0x40080080`, and the mask ROM's own accessors decode the
whole register:

| Field | Meaning | Where |
|---|---|---|
| bit 0 | enable | `0x223C` sets, `0x2614` clears |
| bit 4 | source select: set → source 8, clear → source 59 | `0x372E` |
| bit 8 | divide by 8 | `0x2D64` |
| bits [19:16] | divider *n* | `0x2A2A` writes it |

and the rate query at `0x2D64` computes the total as `(n + 1) × (bit8 ? 8 :
1)`, while the setter at `0x2A2A` encodes a requested divide the same way:
above 16 it shifts right by three and sets bit 8.

**The ROM's SD bring-up sets bit 0 and nothing else.** It never programs a
divider for this clock, and its one call to the source selector passes 10 —
which that dispatcher does not recognise for id 17, since the id 17 case only
tests for 8 and 59 — so nothing is written. The SD clock therefore runs on
whatever the reset defaults are, which is fine for the ROM (it runs at power-on
with the tree in a known state) and is not obviously fine for a payload
arriving in bootloader mode. `examples/SdClock` sweeps all 64 settings of that
register with CMD0 as the witness, and then CLKCR's own divider field.

### [M] The clock is not it either — and CTRL bit 1 holds while bit 0 does not

`examples/SdClock`, 2026-08-13. The clock tree as found, which nobody had
recorded:

```
0x40080080 = 0x00000001     enable on, source 59, no prescale, divider 0
                            -> total divide 1, the fastest setting available
CRU +0x40 = 0x00000009      +0x48 = 0x00000051      +0x60 = 0x00000002
everything else in +0x44..+0x5C and +0x7C..+0x8C reads zero
```

So the SD clock was already enabled, on the default source, undivided. All 64
settings of that register — source 8 and 59, prescale on and off, divider 0
through 15 — were swept with CMD0 as the witness, and so were twelve CLKCR
configurations across both of its `[30:23]` values. **Nothing completed a
command.** The clock into the block is not the missing piece.

The useful line in that run was somewhere else:

```
CTRL <- 0x40000003 (no reset bit)  reads 0x40000002
```

**Bit 1 holds. Bit 0 does not.** And after a full bring-up CTRL reads
`0x40000000`, with bit 1 clear as well — so bit 1 holds when written on its
own and does not survive the bring-up.

Now read ROM `0x3D88` again, which is the only reset in the dump:

```
CTRL |= 7                  bits 0, 1 and 2 in a single store
while (CTRL & 4) ;         wait for bit 2 to clear
```

If bit 2 resets the block, it takes bits 0 and 1 with it, and **the vendor's
bring-up sets two bits and immediately wipes them** — the ROM's and the
bootloader's both, and §23's transcription faithfully reproduces the order.
Nothing in this project or in the vendor's code has ever set those bits
*after* the reset, and one of them is now known to hold if you do.

`examples/SdCtrl` tests it: which CTRL bits are writable at all, then sixteen
combinations of the four bits the ROM ever touches (0, 1, 3 and 25) applied
after bring-up, then every other writable bit on its own. It reports three
things per attempt rather than one — what CTRL read back, **whether the
command's start bit was ever seen latched**, and the status — because "the
start bit did not latch" and "it latched, cleared, and the status stayed
empty" are different faults and only the second is about clocks.

Ways the hypothesis could still be wrong, all of which that sweep answers:
bit 0 may be a self-clearing kick that already did its work; `[1:0]` may be a
two-bit field in which 3 is not a legal value; or the order may be right and
the block disabled for an unrelated reason.

One method note worth keeping. The line above was printed by a test whose
condition was `(ctrl & 3)` and whose message was "they stick" — which is true
of a register where only one of the two stuck, and hid the finding for as
long as it took to read the hex. **A probe that reduces two bits to one
boolean can only report what it was already expecting.** Both this sketch and
`SdProbe` now print the bits separately.

### [M] The command register is live: `start LATCHED` in every configuration

`examples/SdCtrl`, 2026-08-13. Thirty-odd configurations of CTRL — sixteen
combinations of the four bits the ROM touches, applied *after* the reset, then
every other writable bit alone, then a bare reset followed by the enables as a
separate write — and every line reads the same:

```
start LATCHED   STA 0x00000000   CMD 0x0000A400
```

**The start bit is taken when it is written, and has cleared by the time the
poll gives up.** So the block is not gated, is not stalled waiting for a
clock, and is not refusing the command: it accepts it, drops the busy bit,
and the status register reports nothing.

The same run mapped CTRL, which nothing had:

```
writable: bits 1, 3, 4, 24, 25 (and 30/31, the configuration field)
bit 0: NOT WRITABLE
```

That retires the hypothesis the sketch was built on. "The reset wipes the
enable bits" cannot be true of bit 0, because there is no bit 0 to wipe. Bit 1
does hold after a full bring-up and does *not* hold after a bare reset with no
clock configuration, which is a real difference and not obviously an important
one.

Also worth recording: after a bare reset CTRL reads `0x00000000`, so the reset
clears bit 30 — the one bit the clock configuration sets — as well.

**Three hypotheses are now closed by measurement**, which is worth stating
together because each one looked like the answer when it was proposed:

| Ruled out | By | How completely |
|---|---|---|
| a functional-clock bit in `0x400E0000` | `SdWall` | the register is seven bits wide; all seven tried |
| the clock into the block | `SdClock` | 64 settings, and it was already enabled and undivided |
| a CTRL enable the reset destroys | `SdCtrl` | every writable bit, before and after the reset |

### Where that leaves it, and the method change

Every sketch above tested one hypothesis with one witness, and the supply of
plausible hypotheses is now exhausted. `examples/SdScope` has none: it
snapshots all 72 registers of the block, sends one CMD0, snapshots again, and
prints what moved — the method that found the DVP's DMA destination register
(§7n), and the right one once the question has become "does this block execute
anything" rather than "which bit enables it".

It also captures 32 back-to-back samples of the status and command registers
with no formatting in between, because a bit that sets and self-clears in a
few cycles is invisible to a poll loop that prints as it goes; and it sweeps
the five CLKCR bits **no bring-up in the dump ever writes** — the vendor's
read-modify-write clears `0x7FFF0FFF` and leaves bits 12..15 and 31 alone, so
nothing has ever set them.

**One thing has not been controlled for and should be**: whether a card is in
the slot. CMD0 needs no card in principle — it expects no response and the
controller only has to shift 48 bits out — but a host with a card-detect
input may decline to run its clock into a slot it knows is empty, and if so
every run so far has been asking a controller to talk to nothing. The
card-detect pin is unknown (§23), so this cannot be checked in software. Two
runs of `SdScope`, one with a card and one without, settle it in a minute.

### [M] The block takes commands and discards them in under a microsecond

`examples/SdScope`, 2026-08-13. All 72 registers snapshotted, one CMD0 sent,
snapshotted again:

```
registers that bring-up moved:      +0x004  0x0000A400 -> 0x00008020
registers that the command moved:   +0x004  0x00008020 -> 0x0000A400
32 back-to-back samples afterwards: STA 0 and CMD's busy bit already clear
                                    in the very first one
CLKCR bits 12,13,14,15,31:          all DROPPED - those bits do not exist
```

**Exactly one register moved, and it was the one that was written.** Read that
next to `SdCtrl`, which read the command register one MMIO access earlier and
saw the start bit set: both are true, and together they say the busy bit lasts
**less than one register access, about 300 ns**. At the identification clock,
divider 7, shifting a 48-bit command out takes tens of microseconds. The block
is not executing the command slowly or waiting on anything — it is discarding
it.

### The question none of it answers, and the test that does

Every register read so far is on the *register interface*, which module 36
clocks and which demonstrably works: writes stick and read back. **Nothing
that has been measured says whether the logic behind those registers is
running.** A peripheral with a clocked bus interface and a stopped core looks
exactly like this — it takes writes, returns them, and does nothing.

`examples/SdLife` tests that directly and without reference to SD at all:

1. **Sample all 72 registers 200 times and report any word that ever differs
   from itself.** Counters, FIFO levels and state machines all show up here.
   Nothing moving is a fact, where "a command failed" is an inference.
2. **Push words into the FIFO at `+0x200` and watch the flags in `+0x48`**
   (bit 2 receive-empty, bit 3 transmit-full, from ROM `0x464A`/`0x4658`).
   This exercises the datapath with no card, no command and no bus to the
   slot. **Flags that move mean the core is alive and the fault is in the
   command path; flags that do not mean the core is stopped**, and no amount
   of SD protocol work matters until that changes. This is the sharpest test
   available.
3. **The six unused bits of CLKCR's `[30:23]` field.** The vendor uses two
   single bits of it — 29 to identify, 28 to run — and the other six have
   never been set by anything.

If the core turns out to be stopped with module 36 and ROM clock 17 both on,
the remaining candidate is a third clock, and the place to look is the CRU's
own divider registers at `+0x40` and `+0x48` — which read `0x09` and `0x51`
on this device and which nothing in this project has ever written.

### Method: the console loses the head of a long report

Four runs in a row opened with `[lost output - device outran the poll rate]`,
and each time it was the register dump that went missing. The console is a
2 KB ring that **overwrites rather than blocking** (deliberately — a sketch
must not stall because nobody is running the monitor), and in `RUN_MODE=poll`
the host only drains it between `loop()` calls. So any sketch that prints more
than about two kilobytes without returning loses the beginning of it, every
time.

`examples/BtProbe` already solved this and nothing else copied it: a state
machine with one section per `loop()` call, guarded by
`sl6806_console_space() >= SL6806_CONSOLE_SIZE / 2`. `SdLife` is written that
way. Any future probe that prints a register map should be.

### [M] The core IS clocked: the FIFO works. The command path is the fault

`examples/SdLife`, 2026-08-13, and the first positive result this block has
produced:

```
STA2 before 0x00000106     rx-empty 1  tx-full 0
pushed 16 words, STA2 now 0x00200109
*** THE FLAGS MOVED ***
nothing moved on its own across 200 passes over 72 registers
CLKCR [30:23]: all eight bits hold; none of them starts a command
```

Sixteen words went into the data port at `+0x200` and the block noticed.
Decoding the change to `+0x48`:

| Bit | before | after | reading |
|---|---|---|---|
| 0 | 0 | 1 | pairs with bit 1 |
| 1 | 1 | 0 | pairs with bit 0 |
| 2 | 1 | 0 | FIFO empty — clears once there is data |
| 3 | 0 | 1 | FIFO full — sets at exactly 16 words |
| 21 | 0 | 1 | a level field in the high half |

**The FIFO is sixteen words deep, its flags respond, and the datapath has a
clock.** That retires the whole "the core is stopped" line of enquiry, which
five sketches had been circling. Bits 0/1 flipping as a complementary pair
alongside 2/3 suggests two views of the same fullness — the register carries
more than the two flags ROM `0x464A`/`0x4658` use.

Reading the FIFO back returned zeros, so the read port is not simply the write
port; either the two directions are separate FIFOs or reads are only served
during a data phase.

Nothing moving on its own is consistent rather than contradictory: with no
transfer running there is nothing to count.

So the position is now precise. **The register interface works, the datapath
works, and the command state machine discards commands in under 300 ns.**

### The card clock is testable without an oscilloscope

A command shifts out on the *card* clock; the FIFO runs on the core clock.
`SdLife` proved the second one only. And the card clock is observable from
software, because this chip's pad controller has an input register that reads
a pad's physical level **regardless of the function it is muxed to** (§7f,
bank base `+0x010`). So a pad the SD controller is driving can still be read by
the CPU, and a pin carrying a clock, sampled a few thousand times, comes back
as a mixture of ones and zeros where a parked pin comes back constant.

`examples/SdPads` samples the whole of bank 1 four ways: before the SD pads
are configured, with the controller up and idle, while CMD0 is being issued
repeatedly, and with pins 12/13/14 taken back as pulled inputs. It answers:

- **does pin 12 toggle** — if it does, the clock is leaving the chip and the
  fault is on the far side of the pad; if it does not, the controller is not
  driving its clock, which no register dump could have established;
- **do pins 13 and 14 sit high** under the ROM's own pull selector, which is
  what a CMD and a DAT0 line should do;
- **which pins change when a card is inserted** — run it twice and diff. Any
  pin that differs is wired to the slot, which is this board's SD pinout, and
  a pin that changes on insertion and does nothing else is the card-detect
  contact. Neither is known (§23).

### Tooling: the bootloader is now reachable from the repository

All of the above came out of a hand-extracted copy of the HLKJ bootloader in
a scratch directory, which nobody else could reproduce. `tools/sl6806-boot`
does it properly — the mapping, both CRCs, strings at run addresses, a literal
cross-reference and a disassembler front end:

```
tools/sl6806-boot dump.bin --xref 0x40003000
    0x40003000 referenced by 1 literal load(s)
       loaded at 0x008227f0   (pool 0x00822958)
```

which is §23's central finding, reproducible in one command. It exists because
the bootloader is where a lot of the interesting code is and no tool reached
it: `tools/sl6806-xref` scans FIRM, and every `sdio(...)` string in the image
is below file `0x10000`. 36 tests in `tests/tools/test_boot.py`, including
that the bias round-trips and that a single flipped byte fails the CRC — a
wrong bias produces addresses that look perfectly plausible and are off by a
constant, which is exactly what cost §7h five searches.

### [M] Software is blind to the SD bus too — and a test that should not have been written

`examples/SdPads`, 2026-08-13, asked whether the card clock toggles on bank 1
pin 12 by sampling the pad input register while the pad was muxed to function
2. Every phase that used function 2 reported the pin stuck low; the phase that
took the three pads back to function 0 under the ROM's own pull read all three
**high**.

The second reading is real: **the pads exist and their pull-ups work.** The
first three mean nothing, and the reason is recorded three sections above in
this same file, from `examples/PadScope`:

> the input register reads 1 in function 0 and function 14, and 0 in every
> other function … there is no function where the controller is connected
> *and* the pin is readable

`docs/LCD.md` states the consequence in as many words — *"no software test can
watch that bus. Anyone tempted to write another one should read this table
first"* — and that is exactly what happened: a sketch was written on the
assumption that §7f's description of the input register meant it always
reflects the pad, when the measured behaviour was already on file. The
hardware run cost was a whole session's worth of turnaround.

**The conclusion generalises: software is blind to the SD bus for the same
reason it is blind to the LCD bus.** Whether the card clock leaves the chip
cannot be answered from a payload. It needs a probe on the socket.

The sketch is kept and its header now opens with this, rather than being
deleted, because the reasoning inside it is a tidy example of a plausible test
resting on an assumption nobody re-checked.

### The pull table at ROM 0x0006501C, decoded

§7f found the table and left its contents unread. Twelve entries, selectors
4..15; each word's bits [1:0] go to the "pull a" register at bank `+0x24` and
bits [17:16] to "pull b" at `+0x2C` (ROM `0x736`):

| selector | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| pull a | 1 | 1 | 0 | 0 | 2 | 0 | 0 | 2 | 3 | 3 | 3 | 3 |
| pull b | 2 | 3 | 0 | 0 | 0 | 0 | 0 | 2 | 0 | 1 | 2 | 3 |

Selector 8 is known to hold a plain input high (docs/LCD.md), so **a = 2 is
pull-up**, a = 0 is no pull, and **a = 1 is [I] pull-down** — which makes
selector 4 the only clean pull-down in the table. a = 3 is [?], possibly a bus
keeper, and is best left alone.

Having a pull-down matters: it is what turns "this pin floats" into "this pin
is driven", and that is the only card-presence test available while the bus
pads themselves are unreadable.

### `examples/PadMap`: the census, and the card-detect hunt

Stays inside function 0, where software can see.

1. **A census with no writes at all** — every pin's function nibble and level
   across all six banks. Nobody has recorded what this board's pads are set to
   in bootloader mode, and it is the missing half of the README's "this
   board's pinout" item: the pad controller has been done since §7f, the
   pinout has not.
2. **A pull test on pads already in function 0** — pull up, read, pull down,
   read, restore. A pin that follows the pull is floating; one that ignores it
   is driven from outside. **A microSD socket's card-detect contact is a
   switch to ground**, so a pin that follows its pull with an empty slot and
   reads low with a card in it is card detect.

It never drives a pad and never touches a pad outside function 0 — the notes'
own warning is that driving all 192 wedges the device, since something USB
needs is among them.

Run it twice, once with a card and once without; the diff is this board's SD
pinout and its card-detect pin.

### [M] The pad census: 80 bonded pads, and the boot ROM assigns six

`examples/PadMap`, 2026-08-13, reading every bank's function nibbles with no
writes at all. The first pad map this project has had:

| bank | base | pads that exist | assigned by the boot ROM |
|---|---|---|---|
| 0 | `0x40081000` | pins 0..5 | 0,1,2,3 → function 2 |
| 1 | `0x40081040` | pins 0..31 | 9 → function 3 |
| 2 | `0x400F6080` | pins 0..5 | 0,1 → function 2 |
| 3 | `0x400810C0` | pins 0..12 | none |
| 4 | `0x40081100` | pins 0..16 | none |
| 5 | `0x400F6000` | pins 0..5 | none |

Every bank shows the same shape: a contiguous run of **function 15** from pin
0, and **every pin above that run reads 0 in the function nibble**. Six banks
independently producing that pattern is not a coincidence, so:

- **`0` in a function nibble means the pin does not exist** — the register
  file is 32 pins wide per bank and the package bonds fewer;
- **`15` means a real pad the boot ROM has parked**, which matches §23's
  finding that the ROM parks the SD pads on function 15 when it tears the SD
  block down.

**Eighty pads are bonded out and the ROM assigns six of them.** Those six are
the entire pinout that bootloader mode needs: four on bank 0 and two on bank 2,
all function 2, plus bank 1 pin 9 on function 3. `[I]` the bank 0 four are the
SPI flash the bootloader runs from and the bank 2 pair is USB, which would be
consistent with `0x400F7000` being the flash host — but nothing here
establishes that, and the pads carrying them are unreadable in an alternate
function anyway.

This also explains why the sketch's first pull test measured nothing: it
tested "pads already in function 0", which is to say every pin that is not
there, and correctly reported that all of them read 0 under both pulls. The
pads that exist are the parked ones. The test now targets those, and
configures them properly through `sl6806_pad_configure()` (ROM `0x93C`) rather
than writing the pull registers of a pad that is still parked — a parked pad
ignores its pull, which is the second half of why nothing moved.

⚠ **A census is only the boot state on a freshly powered device.** Uploading a
payload does not reset the chip, so pads an earlier sketch configured stay
configured: this run showed bank 1 pins 12, 13 and 14 in function 0, which is
`examples/SdPads` having left them as inputs, not anything the ROM did. Same
caution as §23's cold-dump correction, and the same fix — power-cycle first.

### [M] The parked-pad pull test, empty slot — and the boot ROM's own two inputs

`examples/PadMap` aimed at parked pads, 2026-08-13. Forty-six of the sixty-nine
parked pads follow their pull, so the mechanism works; the rest are driven from
outside the chip:

```
bank 1 pin 0     LOW          bank 2 pins 2,3,4   LOW
bank 1 pin 10    HIGH         bank 2 pin 5        HIGH
bank 1 pins 16,17,19..31  LOW     (18 floats)
bank 3 pin 0     LOW
```

This is the empty-slot half of the card-detect test; the diff against a
card-in run is the result, and has not been taken yet.

**Bank 1 pins 10 and 11 are the boot ROM's own inputs**, which is worth having
independently of SD. ROM `0x3C1F0` configures pin 10 as **function 14** with
pull selector 11 — a pull-up — then sets an interrupt mode (`0x9BC`, bank
`+0x218`), enables it (`0x9D2`), and polls the pending bit (`0xA02`, bank
`+0x214`) five hundred times before giving up. ROM `0x3C19C` does exactly the
same for pin 11 with selector 5, a pull-down.

A pin with a pull-up that the ROM waits for an edge on is an active-low button,
and **pin 10 reading HIGH in this run is that button not being pressed**.
docs/DUMPING.md still describes the boot key as "trial and error"; bank 1
pin 10 is the first candidate anything in this project has produced for it.

That pair also explains §"the pad input buffer is off in almost every alternate
function" from the other end: **functions 0 and 14 are the two that leave the
buffer alive because both are input modes** — 0 a plain input, 14 an
interrupt-capable one. The table was measured without a reason; this is the
reason.

The many bank 1 pins reading LOW against a pull-up (16, 17, 19..31, with 18
floating) are `[I]` unused pads tied to ground on the board, which is ordinary
practice — but note it sits against §"0 means the pin does not exist", since
bank 1 is the one bank whose function nibbles read 15 all the way to pin 31.
Either bank 1 really has 32 bonded pads and sixteen of them are grounded, or
the `0`/`15` reading needs refining. The card-in diff will not settle that; a
board photograph would.

### [M] Two runs, one pin different — and a bug that makes it unreadable

Third run of `examples/PadMap`. Diffed against the second, **exactly one pin
differs across the whole board**:

```
bank 1 pin 10:   HIGH (up=1 down=1)   ->   (up=0 down=1)
```

Every other driven pin, and all forty-six floating ones, read identically.
That is a clean single-variable result, and it is still not usable, for two
reasons — one about the board and one about the sketch.

**The board reason.** Bank 1 pin 10 is the pin ROM `0x3C1F0` waits for an edge
on: pull-up, function 14, interrupt enabled, five hundred polls. That makes it
the boot-key candidate *and* the only pin that changed. Whether it moved
because a card went into the slot or because the boot key was held differently
between two power-ups cannot be told from these logs. Both runs need their
conditions stated: card in or out, key held or not.

**The sketch reason, which is a defect.** `up=0 down=1` is not a level. It is
the exact inverse of following the pull, and that is what a pad reads when it
is sampled *before it has settled*: the first version applied a pull and read
the input register in the next instruction. An internal pull is a weak
resistor into whatever capacitance the board puts on the pin — a long trace, a
switch, a connector — and needs microseconds. Every "driven" verdict in the
previous two runs is therefore suspect, though a pin held hard at ground will
not lag and the forty-six that followed their pull clearly did settle.

`pad_settled()` now waits 500 µs, then samples eight times and reports 2 if the
level never holds still, and the verdicts are `HIGH`, `LOW`, `UNSTABLE` and
`INVERTED` rather than a bare high/low. `INVERTED` appearing again would mean
something stranger than settling.

The general lesson is the same one §"the pad input buffer is off" taught, from
a different direction: **a two-sample test cannot tell a level from a
transition**, and every probe in this tree that reduces an analogue reality to
one boolean has eventually had to be rewritten.

### [V] Bank 1 pins 10 and 11 are the boot ROM's two mode inputs

Following the two edge-waiters found above to their callers, at ROM `0x3D8C0`
and `0x3D922`, gives the shape of boot-mode selection:

```
0x3D8B8   loop for ~1 second:
0x3D8C0       if edge on bank 1 pin 10  -> tail-call 0x3D750
0x3D8E4   then sample bank 1 pin 11's LEVEL for up to 999 ms,
0x3D90A       and store it as a byte of boot state
0x3D922   if edge on bank 1 pin 11      -> tail-call 0x3D63C
```

So **each pin gets about a second at boot and each branches somewhere
different**, and pin 11's level is additionally recorded as a flag. Neither
branch is the SD bring-up at `0x3D5CC`, so these are not card-reader mode
directly.

`docs/DUMPING.md` describes entering bootloader mode as "hold a boot key while
plugging in USB (key varies, trial and error)". This is that mechanism: two
inputs, both on bank 1, both function 14 — pin 10 with a pull-up (so an
active-low button) and pin 11 with a pull-down (active-high). It is the first
thing this project has found that explains the key at all.

### The card-detect result is one pin, and the measurement was defective

Runs two and three of `examples/PadMap` differ in exactly one pin out of
sixty-nine, and the conditions are now known: **boot key unchanged, card in the
slot for the third run.** So bank 1 pin 10 is the only pin on this board that
responded to card insertion.

That is exactly the card-detect signature — and it is also the pin the ROM
polls for a boot key, which is an awkward coincidence rather than a
contradiction (an SD socket's detect contact and a button are the same kind of
signal, and a boot ROM that wakes on card insertion is not unusual). Under the
lag model the readings say the pad's resting level goes from 1 with no card to
0 with a card, with an internal pull-up still able to drag it up slowly — a
weak pull to ground, not a hard short.

**But the numbers come from the version of the sketch that did not let the pad
settle**, so `up=0 down=1` is a transition caught mid-flight rather than a
level, and every "driven" verdict in those two runs is suspect. The fix is in;
the measurement has to be retaken. Two runs of the current build, card out then
card in, nothing else changed:

| pin 10 reads | meaning |
|---|---|
| follows the pull with no card, LOW with one | card detect, and it goes straight into `sl6806_sd.h` |
| the same both ways | the earlier difference was the settling bug, and the card-detect pin is still unknown |
| `UNSTABLE` either way | something is driving it actively — the boot ROM's own interrupt path is the first suspect |

### [M] The first clean census — and why diffing across a power cycle is useless

Fourth run of `examples/PadMap`, and the first from a genuinely fresh
power-up: bank 1 reads function 15 at pins 12, 13 and 14 with the input
register at zero, so `examples/SdPads`' leftovers are gone. **This is the boot
state**, and it confirms two things the earlier runs could only suggest:

- **the ROM parks the SD pads on function 15**, as §23 read out of ROM
  `0x0003D2F0` — pins 12/13/14 are parked, not unassigned;
- **bank 1 has 32 real pads.** Zero pins in function 0, where the other five
  banks have 15 to 26 of them. So `0` really is "not bonded" and bank 1 really
  is the wide bank.

Against the previous run, **ten pins differ**:

```
bank 1 pin 10   driven high  ->  UNSTABLE
bank 1 pin 11   floating     ->  UNSTABLE
bank 3 pin 1    floating     ->  LOW
bank 4 pins 0, 5, 6, 7, 8, 9, 10, 11   floating -> LOW
```

None of that is a card. **A power cycle changes the board's state**, and the
previous run was taken from a session in which earlier payloads had been
running. Diffing across a power cycle answers a question nobody asked, and the
instruction to "power-cycle first" — correct for a census — silently broke the
comparison it was meant to serve.

The protocol is now in the sketch's banner and header:

1. power off, hold the boot key, plug in — **once, at the start**;
2. run with the slot **empty**;
3. insert a microSD, **do not unplug and do not reset**;
4. run again.

Step 3 is safe for exactly the reason leftover pad state is a nuisance
elsewhere: the device stays in bootloader mode across an upload.

### UNSTABLE on both boot inputs, and what that is

The two pads that would not settle at 500 µs are **bank 1 pins 10 and 11** —
precisely the two the boot ROM waits for an edge on (§"the boot ROM's two mode
inputs"). Nothing else on the board came back unstable.

That is what a **button with a debounce capacitor** looks like to an internal
pull. A pull on the order of 100 kΩ into a cap on the order of 100 nF is about
ten milliseconds, and the fast pass allows 500 µs plus 160 µs of sampling — so
the level is still moving when it is read, every time. It is a positive
identification rather than a nuisance: the two pins the ROM polls as keys are
the two pins with debounce hardware on them.

`pad_settled()` now retries a pad that fails the fast pass with a 40 ms
settle — four RC constants of that estimate — and only pads that failed pay
for it.

### [M] RESULT: inserting a card changes nothing software can see

The card-detect test, run to its own protocol at last — one power cycle, slot
empty, then a card inserted without unplugging. **The two logs are identical
byte for byte**: same census across all six banks, same verdict on all
sixty-nine parked pads, same counts.

Two things follow.

**There is no card-detect pin among the pads this test can reach.** It could
still be one of the six the boot ROM has assigned (bank 0 pins 0..3 and bank 2
pins 0..1 on function 2, bank 1 pin 9 on function 3), which are off limits
because something USB needs is among them; or a pin in function 0, which is to
say not bonded; or nowhere at all, since plenty of microSD sockets have no
detect contact or have one the board does not route. `sl6806_sd.h` keeps
reporting card presence as a pair of command timeouts, and that is now a
measured limitation rather than an unexamined gap.

**The earlier "one pin differed" result is retracted.** Bank 1 pin 10 moved
between two runs that also differed by a power cycle, and it was measured by
the build that read a pad before it settled. Neither the pin nor the card had
anything to do with it. The finding that survives from those runs is the one
that came from the ROM rather than the meter: pins 10 and 11 are the boot
ROM's two mode inputs (§"the boot ROM's two mode inputs"), and they are the
only two pads on the board that will not settle under an internal pull even
after 40 ms — which is what a debounced button looks like, and corroborates
them from the other side.

**And the SD bus pads showed nothing either**, which is worth stating because
it was not the question and may be the more useful answer. Bank 1 pins 12, 13
and 14 follow their pull with a card in the slot exactly as they do with it
out. A card holds CMD and DAT up through its own pull-ups, so either those are
weaker than this chip's internal pulls — they are the same order of magnitude,
so quite possible — or **[?] bank 1 pins 12..14 are the chip vendor's default
SD pads rather than the ones this board routes to its socket.** The mask ROM
is generic to the part; the board is not. Nothing in the dump can settle that,
because the dump is the same for every board built on this chip. A photograph
of the socket's traces would.

That last possibility deserves weight in proportion to §23's open problem. The
SD host accepts a command and discards it in under a microsecond with its
datapath demonstrably clocked; a controller wired to pads that go nowhere is
one of the few explanations left standing that does not require the block to
be broken.

### CORRECTION, same day: the previous section is wrong. Bank 5 pin 0 moves

The section above says the card-in and card-out logs were identical and
concludes there is no reachable card-detect pin. **That was written from a
duplicated paste — the same log twice — and it is retracted.**

The real pair differs in exactly one pin of sixty-nine:

```
bank 5 pin 0     slot empty  ->  LOW (up=0 down=0, held to ground)
                 card in     ->  follows the pull (floating)
```

Everything else is identical: the census of all six banks, and every other
driven pin including bank 1 pins 10 and 11 UNSTABLE and bank 2 pin 5 HIGH. So
the protocol worked exactly as intended, and the single-variable result it was
built to produce is a pin nobody had been watching.

**Bank 5 pin 0 is the card-detect contact**, wired the less common way round:
the switch is closed to ground when the slot is **empty** and opens when a card
is inserted. Both polarities are built; this socket is the normally-closed
kind. Card present is therefore "reads high under a pull-up", and slot empty is
"reads low whatever pull is applied".

It is a candidate rather than a fact until it has been run once more —
insert and remove twice, confirming the pin follows the card both ways —
because it rests on one pair of logs, and the last thing that rested on one
pair of logs was bank 1 pin 10, which was wrong. But it is the first thing in
this investigation that a card has moved.

What has not changed from the retracted section is the loose thread, and the
card-detect result sharpens it: **the SD bus pads still show nothing.** Bank 1
pins 12, 13 and 14 follow their pull identically with a card in and with it
out — while a pad on a completely different bank registers the card
mechanically. A card holds CMD and DAT up through its own pull-ups. Either
those are weaker than this chip's internal pulls, or **[?] bank 1 pins 12..14
are the chip vendor's default SD pads rather than the ones this board routes
to its socket.** The mask ROM is generic to the part; the board is not, and
the board demonstrably has the socket wired to bank 5 pin 0 for detection.

That matters to §23 in proportion. The SD host accepts a command and discards
it in under a microsecond with its datapath demonstrably clocked, and a
controller driving pads that do not reach the socket is one of the few
remaining explanations that does not require the block to be broken.

**Method note, and it is the third of its kind in this file.** The retracted
section was written in the same minute as the log arrived, from two blocks of
text that looked like two runs. Two identical logs and one duplicated log are
indistinguishable by inspection — the check that would have caught it is
asking whether *anything at all* differed, since two runs of a sixty-nine-pad
sweep agreeing to the byte is itself the surprising outcome and deserved more
suspicion than it got.

### [M] Bank 5 pin 0 is bistable, and the sketch now labels its own logs

A third run puts bank 5 pin 0 back in the floating state. Across the three
logs taken since the protocol was fixed it has been seen in both states and it
remains **the only pin of sixty-nine that varies at all** — everything else,
census included, is identical run to run.

Bistability on exactly one pin is what the card-detect candidate needed; what
it still needs is the states tracking the card, which requires knowing which
state each run was taken in.

**And that is where this investigation has actually been losing, twice now.**
The result is a diff between two runs, and the two runs are told apart by
whoever pastes the logs remembering which was which. Once that produced a
mislabel and once a duplicated paste analysed as a pair, and each cost a
retracted conclusion — the second one within a minute of being committed. The
measurements were fine both times. The bookkeeping was not.

So `examples/PadMap` now tests bank 5 pin 0 **first** and prints the verdict at
the top of every log:

```
--- card detect: bank 5 pin 0 ---
  up=1 down=0   ->  CARD PRESENT   (pin floats: switch open)
```

A log that says which state it was taken in cannot be mislabelled, and the
diff stops depending on anyone's memory. This is a cheaper fix than any of the
three method notes above it, and it should have been the first thing written
once the answer turned out to be a two-run comparison.
