/*
 * sl6806_regfile.h - the indexed register file, and the rails on it.
 *
 * =====================================================================
 *  FOUND. IT IS A MAILBOX AT 0x400F7000 + 0x100, NOT AN MMIO ARRAY.
 * =====================================================================
 * docs/sl6806_re_notes.md 7l spent a section explaining that this file could
 * not be located: the two accessors live at SRAM 0x00804E44 (write) and
 * 0x00804EAC (read), 140 call sites hang off them, and no pattern search over
 * dump.bin, maskrom.bin or sram.bin ever produced the base. The conclusion
 * was that the accessors were run-time-written veneers and that only SWD on a
 * running stock firmware could settle it.
 *
 * That was wrong, and the reason is worth one paragraph because it invalidates
 * a whole family of earlier negatives. The FIRM header's 0x00804C00 is the
 * *entry*, not the load address; the first-stage segment at file 0x10030 loads
 * to 0x00804800, exactly as 13a says, and the reset handler sits 0x400 in
 * behind the vector table. Disassembled at 0x00804800 the two addresses are
 * ordinary function prologues in flash - `push {r4, r5, r6, lr}` at 0x00804E44
 * - and they always were. The earlier search used the header's word as the
 * base, landed 0x400 out, found floating-point DSP, and concluded the code was
 * not in the image. It is; tools/sl6806-sram --stage1 now maps it.
 *
 * Both accessors are a four-register mailbox:
 *
 *     write(idx, val):  WDATA <- val
 *                       CMD   <- 0x40000060 | (idx << 8)
 *                       CMD   |= START
 *                       spin while CMD & START
 *                       error = STAT & 0xF000
 *
 *     read(idx):        CMD   <- 0x40000061 | (idx << 8)
 *                       CMD   |= START
 *                       spin while CMD & START
 *                       value = RDATA & 0xFF
 *
 * The low byte differing by exactly one between the two directions is an 8-bit
 * I2C address with its R/W bit: device 0x30, written 0x60, read 0x61. So the
 * "indexed register file" is a chip on a serial bus that this block masters -
 * which is why 7l's Hough-style vote over MMIO literals could never have hit
 * it, and why the register numbers behave like a datasheet's rather than like
 * offsets.
 *
 * =====================================================================
 *  AND THE "CLOCK CHANNELS" ARE VOLTAGE RAILS
 * =====================================================================
 * 7h read 0x00CC769C as clk(channel, op, arg) because the camera calls it with
 * 2800 and 2800 looks like a frequency in kHz. It is not. The channel-6 setter
 * at 0x00CC7536 decodes its argument as
 *
 *     1700..2450 mV  ->  (mv - 1700) / 50
 *     2650..3300 mV  ->  (mv - 2650) / 50 + 16
 *
 * - 50 mV steps over a 1.7 V to 3.3 V span, in a 5-bit field. That is an LDO,
 * and 2800 is 2.8 V, which is the supply every camera sensor of this class
 * wants. So the rail the README says the vendor switches and a payload cannot
 * is this one, and it is now three register writes away.
 *
 * WHAT IS AND IS NOT ESTABLISHED. Everything below is transcribed from the
 * vendor's own code and marked [V]. Nothing here has been run on hardware:
 * this file was written from the dump in one sitting, and the sensor has never
 * answered. Treat a successful chip-id read from examples/CameraDemo as the
 * first evidence that any of it works.
 *
 * =====================================================================
 *  THE WRITES ARE REAL. READ THIS BEFORE CALLING A WRITE.
 * =====================================================================
 * This file switches power rails on a chip that is running the code doing the
 * switching. Channels 2..6 are five separate supplies and only channel 6 is
 * known to be the camera's; the other four go somewhere, and "somewhere" on a
 * SoC includes the core, the SRAM the payload is executing from and the USB
 * PHY the console is on. Turning off the rail under your own feet is a hang at
 * best, and the console is a ring in the device's own RAM read back over the
 * link that would die with it, so there would be no log either.
 *
 * Hence: the reads are free, the LDO helpers take a channel and will do what
 * you ask, and sl6806_camera_power_on() is the only preset, because it is the
 * only sequence the vendor is known to perform.
 *
 * Provenance markers as elsewhere: [V] verified against the dump,
 * [M] measured on hardware, [I] inferred, [?] unknown.
 */
#ifndef SL6806_REGFILE_H
#define SL6806_REGFILE_H

#include <stdint.h>
#include "sl6806_mmio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* The mailbox                                                         */
/* ------------------------------------------------------------------ */

/*
 * [V] Base, from the literal pools at 0x00804EA8 and 0x00804EF8.
 *
 * 7c files 0x400F7000 as the SD/MMC and SPI flash host, and that is not a
 * contradiction: this is one sub-block at +0x100 of a larger unit, the same
 * way 15b found module gates for ids 96..127 at 0x400F1000 +0x20. Whatever
 * else lives here, these four registers are the serial master.
 */
#define SL6806_RF_BASE          0x400F7000u

/* [V] Command. [31] start, clears when the transfer completes; [15:8] the
 * register index; [7:0] the target's 8-bit address with the R/W bit. */
#define SL6806_RF_CMD           0x104
#define SL6806_RF_WDATA         0x108   /* [V] byte to write               */
#define SL6806_RF_RDATA         0x10C   /* [V] byte read back, in [7:0]    */
#define SL6806_RF_STAT          0x110   /* [V] [15:12] non-zero = failure  */

#define SL6806_RF_START         (1u << 31)
#define SL6806_RF_ADDR_WRITE    0x60u   /* [V] device 0x30, write          */
#define SL6806_RF_ADDR_READ     0x61u   /* [V] device 0x30, read           */
#define SL6806_RF_STAT_ERR      0xF000u /* [V] the bits the writer tests   */

/*
 * [V] Bit 30, and [?] as to what it means.
 *
 * The vendor's literals are `0x40000060` and `0x40000061`, not `0x60` and
 * `0x61` - the whole word goes in, index shifted over it. Bit 30 is therefore
 * part of every command this block is known to have executed, and nothing
 * says what it selects: transfer length, bus speed, which of several targets,
 * or simply a bit that must be 1. It is carried rather than dropped, because
 * "the vendor sets it and we do not know why" is a much better reason to keep
 * a bit than to remove it.
 */
#define SL6806_RF_CMD_FIXED     0x40000000u

/*
 * How long to wait for START to clear, in polls.
 *
 * The vendor spins without a bound. A payload must not: 15b is the whole
 * lesson, an unbounded poll in payload mode runs inside the boot ROM's USB
 * handler and takes the device off the bus with no log. A transfer to a chip
 * on a serial bus is microseconds, so anything that has not finished in a
 * few thousand polls is not going to.
 */
#define SL6806_RF_TIMEOUT       20000u

/*
 * Read one register. Returns 0..255, or -1 if the mailbox never completed.
 *
 * A -1 is a statement about the mailbox, not about the register: it means the
 * transfer did not finish, so nothing was read. Callers that want to tell a
 * dead block from a live one should check for it rather than treating the
 * result as a byte.
 */
int sl6806_reg_read(unsigned idx);

/*
 * Write one register. Returns 0 on success, 1 if the block reported an error
 * in STAT, -1 on timeout. Those are the writer's own two outcomes at
 * 0x00804E44 plus the bound this adds.
 */
int sl6806_reg_write(unsigned idx, uint8_t val);

/*
 * Read-modify-write: value = (value & keep) | set. Returns as
 * sl6806_reg_write, or -1 if the read failed.
 *
 * Every one of the vendor's 50 write sites has this shape - read, mask with a
 * literal, or in a literal, write - so this is the operation, and a bare
 * sl6806_reg_write of a whole byte is almost always a mistake.
 */
int sl6806_reg_update(unsigned idx, uint8_t keep, uint8_t set);

/* ------------------------------------------------------------------ */
/* The rails                                                           */
/* ------------------------------------------------------------------ */

/*
 * [V] Channel enables: register 0x2C, bit (channel - 2). From the enable and
 * disable paths at 0x00CC73A4 and 0x00CC7422, which are the only thing those
 * paths do for every channel but 4.
 *
 * 7l guessed this register was "the clock driver's channel bitmask" from a
 * single sample of 0x4A on the wrong peripheral. The guess was right about
 * what the register means and wrong about where it lives.
 */
#define SL6806_RF_LDO_ENABLE    0x2Cu

/* [V] Per-channel voltage registers, from the op-2 jump table at 0x00CC7440
 * and confirmed by the op-3 readback table at 0x00CC7580. */
#define SL6806_RF_LDO2_MV       0x0Du   /* [V] 1500 mV + n*100, 5 bits      */
#define SL6806_RF_LDO3_MV       0x0Eu   /* [V] 2650 mV + n*50,  4 bits      */
#define SL6806_RF_LDO4_MV       0x03u   /* [V] a six-entry menu, bits 1/5/7 */
#define SL6806_RF_LDO5_MV       0x79u   /* [V] the split 50 mV scale        */
#define SL6806_RF_LDO6_MV       0x7Au   /* [V] the same, the camera's       */

/* [V] Channel 4 alone toggles this around a change; 0x00CC7350 sets bit 7
 * before and 0x00CC73C4 clears it after a delay. Not needed for the camera,
 * recorded because it is part of that channel's sequence. */
#define SL6806_RF_LDO4_GUARD    0x1Bu

/* [V] The camera's rail and its voltage, from the power-on at 0x00D44B1C:
 * disable channel 6, wait, set 2800, enable. */
#define SL6806_LDO_CAMERA       6
#define SL6806_LDO_CAMERA_MV    2800

/*
 * Enable or disable one rail, channels 2..6. Returns 0 on success, non-zero
 * as sl6806_reg_write, or -1 for a channel outside 2..6.
 *
 * Channel 4 carries the extra register-3 and register-0x1B handling the
 * vendor does for it; the others are one bit in 0x2C.
 */
int sl6806_ldo_enable(unsigned ch);
int sl6806_ldo_disable(unsigned ch);

/*
 * Set a rail's voltage in millivolts. Returns 0 on success, -1 for a bad
 * channel, and otherwise as sl6806_reg_write.
 *
 * The encodings are per channel and are transcribed, including their
 * truncating division and their out-of-range behaviour - channels 5 and 6
 * program 0 for anything between 2450 and 2650 mV, which is what the vendor's
 * `else` branch does and is very likely a gap in the part rather than a bug.
 * Nothing here range-checks on your behalf: ask for a voltage the vendor
 * never asks for and you get whatever that arithmetic produces.
 */
int sl6806_ldo_set_mv(unsigned ch, unsigned mv);

/*
 * Read a rail's programmed voltage back in millivolts, or -1 if the channel
 * is out of range or the read failed. This inverts the encoding above, using
 * the vendor's own inverse at 0x00CC757A.
 *
 * Channel 4 is not readable this way - the vendor keeps its last setting in a
 * RAM variable at 0x0082BD14 rather than decoding the register - so it
 * returns -1.
 */
int sl6806_ldo_get_mv(unsigned ch);

/* ------------------------------------------------------------------ */
/* The camera                                                          */
/* ------------------------------------------------------------------ */

/*
 * [V] The sensor's two enable bits, from the sensor bring-up at 0x00D44EA6
 * and 0x00D44F06. Register 3 field [4:3] = 01, register 0x16 bit 7 set.
 *
 * Register 3 is also channel 4's voltage menu, which is not a conflict: that
 * menu is bits 1, 5 and 7 (the vendor masks with 0x5D, keeping 0, 2, 3, 4 and
 * 6), and the camera's field is bits 3 and 4. Two fields, one byte, and each
 * driver preserves the other's - which is a good reason to go through
 * sl6806_reg_update rather than writing the byte.
 */
#define SL6806_RF_CAM_FIELD     0x03u
#define SL6806_RF_CAM_FIELD_KEEP 0xE7u
#define SL6806_RF_CAM_FIELD_SET  0x08u
#define SL6806_RF_CAM_ENABLE    0x16u
#define SL6806_RF_CAM_ENABLE_BIT 0x80u

/*
 * The whole vendor power-up for the sensor, in order, minus the pads.
 *
 *     1. rail 6 off
 *     2. 5 ms
 *     3. rail 6 to 2800 mV
 *     4. rail 6 on
 *     5. register 3 [4:3] <- 01
 *     6. register 0x16 bit 7 <- 1
 *
 * Steps 1-4 are 0x00D44B1C's clk() calls; 5 and 6 are the two writes the
 * sensor driver makes when it opens. The vendor interleaves the pads - MCLK's
 * mux before the rail, then RESET and PWDN after it - and this deliberately
 * does not, so that a caller which has its own opinion about which pad is
 * which (examples/CameraDemo swaps them as a variable) can sequence them
 * itself. Do the pads around this, not inside it.
 *
 * Returns 0 if every write succeeded, otherwise the step number that failed,
 * negated - so -3 means setting the voltage failed. Nothing is rolled back:
 * a half-applied rail is a state the vendor never produces and guessing at a
 * recovery would be inventing hardware behaviour.
 */
int sl6806_camera_power_on(void);

/* Undo it: the two sensor bits cleared, then rail 6 off. Same return
 * convention. */
int sl6806_camera_power_off(void);

#ifdef __cplusplus
}
#endif

#endif /* SL6806_REGFILE_H */
