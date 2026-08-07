# Getting a good flash dump

A failed SL6806 dump does not announce itself. `smtlink_dump` exits 0 and
writes a file of exactly the size you asked for. If the device was in the
wrong mode, that file is the same 64-byte SCSI reply repeated to length — it
looks like a binary, it opens in a hex editor, and it contains no firmware
whatsoever.

**Always run the checker before analysing anything:**

```sh
tools/sl6806-checkdump dump.bin
```

## The command that works

Dump from **bootloader mode**, not card-reader mode:

1. Power the device off.
2. Hold a button — which one varies per unit, so it is trial and error.
3. Plug in USB while still holding it.
4. Confirm the mode: `sudo ./smtlink_dump inquiry` should report
   `SMTLINK DEVICE 2.00`. If it says `SSTLINK DDVICE 2.02` (with the vendor's
   typos) you are in card-reader mode — unplug and try another button.

```sh
cd 3rd/smartlink_flash && make
sudo ./smtlink_dump chip 6806 init flash_id read_flash 0 4M dump.bin
tools/sl6806-checkdump dump.bin
```

Three details matter, and each one on its own produces a silently bad dump:

| Detail | Why |
|---|---|
| `chip 6806` | The tool otherwise infers the chip from the USB serial, and one of the two known SL6806 serials (`20221008000002`) is commented out in its source. Without this it can decide "unknown chip" or pick SL6801 command variants. |
| `init` | Required in bootloader mode. It is also the thing that *hangs* in card-reader mode, which is why the mode has to be right first. |
| `4M` | SL6806 flash is 4 MiB. Dumping `2M` truncates: FIRM survives (it ends at 0x1C80D0) but PICS and FONT are lost. |

Do **not** pass `--id 301a:2801`. That is the SL6801 card-reader ID. Both
SL6806 modes enumerate as `301a:2800`.

## Running without sudo

Create `/etc/udev/rules.d/80-smtlink.rules`:

```
SUBSYSTEMS=="usb", ATTRS{idVendor}=="301a", ATTRS{idProduct}=="2800", MODE="0666", TAG+="uaccess"
SUBSYSTEMS=="usb", ATTRS{idVendor}=="301a", ATTRS{idProduct}=="2801", MODE="0666", TAG+="uaccess"
```

Then pass `--no-sudo` to the framework's tools.

## What a good dump looks like

```
  ok    size is 4 MiB, the full SL6806 flash
  ok    content varies across the file (not a stuck-response dump)
  ok    HLKJ bootloader magic at offset 0
  ok    HLKJ header CRC16 verifies
  ok    HLKJ payload CRC16 verifies
  ok    partition table at 0x0F000 names FIRM, PICS and FONT
```

## Dumping RAM and the mask ROM

A flash dump is not the whole device. Large parts of the SL6806 driver stack
— the LCD command writers, the GPIO writer, the delay routines — are never in
flash at all. Enumerating the application's calls shows 251 distinct entry
points in SRAM, and the HLKJ bootloader independently makes 393 calls to 100
entry points inside the **mask ROM** at `0x00000000`. The ROM is not just a
USB downloader; it is the vendor SDK's shared driver library.

```sh
tools/sl6806-ramcalls dump.bin           # the SRAM surface, ranked
tools/sl6806-ramcalls dump.bin --rom     # the mask ROM surface
```

**Bootloader mode** is where to read them: the boot ROM answers there, over
the same channel that already works for uploading payloads. Use `--probe`
before committing to a long dump.

```sh
tools/sl6806-dumpram --start 0 --size 0x7D000 --out maskrom.bin
tools/sl6806-dumpram --start 0x800000 --size 0x40000 --out sram.bin
```

**This has been done, and the results are in
[`sl6806_re_notes.md`](sl6806_re_notes.md) §7f.** In short:

- The ROM dump is genuine — 93% of its branches resolve internally — but it
  contains no LCD driver and does not hold the application's SRAM driver blob.
- It *does* contain the pad controller, which is what unblocked GPIO. The
  reason an early scan missed it: the ROM does not address GPIO as base plus
  offset from a literal, it looks the bank base up in a table at
  `0x00065004`. See §7f and `cores/sl6806/sl6806_padctl.h`.
- Nothing is resident at `0x0080E842` or `0x00811C7C` in bootloader mode. The
  SRAM dump is faithful there (its relocated vector table and the ROM's own
  stack both read back correctly), so this is a real absence rather than a
  failed read.

Re-running it on another unit is still worthwhile — the above is one device —
and `examples/RomProbe` does the SRAM half from a sketch without any of this
tooling.

Caveats worth knowing before you spend time on it:

- The read command works by writing a small trampoline to RAM and executing
  it, so it perturbs the very thing you are reading. Keep the dump target
  away from `0x003FB000`, and treat a single anomalous region with suspicion.
- Reading SRAM under the *stock application* (card-reader mode) does not
  work: the application services SCSI and stalls the vendor command
  (`LIBUSB_ERROR_PIPE`), and the attempt resets the device.
- Stop `fwupd` first. Its probing has been observed knocking this board off
  the bus about 1.3 s after it enumerates.

## Keep it

That dump is your recovery image. USB download mode lives in mask ROM rather
than in flash, so a bad flash write is recoverable — but only if you have the
original bytes to write back. Keep a copy somewhere that is not the working
directory, and verify it with `checkdump` before you rely on it.

## Troubleshooting

**`LIBUSB_ERROR_PIPE`** — an endpoint stall, meaning the device is not in a
mode that accepts the command. Usually: not in bootloader mode, or the kernel
driver was not detached.

**The dump is all one repeated block** — see the top of this document. Wrong
mode, or missing `chip 6806`.

**The device stops responding mid-session** — unplug, hold the boot button,
plug back in. Nothing in the payload workflow writes to flash, so there is
nothing to repair.
