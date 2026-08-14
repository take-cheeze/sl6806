/*
 * sl6806_hwinit.h - do what the bootloader's hardware_init does, without
 * leaving. The other half of "semi-firmware".
 *
 * =====================================================================
 *  THE IDEA
 * =====================================================================
 * examples/FirmBoot hands the device to the vendor's application, and the
 * price is everything: USB goes, the console goes, sketches stop being
 * uploadable, and the only observables left are the panel and the host's USB
 * bus. That is the right experiment for "does the product work", and the
 * wrong one for "why does a payload behave differently".
 *
 * This is the inverse. Rather than jumping into the firmware's environment,
 * it **builds that environment where the payload already is** - performing
 * the parts of the bootloader's `hardware_init` (§24) that a payload can
 * perform, and then returning, with USB up, the console alive, and the next
 * sketch still one `make run` away.
 *
 * =====================================================================
 *  WHY IT MIGHT MATTER
 * =====================================================================
 * The SD host takes a command and reports nothing, with seven hypotheses
 * closed by measurement (docs/SD.md). The difference that remains is that a
 * payload runs in bootloader mode, where - measured this session - the mask
 * ROM leaves both the SD host and the serial port switched off, and where
 * nothing has ever started the PLL.
 *
 * **The bootloader sets the PLL to 384 MHz before it touches anything else**
 * (§25, ROM 0x1F6C via 0x008206D0). A payload has never done that. If the SD
 * host's command state machine is fed from a clock domain that the PLL
 * drives, a payload is asking it to shift bits out with nothing to shift them
 * on - which would look exactly like what has been measured: the command
 * register accepts a word, the busy bit drops in under a microsecond, and the
 * status register never says anything.
 *
 * That is a hypothesis, not a finding. It is cheap to test and it is the only
 * one left that does not need an oscilloscope.
 *
 * =====================================================================
 *  WHAT THIS CAN COST
 * =====================================================================
 * **The USB link, if the ROM's USB is fed from what this changes.** Nothing
 * establishes where the ROM clocks USB from, so changing the PLL while it is
 * enumerated may take the console with it. That is recoverable - power-cycle
 * and you are back in bootloader mode - but it means a sketch must say
 * everything it has to say *before* calling this, and check afterwards
 * whether it still has a console.
 *
 * Nothing here writes flash. The whole risk is a link that has to be
 * re-established by unplugging.
 */
#ifndef SL6806_HWINIT_H
#define SL6806_HWINIT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ⚠ [M] 2026-08-14: THE SETTER BELOW DOES NOT MOVE THE READBACK.
 *
 * §25 paired ROM 0x1F6C (which writes +0x10 and +0x14) with ROM 0x1FB8
 * (which reads +0x00) as the setter and readback of one PLL, on the strength
 * of their being adjacent in the ROM. examples/SdWithPll performed those two
 * writes on hardware and `0x40080000` did not change by a single bit.
 *
 * So sl6806_pll_set_384() writes what the vendor writes and this project
 * cannot yet say what it drives. sl6806_pll_hz() is unaffected and is
 * confirmed: it read 192 MHz on a live device, with the lock bit set, and
 * 192/3 is the measured 64 MHz core clock exactly.
 *
 * The register that plainly *does* carry this PLL's rate is +0x00 itself.
 * Changing its divider field from 2 to 1 would ask for 384 MHz directly, and
 * would double the core clock while doing it - which is a sketch with its own
 * countdown, not a library call.
 */

/* [V] §25, §30. The CRU's PLL rate, and the registers the vendor writes. */
#define SL6806_PLL_STATUS   0x40080000u   /* (m * 48 / d) MHz */
#define SL6806_PLL_CTRL     0x40080010u
#define SL6806_PLL_MUL      0x40080014u

/* [V] the constants ROM 0x1F6C writes for anything but 24.576 MHz. */
#define SL6806_PLL_CTRL_MASK  0xFC1F0000u
#define SL6806_PLL_CTRL_BITS  0x00003040u   /* 0x3000 | 0x40 */
#define SL6806_PLL_MUL_MASK   0x00003800u
#define SL6806_PLL_MUL_BITS   0x80000400u
#define SL6806_PLL_MUL_VALUE  0x000186C2u   /* << 14 */

/* [V] what the bootloader asks for, hardware_init -> 0x008206D0. */
#define SL6806_PLL_TARGET_HZ  384000000u

/* Read the PLL's current rate in Hz, decoded the way ROM 0x1FB8 does:
 * ((status >> 8) & 0xFF) * 48 / (status & 0xF) megahertz. Returns 0 if the
 * divider field reads zero, which is not a rate. */
uint32_t sl6806_pll_hz(void);

/*
 * Set the PLL the way the bootloader does: 384 MHz.
 *
 * Returns 1 if the rate reads back as 384 MHz afterwards, 0 otherwise. It
 * does not wait for a lock - nothing in the vendor's routine does, and §17
 * records that this chip's other PLL reports lock through a different
 * register entirely.
 *
 * ⚠ May take USB with it. See the header comment.
 */
int sl6806_pll_set_384(void);

#ifdef __cplusplus
}
#endif

#endif /* SL6806_HWINIT_H */
