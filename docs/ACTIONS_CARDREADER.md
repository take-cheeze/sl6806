# A different chip: the Actions-brand USB card reader

This document is about a device that is **not** the SL6806. It exists because
someone is likely to plug in a "USB CARDREADER" and expect this repository's
tools to work on it. They will not, and this explains why, and what the
starting point for a real attempt looks like.

## Why this isn't SL6806

```
$ lsusb -d 20d6:2101 -v
Bus 003 Device 111: ID 20d6:2101 ACTIONS USB CARDREADER
...
    Interface Descriptor:
      bInterfaceClass         8 Mass Storage
      bInterfaceSubClass      5 SFF-8070i
      bInterfaceProtocol     80
```

Two things rule this out as an SL6806:

- **The vendor ID is `20d6`, not `301a`.** Both SL6806 modes - bootloader
  (`301a:2800`) and card-reader (`301a:2801`) - live under Smartlink's ID.
  `20d6` belongs to a different silicon vendor: the descriptor string reads
  ACTIONS, i.e. Actions Semiconductor, whose chips are unrelated to the
  SL6806 and use a different boot ROM, a different USB download protocol, and
  a different flash layout.
- **It is a plain Bulk-Only Mass Storage device right now** (protocol `80` =
  0x50 = BOT), speaking SFF-8070i/ATAPI, same as SL6806 in card-reader mode
  looks superficially similar and equally unhelpful - the interesting mode is
  a vendor download mode this descriptor gives no evidence of.

None of `sl6806-*`, `smtlink_dump`, or anything in `cores/` applies here.

## What does apply: `3rd/actions_flash`

[`ilyakurdyukov/actions_flash`](https://github.com/ilyakurdyukov/actions_flash)
is a Linux dumper for Actions' **ATJ2127/ATJ2157** MP3-player chips, already
vendored as a submodule (`git submodule update --init 3rd/actions_flash`,
then `make -C 3rd/actions_flash`; needs `libusb-1.0-0-dev`).

Its README documents exactly the same two-mode shape as SL6806: a flash-disk
identity (`10d6:1101`) and, after holding the boot key while powering on, an
ADFU (Actions Device Firmware Update) identity (`10d6:10d6`) where the boot
ROM itself answers. The commands that talk to the raw boot ROM - `inquiry`,
`adfu_reboot`, `adfu_info`, `write_mem`, `switch`, `read_mem` - are sent
before any chip-specific binary is loaded, which is why they read as a
generic ADFU protocol rather than something written specifically for the
ATJ2127.

## What has actually been confirmed on real hardware

One unit has been tested (`lsusb -d 20d6:2101 -v` at the top of this file).
Findings, most useful first:

- **This is not really an "Actions" device or even a card reader.** `20d6` is
  officially registered to PowerA (a gaming-peripherals company); the
  `ACTIONS`/`USB CARDREADER` strings in the USB descriptors are unrelated
  defaults the firmware never customised. The SCSI INQUIRY vendor/product
  fields tell a different story: `HUASHENGDI` / `TECH RECPRO67`. Tracing
  that back, the hardware is a generic, unbranded IC recorder / voice
  recorder (32 GB, touch panel, one-button recording) resold under many
  different storefront names on Amazon/AliExpress - the kind of product with
  no dedicated PC software at all, because drag-and-drop mass storage is all
  it has ever needed to offer. No FCC ID, no model number, no vendor tool
  has been found for it (web search, GitHub code search, and Chinese 量产工具
  forums all came up empty).
- **The `0xcc` handshake matches ATJ2127/2157 exactly.** `actions_dump --id
  20d6:2101 adfu_reboot`'s first step (CDB `cc`, requesting 11 bytes) gets
  back `ACTIONSUSBD` byte-for-byte, with a GOOD status - real shared lineage
  with the chip this tool targets, not a coincidence.
- **The second step is rejected.** The follow-up CDB `cb 21 ... 02` (11
  bytes then a 2-byte confirmation read) gets back `ff 3d` where the tool
  wants `ff 00`; the SCSI status for that command is still GOOD, so the
  device processed the request and chose to answer this way - it is not a
  transport error. Confirmed with `dmesg`/`lsusb` that no reboot into ADFU
  actually happens: the device stays at `20d6:2101` throughout.
- **A second, independent implementation** ([nfd/atj2127decrypt](https://github.com/nfd/atj2127decrypt/blob/master/dfu/adfu.py))
  hard-codes the identical `cb 21` pair for this step, which argues it is a
  fixed protocol request rather than a per-chip submode byte - i.e. `0x3d`
  is most likely this chip's own status/rejection code for a request it
  understood, not evidence the wrong sub-command was sent. That makes
  brute-forcing `cdb[1]` a low-confidence move, not a promising one - see
  below for why it is still worth a few tries.
- **A protocol footgun worth knowing about separately from any of this:**
  `actions_dump`'s `adfu_reboot` does not read the second command's CSW
  status packet when the payload mismatches - it just prints "unexpected
  response" and exits. That leaves a stale status packet queued on the
  device's endpoint, which then corrupts the *next* `actions_dump`
  invocation's first read (observed directly: a subsequent `inquiry` read a
  stale `USBS` packet before the real 36-byte reply). Always replug the
  device between `actions_dump` attempts on this chip until that step
  succeeds cleanly.

## Trying alternate sub-commands (exploratory, low confidence)

Given the above, sweeping `cdb[1]` values other than `0x21` is not grounded
in documentation - there is none for this chip - but it costs little: this
specific command has not been observed to touch flash on any chip it has
been tried against, and the kernel's SCSI generic (`SG_IO`) layer frames each
attempt as a clean, self-contained transaction, so it cannot reproduce the
stale-CSW footgun above.

`tools/actions-cardreader-try-subcmd` sends one value at a time, deliberately
- so you can check `lsusb`/`dmesg` between attempts rather than looping
blindly:

```sh
# find the SCSI generic device node right after plugging in:
dmesg | grep 'Attached scsi generic'   # e.g. sg1

sudo tools/actions-cardreader-try-subcmd /dev/sg1 0x21   # reproduces ff 3d
sudo tools/actions-cardreader-try-subcmd /dev/sg1 0x01
sudo tools/actions-cardreader-try-subcmd /dev/sg1 0x00
```

A result worth reporting: `data` other than `ff 3d`, or the device
disappearing from `lsusb` right after (meaning it rebooted).

`tools/actions-cardreader-probe` automates finding out, safely:

```sh
git submodule update --init 3rd/actions_flash
make -C 3rd/actions_flash        # needs libusb-1.0-0-dev

tools/actions-cardreader-probe                 # inquiry only, prints the plan
tools/actions-cardreader-probe --yes           # also sends adfu_reboot
```

Without `--yes` it stops after `inquiry` - the point being that `adfu_reboot`
is the first command sent to a chip nobody has confirmed accepts it, and the
tool's own docstring plus `3rd/actions_flash/README.md` are worth reading
before that step, not after. With `--yes` it also diffs the USB bus before
and after the reboot to find whatever id the device answers as afterward
(rather than assuming `20d6:20d6` by analogy with `10d6:10d6` - it may not
be), then runs the read-only `adfu_info` query against it and reports the
two-byte chip id: a hit against `ATJ2127`/`ATJ2157` names it outright, a miss
prints the raw number, which is the thing worth recording and searching for
next.

## What is deliberately not attempted

`read_lfi` / `write_flash` in `actions_flash` require a chip-specific
`fwscfNNN.bin` this repository does not have for an unidentified chip, and
`read_nand` requires knowing the NAND geometry. Guessing either is how these
devices get bricked - see the warnings in `3rd/actions_flash/README.md`,
particularly the note that some units have a *software* power switch and can
be stuck in ADFU mode after unplugging, recoverable (if at all) only by
holding the boot key or waiting out the battery.

Once `adfu_info` names or numbers the chip, the safe next step is a **RAM**
read (not flash, not ROM - the boot ROM does not offer to read itself):

```sh
3rd/actions_flash/actions_dump --id <adfu_id> read_mem <addr> <size> out.bin
```

`<addr>` is chip- and SoC-specific and is not guessed here. If the chip turns
out to be an ATJ2127 or ATJ2157, `3rd/actions_flash/README.md` already has
worked examples. If it is neither, the two-byte id from `adfu_info` is the
starting point for finding out what it is.
