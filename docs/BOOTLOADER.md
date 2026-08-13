# The HLKJ bootloader, read end to end

The bootloader is 61,292 bytes at file `0x60`, loaded to `0x0081FC00`, entered
at `0x00820000`. It is the most legible program in the image: self-contained,
small enough to read, and it touches each peripheral once — where the
application's uses of the same block are buried under a scene framework.

Regenerate everything here with:

```sh
tools/sl6806-boot dump.bin            # header, mapping, both CRCs
tools/sl6806-boot dump.bin --map      # every function, what it touches
tools/sl6806-boot dump.bin --strings sdio
tools/sl6806-boot dump.bin --xref 0x40003000
tools/sl6806-boot dump.bin 0x00822768 --dis 96
```

The inventory: **340 call targets, 140 mask-ROM entry points, 1552 call
sites**, of which **81 functions carry an MMIO base or a string** and are the
ones worth reading. Everything below came out of that list.

## The phases

```
mask ROM reset
  |
  +-- boot-mode select (0x0003D8B8): ~1 s on bank 1 pin 10, then pin 11
  |     an edge on either tail-calls a different handler (§"the boot ROM's
  |     two mode inputs"); pin 11's level is also stored as a boot flag
  |
  +-- otherwise load HLKJ from flash to 0x0081FC00 and enter it
        |
0x00820000  zero r0..lr, set SP from the header, zero BSS, call main
        |
0x00820588  main
        |
        +-- 0x00820378  hardware_init(1)          the whole of §2 below
        |
        +-- 0x008244DC  update from PC wanted?    -> update path
        +-- 0x008244CC  forced restore?           -> restore path ("forced...")
        +-- 0x008244D4  sdupdate wanted?          -> SD path
        +-- otherwise   0x00820514 -> 0x00820E8C  load FIRM and run it
```

Each update path runs the same opening: `0x008204BC` (display),
`0x008203DC`, `0x008202B2`, `0x0082029C`, then `0x00820318` logging
`"Finding file..."`.

## 1. What main chooses between

| Decision | Logs | Then |
|---|---|---|
| `0x008244DC` | `boot--->update from pc` | USB download of a new image |
| `0x008244CC` | `boot--->forced_restore` | `0:\restore.up` |
| `0x008244D4` | `boot sdupdate--->...` | `0:\update.up` from a card |
| else | `boot--->load firmware disp.` | FIRM |

The `sdupdate` validator is §6's, and its strings — `header pass`, `mark
pass`, `time is not same` — are how that section decoded the `.up` format.

## 2. `hardware_init`, 0x00820378

The first thing main calls, and the phase with the most in it.

```
0x00820164()                    NVIC and SCB      (0xE000E100, 0xE000ED14)
ROM 0x3BFC()
0x00820DF8(24)
0x008206D0(2, 384000000, 32000000)                the clock tree, §3
0x00820284()
pad_configure(0x00011300)       bank 1 pin 2, function 6
0x0082A220(&{1500000, 48000000, 1, ...})          the console, §4
0x0082356C(1)                   route printf to it
```

## 3. The clock tree — the PLL is at CRU `+0x10`/`+0x14`

`0x008206D0(mode, 384000000, 32000000)`:

```
[0x400F7000 + 0x60] = 0
clock_source_select(3, 10), (6, 10)         both no-ops for those ids
ROM 0x3A6C(2, 384000000)                    -> ROM 0x1F6C, the PLL
romclk_disable(8), romclk_disable(9)
[0x400F7000 + 0xD8] &= ~2
divider(3, 2)  divider(4, 1)  divider(5, 1)
divider(6, 384000000 / 32000000 = 12)
```

and ROM `0x1F6C` is the PLL itself:

```
    ref = 24576000
    mul = (freq == ref) ? 0x3126 : 0x000186C2
    [0x40080010] = (old & 0xFC1F0000) | 0x3000 | (freq == ref ? 0x60 : 0x40)
    [0x40080014] = (old & 0x00003800) | 0x80000000 | 0x400 | (mul << 14)
```

with the rate readable at `0x40080000` as `((v >> 8) & 0xFF) * 48 / (v & 0xF)`
MHz — m = 8, d = 1 being exactly the 384 MHz asked for. See notes §25; this
retracts the README's "the CRU holds no PLL multiplier".

## 4. The console at `0x40091000`

`0x0082A220` enables **module clock 73** and **ROM clock 22**, files
`0x40091000` as a device base with 1,500,000 and 48,000,000 beside it, and
sets `+0x10 |= 0xB0`, `+0x14 |= 3`. `0x0082356C(1)` then tail-calls ROM
`0x260`, which installs the sink the ROM's printf writes to.

So every `boot--->` and `sdio(i):` string in this image comes out of that
block, and FIRM brings it up with the same routine at `0x00D9A4C0`. Notes §26;
`[I]` a UART at 1.5 Mbaud, and the best candidate this project has for a
console that does not depend on USB polling.

## 5. Storage

| Function | What |
|---|---|
| `0x0082167C` | SD pads and clocks — **six pads, bank 1 pins 12..17** |
| `0x00822768` | `HAL_sd_disk_init`, files `0x40003000` into the driver handle |
| `0x0082A7BC` | `HAL_SD_Init`, the bootloader's copy of ROM `0x455A` |
| `0x00822A24` | `HAL_SD_Read_new` |
| `0x00822CF4` | `HAL_SD_Write_new` |
| `0x0082725C` | `sdmmc_wrap_init`, the FatFs mount |
| `0x0082A1AC` | `HAL_sd_disk_deinit` |

The whole of §23 and §24 came out of these. Note `0x0082167C` configures six
pads where the mask ROM configures three: the ROM's is the chip's generic
one-bit minimum, this is the product's four-bit bus.

## 6. The register file, and its real name

`0x00820BF0` (write) and `0x00820C58` (read) are the mailbox accessors for the
chip at `0x400F7000+0x100` — §7m's indexed register file, the one carrying the
LDO rails. They take a mutex through `0x00822260` / `0x0082228C`, and those two
log **`rtwi op in isr`** when called from an interrupt.

**So the vendor's own name for that mailbox is TWI.** §7m deduced it was an
I²C master from `0x60`/`0x61` being an address and its R/W bit; the bootloader
says so directly. That bears on the README's open "the TWI controller's base
address": at least one TWI master is `0x400F7000+0x100`, and it is already
driven by `cores/sl6806/sl6806_regfile.c`.

## 7. Peripherals the bootloader touches

| Base | What | Where |
|---|---|---|
| `0x40000000` | pad mux | `0x008207B4` and 28 other sites |
| `0x40001000` | DMA channels | `0x00829768` |
| `0x40003000` | **SD/MMC host** | `0x00822768`, `0x0082A3E4`, `0x0082A7BC` |
| `0x40070000` | DMA control | `0x00821CC8`, `0x00829CB0` |
| `0x40080000` | CRU, **including the PLL** | `0x008206D0`, `0x008216F8` |
| `0x40081000` | GPIO bank 0 | `0x008207F4` |
| `0x40091000` | **the console** `[I]` | `0x0082A220` |
| `0x400A0000` | **a hardware mutex** `[I]` | `0x00820D90`, `0x00820DA8` |
| `0x400D9000` | LCD controller | `0x00829A28` (`HAL_lcdc_module_init`) |
| `0x400F7000` | SPI flash host; `+0x100` the TWI mailbox | many |

`0x400A0000` is new. `0x00820D90` spins while `[0x400A0010 + id*4]` is
non-zero and `0x00820DA8` writes zero to the same word — a lock array, ten and
eleven call sites respectively, used by the code that guards the TWI mailbox.
`[I]` a hardware mutex block; nothing names it.

## 8. What runs on top

The bootloader carries a small RTOS and a filesystem, which is why the SD path
can mount a card at all:

- **FreeRTOS**, from `[osfreertos]: Error creating exitSem` and
  `task:%s stack overflow`;
- an OAL allocator (`[oal_mem]: ... OAL_calloc1 %d failed.`), at
  `0x0082AF68` / `0x0082AF9C` with twenty call sites between them;
- **FatFs**, from `FAT32   `, `f_open failed.`, `fs disk write err.`;
- a device layer — `_dev_service_queue_`, `%s<%d> has inited!!!` — which is
  what `0x0082356C` files the console into.

## What is not read

Of the 340 call targets, 259 carry neither an MMIO base nor a string: FatFs
internals, the allocator, string and integer formatting, FreeRTOS scheduling.
They are readable, and nothing in them is likely to be a peripheral, because a
peripheral needs a base address and a base address is a literal.

The named gaps are `0x00820DF8(24)` and `0x00820284` in `hardware_init`, ROM
`0x3BFC`, and the register map of the console block beyond `+0x10`/`+0x14` —
which is behind ROM `0x0000023D`.
