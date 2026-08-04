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
