# Flashing (and why you probably should not yet)

The payload workflow — `make upload` — loads your sketch into RAM and runs it
there. It never touches flash, so it cannot brick the device. **Everything in
this framework works in payload mode.** Use it.

This document is about the other path: replacing the vendor application in the
FIRM partition so your code runs standalone, off USB.

## Current status: unproven

`tools/sl6806-pack` will build you an image. Nobody has booted one. The
blocker is that the FIRM application header is only partly decoded:

| Offset | Field | Status |
|---|---|---|
| +0x00 | header length (0x30) | known |
| +0x04 | build timestamp | known — the SD-update path compares it |
| +0x10 | loadToRam (0x00804C00) | known |
| +0x14 | runFrom (entry \| 1, Thumb) | known |
| +0x1C | length copied to RAM (~0x5862 stock) | known |
| ? | the "mark" the bootloader validates | **not decoded** |
| ? | which CRC covers the application body | **not decoded** |

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
