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
- **SRAM: `0x00800000`–`0x008FFFFF`, one megabyte.** Measured on hardware in
  bootloader mode (2026-08-07): `0x008FFF80` returns data, `0x00900000` and
  everything above it returns zeros, no address below `0x00900000` aliases any
  other, and re-reading an address returns the same bytes. The old figure of
  256 KB was the size of the first `tools/sl6806-dumpram` run, not a measurement.
  The stock firmware relies on this: its LVGL framebuffer is at `0x0087B800`
  (§13).
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
  0xB1-0xB6, 0xDF-0xEC and 0xF1-0xF6, so it is a vendor variant rather than a
  stock ST7789. The tables live in `variants/p20_player/panel.c` and are
  checked byte for byte by `tests/host/test_panel.c`.
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
| `0x00047080` | 4/14 | `gpio_write`, power sequencing at `0x00D44AB8` |
| `0x00047880` | 4/15 | same routine |

Other groups worth naming: bank 1 pins 1-8 are the LCD's QSPI pads
(function 2) with a matching teardown to function 15; bank 4 pins 0-15 are a
sixteen-pad function-2 group; bank 3 pins 1-6 are function 11; bank 1 pins
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
| `0x40009000` | timers | channels at 0x100 stride (`+0x108`, `+0x208`) with write-1-clear flags and per-channel callbacks; register triples at 0x20 stride |
| `0x40070000` | DMA **control only** | per-channel IRQ status at `+0x24`/`+0x2c`, callback table indexed by channel, request routing at `+0x00`/`+0x20`/`+0x28`. The data path is **not** here — see `0x40001000` below |
| `0x40001000` | **DMA channel registers** | 8 channels at `+ch*0x40`, `{ctrl, src, dst, len}`; §14b |
| `0x40084000` | **PWM** | 6 channels at `+0x20 + ch*0x20`; channel 3 is the backlight; §14a |
| `0x40080000` | **clock & reset unit** | dividers at `+0x40`/`+0x48` with a bit-31 busy poll; module gates at `+0x64`/`+0x74` bit 15; see `cores/sl6806/sl6806_cru.h` |
| `0x40081000` + `0x400F6000` | **GPIO / pad controller** | six banks, bases in a mask ROM table at `0x00065004`; see §7f and `cores/sl6806/sl6806_padctl.h` |
| `0x400D9000` | **LCD controller** | the bootloader logs `HAL_lcdc_module_init` from the routine that caches this base; see §12b and `cores/sl6806/sl6806_lcdc.h` |
| `0x400F7000` | storage host (SD/MMC + SPI flash) | `+0x100`/`+0x104` command registers with a bit-31 start/busy, `+0x108` argument, `+0x10C`/`+0x110` response, `sdio(e):rx error` strings nearby |

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
| `+0x00` | control. bit 4 = **run**; bit 8 = update trigger, and the same bit is polled until clear; bit 28 polled as busy; `0x40` written at init; `0x00811EC0` writes `src \| (div << 8)` |
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

> ⚠ **CRU state survives a payload upload.** Uploading does not reset the clock
> tree, so a PLL left locked by one run is still locked for the next. One run
> was lost to a sketch that returned early on "already locked" and thereby
> skipped the step it existed to test. **Probes must write every step
> unconditionally**, and print a before/after readback, or a stale device looks
> like a negative result.

**PARKED, after seven runs. The counter clock is not reachable from a payload.**
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

**MEASURED, NEGATIVE — the ADC is readable but not writable.** It reads
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
`0x400E0000` (`0x00D9A734`), and that register is dead from a payload. The CRU
gates are a second, partial mechanism — they cover the LCDC completely, and
the PWM's registers but not its counter. Nothing reachable turns on the rest.

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

**This is worth re-running for the PWM too.** The backlight's functional clock
was hunted across three register pairs with the wrong write order and without
the fourth pair, so §14a's negative is not as complete as it reads.
