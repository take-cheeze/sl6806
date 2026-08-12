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
ATJ2127. Whether *this* chip answers the same commands has never been
checked. That is the actual unknown.

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
