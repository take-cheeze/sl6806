# Flashing (and why you probably should not yet)

The payload workflow — `make upload` — loads your sketch into RAM and runs it
there. It never touches flash, so it cannot brick the device. **Everything in
this framework works in payload mode.** Use it.

This document is about the other path: replacing the vendor application in the
FIRM partition so your code runs standalone, off USB.

## The SD-update format is now decoded

The bootloader's `sdupdate` path - the no-USB install channel - has been read
out of the HLKJ bootloader, which unlike the application is stored verbatim in
flash (file `0x60` -> `0x0081FC00`, so file + `0x81FBA0` = address). The
validation function is at `0x00824110`.

An update file is `0:\update.up` (or `0:\restore.up` for a forced restore) on
the card. The checks it must pass, in order:

| Offset | Field | Check |
|---|---|---|
| +0x00 | `"CONFIG"` | `memcmp(file, "CONFIG", 5)` — the header magic |
| +0x06 | u32 `codeOffsetInByte` | added to `partition_start` to locate the payload |
| +0x16 | `"SL6806"` | `memcmp(file+22, "SL6806", 6)` — **the "mark"** |
| +0x20 | u32 `partition_start` | defaults to `0x3000` when zero |

The bootloader reads a 512-byte header block, checks the magic, prints
`header pass`, checks the mark, prints `mark pass`, then compares the file's
timestamp against the installed one: **matching timestamps skip the update**
(`time is not same` is the message on the path that proceeds).

**There is no body checksum to precompute.** The `crc cmp %x %x` message
compares two CRC16s that the bootloader computes itself - one over the data it
read from the file, one over what it read back from flash, both starting at
`0xFFFF`. It is a write-verify, not a stored field. That removes what looked
like the hardest obstacle: nothing in the payload has to be signed or
checksummed in advance.

Still open: the boot-time path that loads FIRM from flash prints
`firmware_header_len ... loadCrc 0x%x`, which implies a `loadCrc` field is
checked at boot. That code was not located - those format strings appear
nowhere as pointers in the bootloader image, so they are dead strings from a
build where the code was compiled out or relocated. Whether a running system
verifies `loadCrc` is therefore unresolved.

## Writing FIRM directly: still unproven

`tools/sl6806-pack` will build you an image. Nobody has booted one. It patches
the FIRM partition directly rather than going through `update.up`, so the
decode above does not yet apply to it. The application header it writes is
only partly understood:

| Offset | Field | Status |
|---|---|---|
| +0x00 | header length (0x30) | known |
| +0x04 | build timestamp | known — the SD-update path compares it |
| +0x10 | loadToRam (0x00804C00) | known |
| +0x14 | runFrom (entry \| 1, Thumb) | known |
| +0x1C | length copied to RAM (~0x5862 stock) | known |
| +0x18 | `0x1000` on the stock image | purpose unclear |
| ? | `loadCrc` | named by a boot-time log string; the field's offset and whether it is checked are unresolved |

The two questions that used to sit here — the "mark" and the body CRC — turned
out to belong to the SD-update path, not to this header. The mark is
`"SL6806"` at +22 **of an `update.up` file**, and the body CRC is a
write-verify the bootloader computes at flash time. Neither appears in the
FIRM partition header, so neither blocks this route; `loadCrc` is now the only
named field here that is not understood.

The bootloader's own debug strings (`header pass`, `mark pass`,
`time is not same`) show there are checks beyond the fields we understand. An
image that fails one of them will not boot.

Because of that, `sl6806-pack` does not synthesise a header. It patches *your*
dump, changing only the fields above and leaving every unknown byte exactly as
the vendor wrote it. That is the best odds available without finishing the
format — and it is still a guess.

## Recovery

The reason this is survivable at all: **USB download mode is in mask ROM, not
in flash.** A device that will not boot its application still enumerates in
bootloader mode, and you can write your golden dump back.

Before flashing anything, prove the recovery path *first*, in this order:

1. Take a full dump and verify it: `tools/sl6806-checkdump dump.bin`.
2. Copy it somewhere off the working tree.
3. Confirm you can re-enter bootloader mode reliably — you know the button,
   and `inquiry` reports `SMTLINK DEVICE 2.00`.
4. Practise the restore command *before* you need it.

If any of those four is shaky, stop. The payload workflow gives you the same
development loop with none of this risk.

## Restoring

```sh
sudo ./smtlink_dump chip 6806 init \
    erase_flash 0x10000 0x1B8000 \
    write_flash 0x10000 0x10000 0x1B8000 dump.bin
```

Erases are 4 KiB aligned. Rewrite only the FIRM partition — leave the HLKJ
bootloader at offset 0 and the partition table at 0xF000 alone. Damage the
bootloader and mask-ROM recovery still works, but you have more to rebuild.

## If you do try it

```sh
make SKETCH=examples/Hello MODE=firmware
tools/sl6806-pack --template dump.bin --firm build/Hello.bin -o new.bin
```

`new.bin` is a complete 4 MiB image. Write only the FIRM range from it, as
above. Expect it not to boot on the first attempt; the interesting output is
whatever the bootloader prints when it rejects the image, because that names
the check that failed and closes out the header format.

## Finishing the format

The remaining work, from the reverse-engineering notes:

- Decode the mark field and the body CRC in the FIRM header.
- That also yields the SD-update file format — the no-USB install channel,
  and a considerably safer way to deliver firmware than writing flash
  directly.

Until then: payload mode.
