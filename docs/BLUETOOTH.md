# Bluetooth: what the dump says, and what hardware needs to say next

| Layer | State |
|---|---|
| Hardware | **Present**, per the vendor's own firmware — a full HCI/L2CAP/A2DP/AVRCP/HFP/SPP/OPP stack is linked into FIRM. |
| Register block | **Candidate found: `0x400E2000`**, identified from the code next to the HCI command dispatcher. Already read once, indirectly: a later pass through this codebase (§14a/§15 of the notes) read it from the host in bootloader mode while chasing an unrelated peripheral and got all zeros — same as everything else behind the PLL/`0x400E0000` gate that section documents. Not yet read from a payload, and no bit's meaning is confirmed. |
| Driver | **Not started.** `examples/BtProbe` reproduces the zero-read from a payload; the useful next step is the same PLL/gate unlock work §14a/§15 already need for the backlight and the ADC. |

This is an earlier-stage document than `docs/LCD.md`. The LCD controller was
solved because the HLKJ bootloader carries a plain, disassemblable driver for
it. Bluetooth has no bootloader equivalent — everything about it lives inside
the compiled application, so this writeup is closer to where the LCD
controller was before `HAL_lcdc_module_init` was found: an address, a
plausible reason to believe it, and a list of what would confirm it.

## The application's Bluetooth stack is real and large

Strings alone settle that this is not a stub. `dump.bin` contains a complete
event enum (`BT_EVENT_STACK_OPEN`, `_CLOSE`, `_INQUIRY_COMP`,
`_DEV_CONNECT_COMP`, `_A2DP_OPEN`/`_CLOSE`/`_SNK_STREAM_START`,
`_AVRCP_OPEN`/`_OP`, `_HF_CALL_INCOME`/`_RING`/`_CODEC`, `SPP_OPEN`/`_RX`,
`_OPP_OPEN`, and more), profile name strings (`l2cap`, `rfcomm`, `a2dp`,
`avrcp`), and 27 persisted settings keys in the `PSMP` partition —
`bt_addr`, `bt_name`, `bt_inqname`, `bt_showname`, `bt_relink`, `btlinknum`,
`btlinkinfo`, `le_addr`, `bluetooch_status` (sic) — confirming this unit has
actually paired with something at some point (§7k of
[`sl6806_re_notes.md`](sl6806_re_notes.md)).

**No external Bluetooth chip is visible.** A search across `dump.bin` for the
usual signs of a UART-attached BT module — vendor strings (Airoha, Jieli,
Beken, Realtek, CSR, RDA), AT-command text, an `hci_h4`/`hci_uart` transport
layer — finds nothing. What the strings show instead is an `lm`/`llm`
("link manager" / "lower link manager") naming convention and a 64-entry HCI
opcode dispatch table that logs `"-hci cmd0x%x"` for anything it does not yet
implement — i.e. this looks like an HCI stack terminating on this CPU against
local hardware, not a transport driver talking to a separate radio SoC over a
wire. That is consistent with the SL6806 being marketed as a combo part (this
same dump already gave up an integrated FM tuner and camera interface on
plain I2C — see the README's peripheral table) rather than with a
discrete Bluetooth chip riding along on a spare UART.

## Finding the register block

The HCI dispatcher is a 64-way jump table between file-offset-derived
addresses `0x00DA0000` and `0x00DA9BCA` (reproduce with
`tools/sl6806-xref dump.bin --string="-hci cmd0x%x" --context 12`, after
`sudo apt install gcc-arm-none-eabi` for the disassembler it shells out to).
Scanning literal loads restricted to the neighbourhood around it —
file offsets `0x190000`–`0x1B0000`, i.e. addresses `0x00D90000`–`0x00DB0000`
— rather than the whole 1.8 MB FIRM image turns up one peripheral-shaped
cluster that the whole-firmware scan in §7c had already flagged as
unidentified: **`0x400E2000`**.

```
grep -n "0x400E2000\|0x400E2214\|0x400E2218\|0x400E2228" docs/sl6806_re_notes.md   # cross-check
tools/sl6806-xref dump.bin 0x400E2000 --context 30
```

That last command is the whole case: **`0x400E2000` is loaded exactly once
in the entire 1.8 MB application** — at `0x00D98C9E` — which is the same
"one owner, and it's the right neighbourhood" signature that first pointed at
the LCD controller. The function containing that load, `0x00D98C9C`, is a
module-registration routine:

```
push {r3,r4,r5,r6,r7,lr}
ldr  r3, =0x400E2000          ; the base
ldr  r4, =0x0082B3A8          ; an SRAM descriptor slot
movs r1, #0xC0
strd r3, r0, [r4]             ; descriptor := { base, r0 (a config pointer) }
movs r0, #42
bl   0x00D9A7FC                ; starts the PLL and spins on its lock bit (§14a)
movs r0, #0
bl   0x00D9A734                ; enables the module through 0x400E0000 (§15) — dead from a payload today
movs r0, #0
bl   0x00D9A768                ; still unexamined
ldr  r2, =0x400E2214
ldr  r3, [r2]
orr  r3, r3, #0x1000000        ; bit 24 set
str  r3, [r2]
...                             ; unpacks ~12 bitfields from *r0 into +0x10 .. +0x7C
```

A few instructions earlier, the same function does a reset handshake on a
neighbouring pair of registers:

```
ldr  r4, =0x400E2228
ldr  r3, [r4]
bfc  r3, #31, #1               ; clear bit 31
str  r3, [r4]
ldr  r3, =0x400E2200
mov  r2, #0xFFFFFFFF
str  r2, [r3]
movs r0, #10
bl   0x00807214                 ; already documented as "delay", 39 sites (§ ramcalls)
ldr  r3, [r4]
orr  r3, r3, #0x80000000        ; set bit 31
str  r3, [r4]
```

That is a clear-bit31 / delay(10) / set-bit31 reset pulse through the same
delay veneer used all over this codebase, immediately followed by a config
struct fanned out into a dozen narrow register fields. That two-part shape —
reset handshake, then config fanout — is what every other confirmed
peripheral bring-up in this repository looks like (compare the CRU divider
setters in `sl6806_cru.h` and the panel init in `variants/p20_player/panel.c`).
It is why this reads as a real, distinct hardware block rather than a
software-only lookup table, and it is the basis for treating `0x400E2000` as
a genuine candidate rather than noise.

A second, separate accessor cluster at `0x00D98B18`–`0x00D98C58` reads back
`0x400E2200`, `0x400E2218` and `0x400E2228` through the same SRAM descriptor
— `0x400E2218`'s top nibble is compared against 4 and used to index a small
table, which is the shape of a status/mode field, and `0x400E2200` has a
single bit (bit 1) read out elsewhere as a boolean flag.

Full offset list, with what's known about each, is in
[`cores/sl6806/sl6806_bt.h`](../cores/sl6806/sl6806_bt.h) — kept as one
source of truth rather than duplicated here.

## Two of the three unexamined calls are no longer unexamined

The two `bl`s marked "unexamined" above were named independently, by a later
pass through this codebase chasing the backlight and the ADC rather than
Bluetooth (`docs/sl6806_re_notes.md` §14a and §15). `0x00D9A7FC` is "the
first thing the vendor's module bring-up does" — it writes `0xC0000C04` to
the PLL config register at `0x40080008` and spins on lock bit 28, which in
bootloader mode never sets. `0x00D9A734` is the routine through which "the
application enables most peripherals through `0x400E0000`", confirmed dead
from a payload; its other call sites use arguments 0/1/2/3/4/6, and this
call's argument — 0 — sits inside that range. `0x00D9A768` is still
unexamined.

That same section read `0x400E2000` directly, from the host, in bootloader
mode, while building a table of which blocks are live:

```
0x40080000 CRU   — live
0x400D9000 LCDC  — live
0x400E0000        — all zeros
0x400E2000        — all zeros
0x40084000        — all zeros
```

So this is no longer a hole in this document — the register block is
already confirmed to read as flat zero in bootloader mode, and the reason is
not specific to Bluetooth: it is the same unlocked PLL and `0x400E0000` gate
that keeps the PWM and (until §15b's fix) the ADC dark too, and it answered
the same way for the host command as it would for a payload. Unlocking it
means finishing `0x4008011C` (§14a's open item — one bit apart between
enabled and disabled, so plausibly safe, but untried because it might
reparent the core or USB clock) or finding whatever `0x00D9A734`'s
`0x400E0000` gate actually needs.

## What none of this establishes

- **What any bit means.** The bitfield widths pulled out of the config
  struct (12, 16, 8, 7, 18, 10, 14 bits, several packed per register) are
  consistent with radio or link-layer timing parameters — that many
  differently-sized fields in one block is not what a simple digital
  peripheral looks like — but "consistent with" is a shape argument, not a
  decode. Nobody has correlated a single one of these fields against a known
  BT parameter (channel map, clock offset, access code, whatever).
- **What unlocks it.** It reads zero today because the PLL and the
  `0x400E0000` enable gate it depends on are off, the same wall §14a/§15
  document for the backlight and the ADC. Getting past that wall without
  reparenting the core or USB clock out from under the session is an open
  problem shared with those two, not something specific to Bluetooth.
- **Whether this is the whole story.** A combo SoC with a real radio needs
  more than one register window in general (analog/PHY trim is often
  elsewhere entirely). `0x400E2000` explains the code next to the HCI
  dispatcher; it does not by itself prove there is nothing else to find.

## Next step

`examples/BtProbe` reads every offset this document names, read-only, paced
one register per `loop()` call the same way `examples/MmioProbe` and
`examples/RegFileProbe` are, specifically so that if the device stops
responding, the last line in the monitor names the exact address that did
it:

```sh
make SKETCH=examples/BtProbe RUN_MODE=poll run
```

Expect flat zero — that is what the host-side read already found, and a
payload has no more access to the PLL/`0x400E0000` gate than the host does.
Running it anyway is still worth doing once, both to confirm a payload sees
the same thing the host command does (nothing in this codebase has checked
that the two paths agree here) and as the sketch to come back to once the
gate is unlocked. Three outcomes, in order of how interesting they'd be:

1. **Everything reads `0x00000000`** — expected, matches the host-side
   read in `docs/sl6806_re_notes.md` §14a. Confirms a payload sees what the
   host sees; does not by itself rule the block out.
2. **Everything reads `0xFFFFFFFF`** — would disagree with the host-side
   read and be worth a second look at whether payload and host are really
   addressing the same thing.
3. **Values are varied, and/or `SL6806_BT_STATUS` (`+0x218`) or
   `SL6806_BT_CTRL` (`+0x200`) changes between the two watch passes without
   anything having been written** — would mean the gate is no longer where
   §14a found it, which would be news on its own.

The result that actually moves this forward is `examples/BtProbe` re-run
*after* whatever unlocks the PLL/`0x400E0000` wall for the backlight or the
ADC — at that point this document's three outstanding questions (bit
meanings, whether one window is the whole story, and the config struct's
field layout) become answerable from real values instead of from code shape.
Report results the same way `examples/RegFileProbe`'s comment block records
its hardware run, so nobody has to repeat it to find out.
