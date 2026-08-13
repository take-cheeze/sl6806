/*
 * sl6806_audio.h - the SL6806's audio controller at 0x40009000.
 *
 * =====================================================================
 *  WHAT THIS IS, AND WHAT IT IS NOT
 * =====================================================================
 * This is a transcription of the vendor's own audio path, register for
 * register, out of the flash dump. Everything marked [V] was read out of code
 * that the stock firmware runs every time it plays a file.
 *
 * =====================================================================
 *  [M] MEASURED 2026-08-13: THE BLOCK IS HERE AND IT WAKES
 * =====================================================================
 * examples/AudioProbe, on a P20 Player. Cold, every register in the list
 * reads 0x00000000. After `sl6806_module_enable(37)` and romclk 19 - two
 * writes, no PLL, nothing near 0x400E0000 - **thirty of them come up at once
 * with sensible reset defaults**:
 *
 *     +0x018  0x00000200
 *     +0x07C  0x24924924   every 3-bit field = 4
 *     +0x080  0x24924924   likewise
 *     +0x100  0x00000700   playback control, bits [10:8]
 *     +0x108  0x00000800   playback status, bit 11
 *     +0x10C  0x00800000   playback buffer address = the base of SRAM
 *     +0x134  0x00010801   playback ch0: bit 16 SET
 *     +0x138  0x0000A1FF   playback ch0 level = 0x1FF, the maximum
 *     +0x154  0x00010801   ch1, identical
 *     +0x158  0x0000A1FF
 *     +0x174  0x00010801   ch2, identical
 *     +0x178  0x0000A1FF
 *     +0x200  0x02300700   capture control
 *     +0x208  0x00000800   capture status
 *     +0x20C  0x00800000   capture buffer address = SRAM base
 *     +0x224  0x00010801   capture ch0: bit 16 SET
 *     +0x228  0x0000A000   capture ch0 level = 0, the minimum
 *     +0x254/8, +0x284/8, +0x2B4/8  identical for ch1..ch3
 *     +0x400, +0x5FC       coefficient RAM, still zero
 *
 * That is not a plausible-looking read; it is the block. Two independent
 * things fall out of it:
 *
 *   - **Playback levels reset to maximum and capture levels to zero.** Three
 *     channels at 0x1FF, four at 0. A DAC comes up open and an ADC comes up
 *     shut. This is the strongest single confirmation that the TX/RX
 *     assignment in this file is the right way round.
 *   - **Bit 16 is the mute, and it resets SET** on all seven channels - which
 *     is exactly the bit the vendor's init clears on the three playback
 *     channels and nowhere else. It was marked [I] here on the strength of
 *     that clear alone; the reset default makes it [M].
 *
 * Two registers read something this file did not predict: +0x108 and +0x208
 * both reset with **bit 11** set, which is not one of the two interrupt flags
 * and has no name yet, and +0x100 resets with [10:8] set.
 *
 * =====================================================================
 *  [M] RETRACTED: "THE DATA PATH RUNS". IT DOES NOT MOVE A BYTE.
 * =====================================================================
 * This file said the DMA path was "confirmed end to end" on the strength of
 * examples/ToneDemo getting +0x108 = 0x12BC0910 back - 0x12BC being 4796, the
 * buffer length exactly - with the completion flag setting on its own, five
 * hundred times running.
 *
 * examples/AudioWall then asked the question that reading could not:
 * it pre-filled a capture buffer with a pattern, handed it to the RX
 * descriptor, waited 100 ms and counted what changed.
 *
 *     0 of 64 words changed
 *
 * [!] That test is weaker than it first read. begin() performs the vendor's
 * PLAYBACK bring-up and never opens the capture direction, so it shows "RX
 * moved nothing while unopened" rather than "the engine cannot move memory".
 *
 * The load-bearing evidence is the timing, and it stands alone: 4796 bytes in
 * 10 us is 480 MB/s, which a 64 MHz AHB does not do. Whatever +0x108 retires
 * in 10 us is not 4796 bytes of memory. So the completion flag is not a
 * transfer, and everything above it is a descriptor being accepted and
 * retired - as consistent with a transfer that never happened.
 *
 * SL6806_AUD_IRQ_DONE is therefore very likely not "done" at all; an error or
 * abort flag that raises instantly on a block with no data path fits every
 * observation better.
 *
 * What survives: the length register holds what it is given, and +0x10C /
 * +0x20C hold an address. Those are still real. "The DMA path is confirmed"
 * was not.
 *
 * =====================================================================
 *  [?] WHAT IS MISSING IS STILL OPEN. ONE HYPOTHESIS, NOT CONFIRMED.
 * =====================================================================
 * There is a generic DMA channel setup at 0x00D97ED8 - it claims one of eight
 * slots, enables module clock 33, and configures 0x40001000 + ch * 0x40,
 * which 7c and 14b already describe as {ctrl, src, dst, len}. It is in the
 * same address region as the audio HAL.
 *
 * [!] AND THAT IS NOT ENOUGH TO SAY AUDIO USES IT. An earlier version of this
 * comment said "the audio block does not carry its own bytes" on the strength
 * of that proximity. Checking the one FIRM caller (0x00D3EAC0) turns up
 * lcdc_dma_write / lcdc_set_descriptor / lv_lcd_init in its literal pool:
 * **it is the LCD controller's DMA setup, not audio's.** 0x00D97ED8 is a
 * shared HAL routine whose only identified user is the LCDC.
 *
 * So the honest state is:
 *
 *   - the playback path moves no memory - inferred from 480 MB/s being
 *     impossible here, corroborated by the capture test;
 *   - measured, 0x400E0000 is not audio's gate, all 32 bits walked;
 *   - the general DMA controller exists, is used by at least the LCDC, and
 *     needs module clock 33 - all true, and none of it evidence about audio;
 *   - whether the audio FIFO is fed by that controller is UNKNOWN, and the
 *     way to settle it is to find what fills the DMA config struct for the
 *     audio path, not to reason from which region a literal sits in.
 *
 * =====================================================================
 *  HOW 0x40009000 WAS IDENTIFIED
 * =====================================================================
 * The application has a small device registry with string names; two of them
 * are "/dev/audio0" and "/dev/audio1" (flash 0x00C7595C and 0x00C75950). The
 * routine that opens them, at 0x00D72418, logs
 *
 *     -audio_driver_open _volume=====%d cfg1.is_headphone:%d
 *
 * and everything under it reaches hardware through one four-entry table of
 * driver objects at SRAM 0x0082B430. Entry 1 is the audio stream device; its
 * ops are installed at 0x00D9A0A4 and its ioctl is 0x00D9A024, which forwards
 * every command it does not handle itself to 0x00D96824 - a 145-way jump
 * table that is the codec's whole control surface. Every literal that table
 * loads is in 0x40009000..0x400092BC.
 *
 * The corroboration is the mic path. 0x00D3C688 logs "[driver_audio] AMIC0_
 * GAIN set to:%d" (and AMIC1, AMIC2) and reaches the same dispatcher with
 * commands 0x2D..0x32, so this block carries three analogue microphone
 * inputs - which is what an MP3 player with a voice recorder has.
 *
 * =====================================================================
 *  THIS CORRECTS THE NOTES: 0x40009000 IS NOT THE TIMERS
 * =====================================================================
 * docs/sl6806_re_notes.md 7c files 0x40009000 as "timers", on the evidence of
 * "channels at 0x100 stride (+0x108, +0x208) with write-1-clear flags and
 * per-channel callbacks". That description is exactly right and the
 * conclusion drawn from it is wrong: +0x108 and +0x208 are this block's two
 * DMA directions, their write-1-clear flags are the playback and capture
 * interrupts, and the per-channel callbacks are the four function pointers
 * the IRQ 30 handler at SRAM 0x0080D8F8 dispatches to.
 *
 * The same section already contains the reason it cannot also be the timers:
 * "FIRM's HAL_timer_* is at 0x40099000, a different block". Nothing in the
 * HLKJ bootloader references 0x40009000 at all, so the attribution never had
 * a second source.
 *
 * Provenance markers as elsewhere: [V] verified against the dump,
 * [M] measured on hardware, [I] inferred, [?] unknown.
 */
#ifndef SL6806_AUDIO_H
#define SL6806_AUDIO_H

#include <stdint.h>
#include "sl6806_mmio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* The block                                                           */
/* ------------------------------------------------------------------ */

/* [V] Base. Loaded by the codec dispatcher's literal pool at 0x00D96C34 and
 * by the two DMA submit routines in the SRAM blob. */
#define SL6806_AUD_BASE         0x40009000u

#define SL6806_AUD_REG(off)     (SL6806_AUD_BASE + (uint32_t)(off))

/*
 * [V] Global control, +0x00.
 *
 * Bits 24 and 25 are set to 1 by the DAC power-up path (0x00D93FB0 and
 * 0x00D93FBC) one at a time, through the vendor's read-modify-write helper -
 * so they are two separate enables, not a two-bit field. Bit 3 and the
 * two-bit field at [27:26] are reachable as codec commands 0x09 and 0x02 and
 * have no name yet.
 */
#define SL6806_AUD_CTRL         SL6806_AUD_REG(0x00)
#define SL6806_AUD_CTRL_EN0     (1u << 24)  /* [V] first DAC enable          */
#define SL6806_AUD_CTRL_EN1     (1u << 25)  /* [V] second DAC enable         */

/*
 * [V] The DAC path register, +0x08, and the one place in this block where
 * the layout is certain rather than inferred: 0x00D93F54 and 0x00D94028 are
 * the same function written twice, once per channel, and the second one uses
 * every constant of the first shifted left by 16. So +0x08 is two identical
 * 16-bit halves, channel 0 in [15:0] and channel 1 in [31:16].
 *
 * Within a half, from the four routes 0x00D93F54 selects on:
 *
 *     bits [1:0]   2 for routes 1..3, 3 for route 1 and route 3
 *     bit  6       set for routes 1 and 3   (0x40)
 *     bit  7       set for route 0          (0x80)
 *     bit  13      set for routes 0, 1, 3   (0x2000)
 *     bits [12:8]  a 5-bit field taken from the driver's own state (+0xA8)
 *
 * "Route" is this file's word for the byte at state+0x113, which the vendor
 * sets elsewhere and which is almost certainly speaker / headphone / line -
 * the strings around it are `-play status to spk play` and `-audio switch
 * earphone`. Which number is which output is [?].
 */
#define SL6806_AUD_DAC          SL6806_AUD_REG(0x08)
#define SL6806_AUD_DAC_CH1_SHIFT 16         /* [V] channel 1 is the top half */

/*
 * [V] +0x18 bits [9:5], +0x2C bits [20:16], +0x30 bits [20:16] and [28:24],
 * +0x30 bits 3 and 7, +0x3C bit 10, +0x7C bits [26:24], +0x80 bits [14:12]
 * and [11:9] are all written by the codec dispatcher. +0x30's two 5-bit
 * fields are the analogue mic gains (codec commands 0x03..0x05, which is
 * where "AMIC0_GAIN set to" lands); the rest are [?].
 */
#define SL6806_AUD_MIC_GAIN     SL6806_AUD_REG(0x30)
#define SL6806_AUD_ADC_GAIN     SL6806_AUD_REG(0x2C)

/*
 * [V] Playback DMA, +0x100..+0x10C, from 0x0080D9BC in the SRAM blob - the
 * routine the stream device calls with (handle, buffer, length):
 *
 *     +0x10C  <- buffer address
 *     +0x108[31:16] <- length in bytes
 *     +0x104  <- ((length * 3 / 4) << 16) | 1
 *     +0x108  |= 0x10                        (0x00D97A5E, the start)
 *
 * The length is rejected outright unless it is a multiple of 4, and the
 * three-quarter point in +0x104 is what raises the refill interrupt, so a
 * driver that ignores +0x104 gets one buffer and then silence.
 *
 * +0x100 is not part of that descriptor - it is the direction's own control
 * register, and the two inits touch two different things in it: 0x00D94A6E
 * sets bit 0, and 0x00D96774 clears the 4-bit field at [15:12]. [?] both.
 * [M] It resets to 0x00000700, and the capture side resets to 0x02300700.
 */
#define SL6806_AUD_TX_ENABLE    SL6806_AUD_REG(0x100)
#define SL6806_AUD_TX_EN_BIT    (1u << 0)   /* [V] set by the hardware init  */
#define SL6806_AUD_TX_TRIG      SL6806_AUD_REG(0x104)
#define SL6806_AUD_TX_CTRL      SL6806_AUD_REG(0x108)
#define SL6806_AUD_TX_ADDR      SL6806_AUD_REG(0x10C)

/* [V] Capture DMA, +0x200..+0x20C, from 0x0080D964. Same four registers,
 * same order, one page up. */
#define SL6806_AUD_RX_ENABLE    SL6806_AUD_REG(0x200)
#define SL6806_AUD_RX_TRIG      SL6806_AUD_REG(0x204)
#define SL6806_AUD_RX_CTRL      SL6806_AUD_REG(0x208)
#define SL6806_AUD_RX_ADDR      SL6806_AUD_REG(0x20C)

/* [V] The length field of the CTRL register, and the start bit. */
#define SL6806_AUD_LEN_SHIFT    16
#define SL6806_AUD_LEN_MASK     0xFFFF0000u
#define SL6806_AUD_START        (1u << 4)

/* [V] The trigger word: watermark in [31:16], bit 0 arms the interrupt. */
#define SL6806_AUD_TRIG_ARM     (1u << 0)

/*
 * [V] Interrupt flags, in the CTRL register of each direction. The handler at
 * SRAM 0x0080D8F8 reads CTRL, writes the same value straight back - so these
 * are write-1-to-clear - and then tests bit 8 and bit 10, dispatching to two
 * different callbacks.
 *
 * [I] WHICH IS WHICH. Bit 8 is taken here as the watermark the trigger
 * register arms and bit 10 as end-of-buffer, because +0x104 programs a
 * three-quarter point and a three-quarter point exists to be a refill
 * warning. Nothing in the dump names them; if a hardware run shows bit 10
 * arriving at 75% of the buffer, these two are simply swapped.
 */
#define SL6806_AUD_IRQ_WATER    (1u << 8)
/* [M] RETRACTED as "done". It raises within microseconds of a submit on a
 * block that provably moves no memory (examples/AudioWall: 0 of 64 words
 * changed). An error or abort flag fits better. The name is kept because the
 * vendor's handler dispatches on it; the meaning is [?]. */
#define SL6806_AUD_IRQ_DONE     (1u << 10)
#define SL6806_AUD_IRQ_ALL      (SL6806_AUD_IRQ_WATER | SL6806_AUD_IRQ_DONE)

/* [V] IRQ line, from 0x00D967D0..0x00D967E4: priority 2, vector installed
 * through ROM 0x1C28, then enabled. Nothing here uses it - see the note on
 * polling in sl6806_audio_play() - but a sketch that wants an ISR needs the
 * number. */
#define SL6806_AUD_IRQ          30

/*
 * [V] Volume. Three playback channels at 0x20 stride from +0x134, four
 * capture channels at 0x30 stride from +0x224. Each is a pair: a 9-bit level
 * in the second register, and bit 16 of the first.
 *
 * [M] Bit 16 is the mute. It resets SET on all seven channels (measured), it
 * is the one bit the vendor's init clears, and it clears it on exactly the
 * three playback channels. That was [I] from the clear alone; the reset
 * default settles it.
 *
 * [M] The levels reset to 0x1FF on the three playback channels and 0 on the
 * four capture ones, inside words of 0x0000A1FF and 0x0000A000 - so bits 13
 * and 15 are set in both and belong to something else. [?] what.
 */
#define SL6806_AUD_TX_VOL_CTRL(ch)  SL6806_AUD_REG(0x134u + (uint32_t)(ch) * 0x20u)
#define SL6806_AUD_TX_VOL(ch)       SL6806_AUD_REG(0x138u + (uint32_t)(ch) * 0x20u)
#define SL6806_AUD_RX_VOL_CTRL(ch)  SL6806_AUD_REG(0x224u + (uint32_t)(ch) * 0x30u)
#define SL6806_AUD_RX_VOL(ch)       SL6806_AUD_REG(0x228u + (uint32_t)(ch) * 0x30u)

#define SL6806_AUD_TX_CHANNELS  3           /* [V] the init writes three     */
#define SL6806_AUD_RX_CHANNELS  4           /* [V] the dispatcher has four   */
#define SL6806_AUD_VOL_MAX      0x1FFu      /* [V] the init's own value      */
#define SL6806_AUD_VOL_MUTE     (1u << 16)  /* [I] in the CTRL half          */

/* [V] 128 words at +0x400..+0x5FC that the hardware init zeroes one at a
 * time, each with a read-back poll. The application logs "hardware EQ open
 * success, init max sample: %d", which is what a coefficient RAM this size
 * next to an audio DAC is for. Not touched by this driver. */
#define SL6806_AUD_COEFF_BASE   SL6806_AUD_REG(0x400)
#define SL6806_AUD_COEFF_WORDS  128

/* ------------------------------------------------------------------ */
/* Clocks                                                              */
/* ------------------------------------------------------------------ */

/*
 * [V] Module clock id 37, from 0x00D96754: `movs r0,#37; bl 0x1EC0`, and
 * ROM 0x1EC0 is a tail-branch to 0x1C5C, which sl6806_module.h already
 * documents as module_clock_enable. Id 37 lands in the 32..63 bank, so it is
 * CRU +0x74 shadow then +0x64 gate, bit 5.
 */
#define SL6806_AUD_MODULE_ID    37

/*
 * [V] And a romclk id, 19, immediately after: `movs r0,#19; bl 0x008051EC`,
 * and 0x008051EC is a tail-branch to ROM 0x20EC, which is the second clock
 * family sl6806_romclk.h tabulates. Entry 19 is 0x40080088 bit 0.
 *
 * Both are needed. The module clock gives the block its registers; this is
 * the one that has, in every other peripheral this project has brought up,
 * turned out to be the difference between registers that answer and hardware
 * that runs.
 */
#define SL6806_AUD_ROMCLK_ID    19

/*
 * [V] The hardware init borrows a second pair - module 32 and romclk 45 -
 * plus bit 2 of the pad-mux register at 0x40000020, for exactly as long as it
 * takes to clear the coefficient RAM, and then gives all three back
 * (0x00D94AA6..0x00D94AEA). This driver does not clear that RAM, so it does
 * not take them; they are recorded because the pattern - a second clock
 * domain for the EQ block behind the same base address - is the sort of thing
 * that explains a later mystery.
 */
#define SL6806_AUD_EQ_MODULE_ID 32
#define SL6806_AUD_EQ_ROMCLK_ID 45
#define SL6806_AUD_PADMUX_REG   0x40000020u
#define SL6806_AUD_PADMUX_EQ    (1u << 2)

/*
 * =====================================================================
 *  THE AUDIO PLL, WHICH IS A DIFFERENT PLL
 * =====================================================================
 * [V] 0x00807300 in the SRAM blob takes a frequency and programs CRU +0x10
 * and +0x14. It knows exactly two frequencies, and they are the two every
 * audio clock tree in the world is built from:
 *
 *     24,576,000 Hz  = 48000 x 512   -> select 3, ratio 0x3126
 *     22,579,000 Hz  ~ 44100 x 512   -> select 2, ratio 0x000186C2
 *
 * and the caller picks between them with `rate % 8000` (0x00D979E0): a rate
 * that divides by 8000 belongs to the 48 kHz family, anything else to the
 * 44.1 kHz one. That single modulo is the strongest single piece of evidence
 * in this file that 0x40009000 is audio and not anything else.
 *
 * The sequence, in order:
 *
 *     while (CRU[0x14] & BUSY) ;
 *     CRU[0x10] = (CRU[0x10] & 0xFC1F0000) | 0x3000 | (select << 5);
 *     CRU[0x14] = (CRU[0x14] & 0x3800) | (1 << 31) | (1 << 10) | (ratio << 14);
 *
 * with a lock/unlock pair around it (0x00804DA8 / 0x00804DA4) that is a
 * scheduler critical section, not hardware.
 */
#define SL6806_AUD_PLL_SEL      0x40080010u
#define SL6806_AUD_PLL_RATIO    0x40080014u
#define SL6806_AUD_PLL_BUSY     (1u << 11)  /* [V] polled before writing     */
#define SL6806_AUD_PLL_SEL_KEEP 0xFC1F0000u /* [V] bits the vendor preserves */
#define SL6806_AUD_PLL_SEL_SET  0x3000u     /* [V] and the bits it forces    */
#define SL6806_AUD_PLL_SEL_SHIFT 5
#define SL6806_AUD_PLL_RATIO_KEEP 0x3800u
#define SL6806_AUD_PLL_RATIO_SET  ((1u << 31) | (1u << 10))
#define SL6806_AUD_PLL_RATIO_SHIFT 14

/*
 * =====================================================================
 *  [V] THE BIT CLOCK - the thing that paces the DMA
 * =====================================================================
 * [M] 2026-08-13 found this the hard way: with the route written and the DAC
 * enabled, a 25 ms buffer still completed in a mean of **10 microseconds** on
 * all four routes. 4796 bytes in 10 us is 480 MB/s on a 64 MHz Cortex-M4,
 * which is not a transfer at all - it is a block with nothing clocking its
 * output stage, retiring the descriptor as fast as it can read it.
 *
 * What was missing is the vendor's *stream start*, 0x00D9662C, which is a
 * different routine from the init at 0x00D96710 and which this driver had
 * never looked at:
 *
 *     module_clock_disable(2); delay(10);
 *     module_clock_enable(2);  delay(10);
 *     romclk_set(44, 8);       -> ROM 0x2B1A: 0x40080094[10:8] = 8 - 1
 *     romclk_enable(44);       -> 0x40080094 |= 1
 *     0x40009400 |= 0x80;
 *     then one of:  0x40009080[11:9]  = 6      (mode 1)
 *                   0x4000907C[26:24] = 6      (mode 2)
 *                   0x4000907C[5:3]   = 6      (mode 3)
 *
 * ROM 0x2B1A is in the mask ROM's *third* clock family - the setter at
 * 0x289C, which takes an id and a value, as opposed to the enable-only family
 * at 0x20EC that sl6806_romclk.h tabulates. Its whole body for id 44 is a
 * 3-bit divider field at [10:8] of 0x40080094, written as value - 1.
 *
 * AND THE ARITHMETIC SETTLES IT. The vendor passes 8, and:
 *
 *     24,576,000 / 8 = 3,072,000 = 64 x 48000
 *     22,579,000 / 8 = 2,822,375 = 64 x 44100
 *
 * Both families land on exactly 64 fs - the bit clock for 32-bit stereo
 * frames. A divider that produces 64 fs from either master clock is the
 * audio bit clock and nothing else.
 */
#define SL6806_AUD_BITCLK_REG       0x40080094u
#define SL6806_AUD_BITCLK_ENABLE    (1u << 0)   /* [V] romclk 44's bit       */
#define SL6806_AUD_BITCLK_DIV_SHIFT 8
#define SL6806_AUD_BITCLK_DIV_BITS  3
#define SL6806_AUD_BITCLK_DIV       8u          /* [V] the vendor's value    */

/* [V] Cycled off and on at the start of every stream (0x00D96646/0x00D96650).
 * [?] What it clocks. */
#define SL6806_AUD_STREAM_MODULE_ID 2

/* [V] 0x40009400 |= 0x80 at stream start. The init clears +0x400..+0x5FC as a
 * coefficient RAM, so this word is both a control register and the first word
 * of that array - or the array does not start until +0x404. [?] */
#define SL6806_AUD_EQ_CTRL          SL6806_AUD_REG(0x400)
#define SL6806_AUD_EQ_CTRL_START    (1u << 7)

/*
 * [V] And a 3-bit source field set to 6, in one of three places depending on
 * a driver-state byte. Their reset value is 4 - +0x07C and +0x080 both come
 * up 0x24924924, which is every 3-bit field at 4 - so this is a select, and
 * the vendor moves one of them off its default.
 *
 * [?] Which of the three is playback. sl6806_audio_clock_start() takes it as
 * a parameter and examples/ToneDemo tries all three.
 */
#define SL6806_AUD_SRC_MODES        3

/* [V] The two families, named by the constants the vendor compares against. */
#define SL6806_AUD_MCLK_48K     24576000u
#define SL6806_AUD_MCLK_44K1    22579000u
#define SL6806_AUD_PLL_48K_SEL      3u
#define SL6806_AUD_PLL_48K_RATIO    0x3126u
#define SL6806_AUD_PLL_44K1_SEL     2u
#define SL6806_AUD_PLL_44K1_RATIO   0x000186C2u

/*
 * [V] 0x400F70D8 bit 1, set once before the module clocks in 0x00D9674A.
 * 7c files 0x400F7000 as the storage host and 7m found the register file's
 * mailbox at +0x100 of the same base, so this is a third tenant of that
 * block. [?] as to what it gates; it is one OR, it is cheap, and leaving it
 * out is the kind of omission that produces a silent negative.
 */
#define SL6806_AUD_MISC_REG     0x400F70D8u
#define SL6806_AUD_MISC_BIT     (1u << 1)

/* ------------------------------------------------------------------ */
/* Pure logic, shared with the host tests                              */
/* ------------------------------------------------------------------ */

/*
 * Which master clock a sample rate wants. Exactly the vendor's test: the
 * 8000 Hz family (8k, 16k, 24k, 32k, 48k, 96k) against everything else.
 */
static inline uint32_t sl6806_audio_mclk_for(uint32_t rate)
{
    return (rate % 8000u) ? SL6806_AUD_MCLK_44K1 : SL6806_AUD_MCLK_48K;
}

/*
 * The trigger word for a buffer: watermark at three quarters, interrupt
 * armed. Kept as a function because it is the one bit of arithmetic in the
 * submit path and the one worth a test.
 */
static inline uint32_t sl6806_audio_trigger_word(uint32_t len)
{
    return (((len * 3u) / 4u) << SL6806_AUD_LEN_SHIFT) | SL6806_AUD_TRIG_ARM;
}

/* ------------------------------------------------------------------ */
/* The driver                                                          */
/* ------------------------------------------------------------------ */

/*
 * Bring the block up for playback at `rate`.
 *
 * Performs, in the vendor's order: the 0x400F70D8 bit, module clock 37,
 * romclk 19, the audio PLL for the rate's family, the hardware init's
 * register writes, and the DAC enables. It does NOT clear the coefficient
 * RAM and does not install an interrupt handler.
 *
 * [M] Returns 1 if the module clock acknowledged, and configures the block
 * either way. It used to also veto on sl6806_audio_writable(), and that was
 * removed on 2026-08-13 for two reasons: the write test was wrong (see its
 * comment in the .c), and thirty registers coming up at their reset defaults
 * is far better evidence that the block is awake than one scratch write is
 * evidence that it is not.
 */
int sl6806_audio_begin(uint32_t rate);

/*
 * Select the output route for one channel (0 or 1), 0..3, and write the 5-bit
 * trim at +0x08 [12:8].
 *
 * [M] **You have to call this.** begin() does not, and a run on 2026-08-13
 * moved a whole buffer through the DMA with +0x08 still reading zero: the
 * descriptor path works without an output stage, it just finishes instantly
 * and makes no sound. The measured symptom to watch for is the completion
 * time - 4796 bytes of 48 kHz stereo is 25 ms of audio, and it was finishing
 * in under 1 ms.
 *
 * [?] Which route is the speaker and which the headphone jack is unknown;
 * examples/ToneDemo tries all four. `trim` is a 5-bit field the vendor takes
 * from its own driver state and this analysis did not follow - 0 is as good
 * a guess as any and is what ToneDemo passes.
 */
void sl6806_audio_route(unsigned ch, unsigned route, unsigned trim);

#define SL6806_AUD_ROUTES       4

/*
 * Start the bit clock and the output stage: the vendor's stream start,
 * 0x00D9662C. `mode` is 1, 2 or 3 and selects which of the three 3-bit source
 * fields is moved to 6 - see SL6806_AUD_SRC_MODES above.
 *
 * [M] **The DMA is not paced without this.** Before it existed, a 25 ms
 * buffer completed in 10 us on every route. begin() calls it with mode 1;
 * examples/ToneDemo sweeps all three against all four routes, because the
 * completion time is a number and the combination that produces 25000 us is
 * the answer.
 *
 * ONE DELIBERATE DEVIATION. The vendor disables module clock 2 and
 * re-enables it; this only enables. Turning off an unidentified module clock
 * on a chip running the code that turns it off is the kind of thing this
 * codebase has a rule about, and enabling alone should be sufficient on a
 * cold block. If the clock turns out to need the cycle, that is the first
 * thing to try.
 */
void sl6806_audio_clock_start(unsigned mode);

/*
 * Hold the EQ sub-block's three clocks - module 32, romclk 45 and bit 2 of
 * 0x40000020 - instead of borrowing them for an init the way the vendor does.
 *
 * [?] A hypothesis with a witness, and the last one standing: everything else
 * that could explain a descriptor retiring in 10 us has been eliminated (the
 * route, the bit clock, all 32 bits of 0x400E0000, and the general DMA
 * controller, whose only two call paths in the image are the LCDC's).
 * examples/ToneDemo takes ToneDemo EQ=1 to try it.
 */
void sl6806_audio_eq_hold(int on);

/* Undo the mode select and stop the bit clock. */
void sl6806_audio_clock_stop(void);

/* Drop the DAC enables and the start bit. Leaves the clocks alone, because
 * turning a clock off is the half of this that could take something else
 * with it and nothing here needs it. */
void sl6806_audio_end(void);

/*
 * Hand one buffer to the playback DMA and start it.
 *
 * `buf` must be word-aligned and `len` a multiple of 4 - the vendor's own
 * routine returns -1 rather than truncate, and so does this. Returns 0 on
 * success, -1 if rejected.
 *
 * THE BUFFER IS NOT COPIED. The DMA reads it where it lies, so it has to stay
 * put and unmodified until the transfer completes; a buffer on the stack of
 * the function that submitted it is the classic way to hear noise.
 *
 * There is no interrupt. IRQ 30 exists and the vendor uses it, but a payload
 * shares its vector table with the boot ROM and installing a handler there is
 * a much larger change than this is worth before anyone has seen the block
 * move at all. Poll sl6806_audio_done() instead.
 */
int sl6806_audio_play(const void *buf, uint32_t len);

/*
 * Has the current buffer finished? Reads the playback CTRL register and, if
 * the completion flag is set, clears it the way the vendor's handler does -
 * by writing the whole word back - and returns 1.
 *
 * Call this rather than watching the length field: nothing establishes that
 * the length counts down.
 */
int sl6806_audio_done(void);

/*
 * Capture into `buf`, same rules as play(). Present for one reason: it is the
 * only way to catch the DMA engine touching memory. Pre-fill the buffer, run
 * a capture, and if the contents change the engine moves data - which
 * separates "the output stage is unclocked" from "the completion flag is a
 * lie". examples/AudioWall does exactly that before it walks anything.
 */
int sl6806_audio_capture(void *buf, uint32_t len);
int sl6806_audio_capture_done(void);

/* Clear the start bit. Does not wait for anything. */
void sl6806_audio_stop(void);

/* Playback volume, 0..511, on one of the three channels. Anything larger is
 * clamped to SL6806_AUD_VOL_MAX. */
void sl6806_audio_volume(unsigned ch, unsigned level);

/* Mute bit for one playback channel. [I] - see SL6806_AUD_VOL_MUTE. */
void sl6806_audio_mute(unsigned ch, int on);

/*
 * Does the block answer writes? A diagnostic, not a gate - see the note on
 * begin(). Tries the playback address register with a real SRAM address, then
 * the 9-bit playback level, and restores both.
 *
 * [M] The first version wrote 0x5A5A5A50 here and reported "drops writes" on
 * a block that had visibly just woken up. A DMA address register that will
 * not hold an address outside its memory map is ordinary; the test was not.
 * Safe to call before sl6806_audio_begin(), though it means more after.
 */
int sl6806_audio_writable(void);

/*
 * Read the playback interrupt flags and clear them. Exposed because
 * examples/AudioProbe wants to watch them, and because a flag that sets
 * itself with no buffer submitted would say something about this block that
 * nothing in the dump does.
 */
uint32_t sl6806_audio_flags(void);

#ifdef __cplusplus
}
#endif

#endif /* SL6806_AUDIO_H */
