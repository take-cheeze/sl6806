# Bluetooth: what the dump says, and what hardware needs to say next

| Layer | State |
|---|---|
| Hardware | **Present**, per the vendor's own firmware — a full HCI/L2CAP/A2DP/AVRCP/HFP/SPP/OPP stack is linked into FIRM. |
| Register window | **Candidate found: `0x400E2000`**, identified from the code next to the HCI command dispatcher. About ninety load sites across some thirty registers in three clusters. |
| The gate | **DECODED AND RUN.** The block's functional clock is `0x400E0000` bit 0 and its reset is `0x400E0008` bit 0. The shared bring-up opens the whole `0x400Exxxx` group, **the PLL locks from a payload**, and four registers in the window wake up. Measured 2026-08-13. |
| Driver | **A gate, and nothing above it.** [`cores/sl6806/sl6806_bt.c`](../cores/sl6806/sl6806_bt.c) performs the vendor's unlock sequence; `examples/BtProbe` runs it and reads the window before and after. Not one bit's *meaning* is known. |
| Running | **No.** The four counters stay at zero. The window is powered and partly readable; nothing is configured. |

This is still an earlier-stage document than [LCD.md](LCD.md). What has
changed is that its two headline unknowns — "what unlocks it" and "which bit
in `0x400E0000` it needs" — are both answered, and the answer has been run.

**Outside corroboration, 2026-08-13.** Every claim above comes from the dump, so
it is worth recording one piece of evidence that does not. The chip is very
probably Zhuhai 绅聚科技's 云P3 — the vendor's `yp3_` version strings, and see
[notes §1a](sl6806_re_notes.md) — and their 2022 announcement of it says
"主控与蓝牙功能一体": controller and Bluetooth in one part. So the radio is
on-die, and `0x400E2000` is a link controller to be driven, not a transport to
some companion chip that this board may or may not be fitted with. That was
assumed here already; it is no longer only an assumption. It does not narrow
which register does what.

## MEASURED, 2026-08-13 — the wall comes down

`examples/BtProbe` on a P20 Player. The sequence ran, the device stayed on the
bus, and:

```
PLL   (0x40080008) = 0xD0010C04     written 0xC0000C04; bit 28 LOCK and bit 16
                                    came back set
gate  (0x400E0000) = 0x00000021     bits 5 and 0, both HELD
reset (0x400E0008) = 0x00000001
```

**The PLL locks from a payload.** §14a read it stopped at `0x00000801` in
bootloader mode and left "what unlocks it" open across two sections. And
`0x4008011C = 0x31` — the write §14a flagged as *"plausibly safe, but untried
because it might reparent the core or USB clock"* — does not do that. The
console survived the whole sequence, twice.

Four registers in the window came up from zero:

```
+0x078 = 0x0C019A14        +0x1B4 = 0x00003301
+0x07C = 0x06060502        +0x21C = 0x00400000
```

and they **stay up across a re-upload** — a second run built with
`-DBTPROBE_READ_ONLY=1` saw all four in its "cold" pass, because the gate was
still open from the first. Worth knowing before anyone reads a cold pass as
cold.

**The four counters stayed at zero, all eight samples.** So: the window is
powered and partly readable, and the link controller is not running. That is
exactly what you would expect with no configuration written, and it is the
next thing to attack — but it needs the config struct decoded, not another
probe.

### What this unlocks beyond Bluetooth

The wall §14a and §15 spend pages on is the same wall for the PWM and the
camera, and it is now known to come down from an ordinary payload. Concretely:

- The backlight's dimming has been stalled since §14a behind a counter that
  will not run. Its functional-clock bit in `0x400E0000` has never been
  looked for because the register would not hold bits. It holds them now.
- Four of that register's bits are attributed (camera 6, Bluetooth 0, the
  `0x400E2300` cluster 1, group 5), so a walk over the other 28 has known-good
  answers to check itself against.

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
wire.

## Finding the register window

The HCI dispatcher is a 64-way jump table between file-offset-derived
addresses `0x00DA0000` and `0x00DA9BCA` (reproduce with
`tools/sl6806-xref dump.bin --string="-hci cmd0x%x" --context 12`). Scanning
literal loads restricted to the neighbourhood around it turns up one
peripheral-shaped cluster that §7c had already flagged as unidentified:
**`0x400E2000`**.

```
tools/sl6806-xref dump.bin 0x400E2000 --context 30
```

**`0x400E2000` is loaded exactly once in the entire 1.8 MB application** — at
`0x00D98C9E` — which is the same "one owner, and it's the right
neighbourhood" signature that first pointed at the LCD controller. The
function containing that load, `0x00D98C9C`, is a module-registration routine.

### One correction to that case, worth making explicitly

"Loaded exactly once" is true of the *base* literal and gave the impression of
a lonely, barely-used address. Scanning the neighbourhood for every literal in
`0x400E2000..0x400E2FFF` instead — rather than for the base alone — turns up
**about ninety load sites across some thirty distinct registers**, in three
clusters with their own bring-up routines:

| Offsets | Driven from | Own gate |
|---|---|---|
| `+0x000`..`+0x07C`, `+0x200`..`+0x234` | `0x00D98B18`..`0x00D98D80` | `0x400E0000` bit 0 |
| `+0x180`..`+0x1B8` | `0x00D98EAC`..`0x00D990D4` | not seen |
| `+0x300`..`+0x32C` | `0x00D989D0` | `0x400E0000` bit 1 |

This is a busy window, not a stub. Only the first cluster is what the HCI
neighbourhood argument names; the other two are simply the rest of what lives
here, and `examples/BtProbe` reads all three, because a window where one
cluster is live and another is dark says more than either alone.

## The registration, decoded

```
push {r3,r4,r5,r6,r7,lr}
ldr  r3, =0x400E2000          ; the base
ldr  r4, =0x0082B3A8          ; an SRAM descriptor slot
strd r3, r0, [r4]             ; descriptor := { base, r0 (a config pointer) }
bl   0x00D9A7FC               ; the shared group bring-up  <- decoded below
movs r0, #0
bl   0x00D9A734               ; 0x400E0000 |= (1 << 0)     <- THE BIT
movs r0, #0
bl   0x00D9A768               ; 0x400E0008 bit 0, reset pulse
ldr  r2, =0x400E2214
orr  r3, r3, #0x1000000       ; bit 24 set
...                           ; unpacks ~12 bitfields from *r0 into +0x10 .. +0x7C
```

A few instructions earlier the same function does a second reset — a
clear-bit31 / delay(10) / set-bit31 pulse on `+0x228`, with `0xFFFFFFFF`
written to `+0x200` inside the window. That is *in addition* to the
`0x400E0008` one, and the vendor does both.

## What `0x00D9A7FC` actually is

The earlier version of this document listed three calls here, named two of
them from §14a/§15, and left the third "still unexamined". All three are now
read, and the first one is much more than "starts the PLL":

```c
if (module_clock_is_enabled(46))   return;      /* ROM 0x1E54 — a once-guard */
power_request();                                /* 0x00D9A7AC, see below */
*(u32 *)0x40080008 = 0xC0000C04;                /* the PLL */
while (!(*(u32 *)0x40080008 & (1 << 28))) ;     /* spin on lock */
*(u32 *)0x40080008 |= 0x10000;                  /* output enable */
module_clock_enable(46);
delay(10);
*(u32 *)0x4008011C = 0x31;
delay(10);
periph_enable(5);                               /* 0x400E0000 bit 5 */
```

and `power_request()` is at the **pad-mux** base, not the CRU, which is why
nothing had connected it to clocking before:

```c
for (i = 0; i < 10; i++)
    if (!(*(u32 *)0x40000070 & (1 << (16 + i)))) {
        *(u32 *)0x40000070 |= (1 << i);
        while (!(*(u32 *)0x40000070 & (1 << (16 + i)))) ;
    }
*(u32 *)0x40000074 = 0;
```

Ten request bits in `[9:0]`, ten acknowledgements in `[25:16]`. This is the
only code in the whole dump that touches either register, so there is nothing
to cross-reference it against; a power-domain or isolation handshake is the
shape, not a decode. The teardown at `0x00D9A7E4` sets `0x40000074` bit 0,
writes `0x40000070 = 0` and waits for it to read back zero.

Three things fall out of this that reach well beyond Bluetooth:

1. **Module 46 is not "the camera front end".** It is the gate for the whole
   `0x400Exxxx` address group — which is exactly what §7n measured when
   `sl6806_module_enable(46)` made `0x400E0000` start holding bits, and it
   explains why one module id appeared to belong to a peripheral it shares
   nothing else with.
2. **`0x400E0000` bit 5 is taken by the shared path**, before any individual
   peripheral's bit. Nothing else in the group claims bit 5.
3. **`0x4008011C`'s value is `0x31`.** §14a left this as an open item —
   "one bit apart between enabled and disabled, so plausibly safe, but
   untried". `sl6806_pwm.h` already carries the same reading. This is the
   second, independent sighting of the same write, from a completely
   different peripheral's bring-up.

## The counters, which are a second kind of evidence

`0x00D9A8F4` is a four-way accessor over `+0x228`, `+0x22C`, `+0x230` and
`+0x234`, masking what it returns to **25, 30, 30 and 30 bits**. Nothing
writes any of them. Odd widths like that on read-only registers are counters,
and a ~28-bit free-running counter is where a Bluetooth link controller keeps
its native clock.

That is still a shape argument — but it is a *different* shape argument from
the one the window was found by, and it is the one that can be tested. A
counter is the only kind of register that distinguishes "the block is
readable" from "the block is running", which is the distinction that cost the
PWM twelve hardware sessions (§14a). `examples/BtProbe` samples all four,
eight times, and says which moved.

`+0x22C` also carries two control bits above its counter: `0x00D9A8BC` sets
bit 31 and spins until it reads back — a request/grant handshake — and
`0x00D9A930` reads bit 30 as a boolean. `sl6806_bt_clocks()` masks both out,
so a busy control bit cannot make a stopped counter look alive.

## What none of this establishes

- **What any bit means.** The bitfield widths pulled out of the config struct
  (12, 16, 8, 7, 18, 10, 14 bits, several packed per register) are consistent
  with radio or link-layer timing parameters, but "consistent with" is a shape
  argument, not a decode. Nobody has correlated a single one of these fields
  against a known BT parameter.
- ~~**Whether the unlock works.**~~ It does — see the measured section above.
  What is *not* established is whether it is sufficient: four registers out of
  forty-two woke, and the counters did not move.
- **Whether this window is the whole story.** A combo SoC with a real radio
  needs more than one register window in general — analog and PHY trim is
  often elsewhere entirely.

## Next step

```sh
make SKETCH=examples/BtProbe RUN_MODE=poll run
```

The sketch now has two halves: a read-only pass over all thirty registers,
then `sl6806_bt_begin()` and the same pass again, then eight samples of the
four counters.

**The second half writes, and one of the writes is a PLL.** The argument that
this is defensible rather than reckless: `0x4008011C` differs by one bit
between the vendor's on and off values, which is an enable and not a
clock-source reparent, and the PLL is started rather than switched onto
anything. Nothing in the sequence touches a divider the core or USB is running
on. It is still the most invasive sketch in the tree — in payload mode it
cannot brick anything, since flash is never written, but it can plausibly hang
the device. Unplug, hold the boot button, plug back in. Build with
`-DBTPROBE_READ_ONLY=1` for the old behaviour.

Outcomes 1 and 2 of the original three have been settled — the gate opens.
What is left is the one that matters:

**The counters moving between samples.** That is a running link controller,
and it would turn "a plausible register window" into "confirmed hardware" in
one line of a transcript. They do not move today. Getting them to move needs
the config struct at `0x00D98CC4..0x00D98D80` decoded and replayed into the
twelve fanout registers, which is a reading exercise on the dump, not another
probe run.

A cheaper thing to do first, and a better use of a device: **walk
`0x400E0000`'s other 28 bits now that it holds them**, with the PWM's counter
as the witness. That has been the top unclaimed item in the README for two
sessions and the register only became writable from a payload today.

Report results the way `examples/RegFileProbe`'s comment block records its
hardware run, so nobody has to repeat it to find out.
