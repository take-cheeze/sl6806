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

## 5. Partition table (file 0xf000)

| Name | Flash off | Run addr     | Size      | Contents |
|------|-----------|--------------|-----------|----------|
| FIRM | 0x010000  | `0x00C10000` | 0x1B80D0  | application code (**the target**) |
| PICS | 0x1DB000  | `0x00DDB000` | 0x0E447C  | image/UI resources (data) |
| FONT | 0x2C0000  | `0x00EC0000` | ~0xCA39C  | glyphs (data) |

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

**Open thread:** finish decoding this header + find the "mark" magic and which CRC
covers the app body, to produce a valid SD-update file (the no-USB install channel).

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

## 12. Next actions (pick up here)

1. **Trace `lv_lcd_init` (`0x00D3E34C`)** → reconstruct `lv_disp_drv_t`: framebuffer
   address, flush callback, panel resolution + color depth. Needed to draw anything.
2. **Finish the FIRM/SD-update header** (§6): mark magic + body CRC → valid update file.
3. **Enumerate vtable slots / message-id enum** via the central `__act_on_request`
   dispatcher (turns `method_20` + siblings into named methods across all scenes).
4. **Build a Unicorn harness** to execute functions from the image.
