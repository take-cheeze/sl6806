/*
 * sl6806_dvp.h - the camera front end at 0x400E1000, and the wall in front of it.
 *
 * =====================================================================
 *  THIS IS A MAP, NOT A DRIVER
 * =====================================================================
 * Nothing in this framework programs these registers, for the reason at the
 * bottom: the block's enable lives in 0x400E0000, and no payload has ever
 * written that register successfully. The map is here because it was believed
 * not to exist.
 *
 * WHAT IT OVERTURNS. Every previous note in this tree says the camera's pixel
 * path is undecoded and out of reach - "pixels leave the sensor on a DVP/CSI
 * parallel bus into hardware JPEG and H.264 encoders, and none of that is
 * decoded - the firmware reaches the front end as a named channel resource,
 * not as a register block". The named channel resource is real, but it sits
 * on top of an ordinary register block, and the block is right here. The
 * chain is short once you have the string: `-bind_dvp_channel CSI_CHAN
 * failed` at 0x00C7D473 is logged by 0x00D42834, which calls 0x00D44740,
 * which configures eleven pads and calls 0x00D98224, whose literal is
 * 0x400E1000.
 *
 * =====================================================================
 *  BANK 4 IS THE CAMERA PORT, ALL SIXTEEN PINS
 * =====================================================================
 * This falls out of the same function and is worth as much as the register
 * map. 0x00D44740 muxes eleven pads to alternate function 2, and with the
 * four already known from 7h they complete the picture:
 *
 *     pin  0, 1, 2    DVP sync and clock (PCLK / HSYNC / VSYNC, order [?])
 *     pin  3          MCLK out to the sensor                    [V] 7h
 *     pin  4..11      DVP data, eight bits                      [V]
 *     pin 12, 13      TWI0 SDA / SCL to the sensor              [V] 7h
 *     pin 14, 15      sensor RESET and PWDN                     [V] 7h
 *
 * Sixteen pins, one bank, one peripheral, all on function 2 except the two
 * power lines. 7g already had "bank 4 is the camera" from a pad sweep that
 * saw sixteen function-2 pads move together; what these eleven ids add is
 * which pin is which, from the driver instead of from the pattern - and a
 * confirmation that the sweep's grouping was right.
 *
 * Provenance markers as elsewhere: [V] verified against the dump,
 * [M] measured on hardware, [I] inferred, [?] unknown.
 */
#ifndef SL6806_DVP_H
#define SL6806_DVP_H

#include <stdint.h>

/* ------------------------------------------------------------------ */
/* The pads                                                            */
/* ------------------------------------------------------------------ */

/* [V] The eleven the vendor muxes, in the order it muxes them:
 * data first, then the three sync lines. 0x00D44780..0x00D447BC. */
#define SL6806_DVP_PAD_D0       0x00042130u   /* bank 4 pin  4, func 2 */
#define SL6806_DVP_PAD_D1       0x00042930u   /* bank 4 pin  5         */
#define SL6806_DVP_PAD_D2       0x00043130u   /* bank 4 pin  6         */
#define SL6806_DVP_PAD_D3       0x00043930u   /* bank 4 pin  7         */
#define SL6806_DVP_PAD_D4       0x00044130u   /* bank 4 pin  8         */
#define SL6806_DVP_PAD_D5       0x00044930u   /* bank 4 pin  9         */
#define SL6806_DVP_PAD_D6       0x00045130u   /* bank 4 pin 10         */
#define SL6806_DVP_PAD_D7       0x00045930u   /* bank 4 pin 11         */
#define SL6806_DVP_PAD_SYNC2    0x00041130u   /* bank 4 pin  2         */
#define SL6806_DVP_PAD_SYNC1    0x00040930u   /* bank 4 pin  1         */
#define SL6806_DVP_PAD_SYNC0    0x00040130u   /* bank 4 pin  0         */

/* [V] §7h's MCLK out to the sensor: bank 4 pin 3, the same function 2 as the
 * eleven above, which is what identifies the front end as its source. */
#define SL6806_DVP_PAD_MCLK     0x00041930u

/*
 * [?] Which of pins 0, 1 and 2 is PCLK, which HSYNC and which VSYNC is not in
 * the firmware: it muxes all three and never names them. They are numbered
 * here rather than guessed, because a guessed name in a header outlives the
 * guess. A scope on a working stock firmware settles it in one capture -
 * PCLK is the fast one.
 */

/* ------------------------------------------------------------------ */
/* The block                                                           */
/* ------------------------------------------------------------------ */

/* [V] Base, the only MMIO literal in the front-end HAL: 0x00D9821C and
 * 0x00D98254, both reached from the camera's own bind path. */
#define SL6806_DVP_BASE         0x400E1000u

#define SL6806_DVP_REG(off)     (*(volatile uint32_t *)(SL6806_DVP_BASE + (off)))

/*
 * [V] Control, 0x400E1000. Written last by the configure routine
 * (0x00D981FE): the format goes into [11:8], then bit 4 is set, then bit 1.
 * Two separate stores for the two bits, in that order.
 */
#define SL6806_DVP_CTRL             0x00
#define SL6806_DVP_CTRL_FMT_SHIFT   8       /* [V] 4 bits                  */
#define SL6806_DVP_CTRL_FMT_MASK    0x0F00u
#define SL6806_DVP_CTRL_BIT4        (1u << 4)   /* [?] set after the format */
#define SL6806_DVP_CTRL_BIT1        (1u << 1)   /* [?] set last - a start?   */

/* [V] The format the camera path uses. What 8 means is [?] - a pixel format
 * enum with no other value observed. */
#define SL6806_DVP_FMT_CAMERA       8

/*
 * [V] Input size, 0x400E1004. [11:0] = width - 1, [27:16] = height - 1.
 * The bind path fills these with the sensor's full array, 1280x720.
 *
 * Every size field in this block is stored minus one and every offset field
 * is stored as-is. That asymmetry is the vendor's, it is consistent across
 * all six registers, and getting it wrong is a one-pixel error that would be
 * invisible on a preview and wrong in a still.
 */
#define SL6806_DVP_INSIZE           0x04

/* [V] 0x400E1008. The open routine (0x00D98242) clears it and then sets bit
 * 4. [?] otherwise - it is touched nowhere else. */
#define SL6806_DVP_CFG08            0x08
#define SL6806_DVP_CFG08_BIT4       (1u << 4)

/* [V] The crop window, 0x00D9819E..0x00D981C8. Offsets as-is, sizes minus
 * one, all in [11:0]. */
#define SL6806_DVP_CROP_X           0x10
#define SL6806_DVP_CROP_Y           0x14
#define SL6806_DVP_CROP_W           0x18
#define SL6806_DVP_CROP_H           0x1C

/* [V] Output size, 0x400E1020: [11:0] = width - 1, [27:16] = height - 1. */
#define SL6806_DVP_OUTSIZE          0x20

/*
 * [V] Scaler, 0x400E1024. [13:0] horizontal, [29:16] vertical, each
 * (crop << 10) / out - a 10-bit fractional step, so 0x400 is 1:1 and larger
 * means downscaling. There is a scaler in front of the encoders, which is how
 * a 1280x720 sensor feeds a 240x296 panel.
 */
#define SL6806_DVP_SCALE            0x24
#define SL6806_DVP_SCALE_ONE        0x400u  /* 1:1, for arithmetic that reads */

/* [V] 0x400E1040, written by 0x00D98160 with the same width-1 / height-1
 * shape as INSIZE. [?] which stage it sizes. */
#define SL6806_DVP_SIZE40           0x40

/*
 * The geometry struct the configure routine takes, recovered field by field
 * from the loads at 0x00D98186..0x00D981D6. Recorded because the register
 * writes only make sense next to it, and because the bind path at
 * 0x00D447BE fills one in with literals.
 */
typedef struct {
    uint8_t  format;        /* +0x00  SL6806_DVP_FMT_CAMERA                */
    uint8_t  pad;
    uint16_t in_w, in_h;    /* +0x02  the sensor's array: 1280x720         */
    uint16_t crop_x, crop_y;/* +0x06                                       */
    uint16_t crop_w, crop_h;/* +0x0A                                       */
    uint16_t out_w, out_h;  /* +0x0E  after the scaler                     */
} sl6806_dvp_geometry_t;

/* ------------------------------------------------------------------ */
/* The wall                                                            */
/* ------------------------------------------------------------------ */

/*
 * [V] The peripheral enable, and the reason there is no driver above.
 *
 * 0x00D9A734 is `enable(id)`: 0x400E0000 |= 1 << id, then a 10 ms wait.
 * 0x00D9A74C is its disable. 0x00D9A768 is a reset pulse and it uses a
 * *different* register, 0x400E0008 - clear the bit, wait, set it again.
 * The camera front end is id 6, from the open routine at 0x00D98236.
 *
 * 15 records 0x400E0000 as dead from a payload, and that is why this file
 * stops here. But read that negative carefully before repeating it: it was
 * written when this register was known only as "the thing the application
 * enables peripherals through", with no id, no reset register and no
 * peripheral to check the result against. What is now available is a specific
 * bit, a specific second register, and a block at 0x400E1000 whose INSIZE
 * field would read back what was written to it if the block were alive.
 *
 * So the experiment is no longer "does 0x400E0000 take writes" - which is
 * unanswerable on its own - but:
 *
 *     1. read 0x400E1004 and remember it
 *     2. 0x400E0000 |= (1 << 6), wait 10 ms
 *     3. write a size to 0x400E1004 and read it back
 *     4. if it did not stick, pulse 0x400E0008 bit 6 and try again
 *     5. restore
 *
 * A block that holds a written size is enabled; one that does not is not.
 * That is the same read-back discipline that turned the register-file
 * mistake into a clean negative, and it is worth one careful bench run.
 */
#define SL6806_DVP_PERIPH_ENABLE    0x400E0000u
#define SL6806_DVP_PERIPH_RESET     0x400E0008u
#define SL6806_DVP_PERIPH_ID        6

/*
 * =====================================================================
 *  [M] RESOLVED ON HARDWARE, 2026-08-13 - AND NOT BY ANY OF THE ABOVE
 * =====================================================================
 * examples/DvpProbe ran the four mechanisms in order on a P20 and the answer
 * is not the one the vendor's code suggests:
 *
 *   as found        0x400E0000 = 0, 0x400E0008 = 0, CRU +0x118 = 0, and the
 *                   whole block reads as zeros and drops writes
 *   CRU +0x118      the write sticks (reads back 1) - and changes nothing
 *   0x400E0000      **bit 6 does not stick.** The register reads back 0.
 *                   15's negative is confirmed: it is dead from a payload.
 *   0x400E0008      likewise
 *   module id 46    **the block takes and holds writes.**
 *
 * So the front end is opened by a mask ROM module clock, exactly the way the
 * ADC turned out to be (15b), and 0x400E0000 is a red herring for a payload -
 * it is presumably how the vendor's own supervisor reaches the same gates
 * from code the boot ROM has not handed control away from.
 *
 * The module id is not in the dump for this or any peripheral. It was found
 * by walking 0..127 and testing the block after each, which is now twice the
 * method that has worked where reading the firmware did not.
 */
#define SL6806_DVP_MODULE_ID        46

/*
 * [V] The camera's module clock, CRU +0x118, from 0x00D44688 - called with
 * source 0 and divider 0 from the bind path immediately before the front end
 * is opened. Read, clear both fields, store, then set bit 0 in a second
 * store.
 *
 * Note the source field is [5:4], two bits: 15a records it as [7:4] from this
 * same routine, but the `bic #0x30` says otherwise. The divider at [11:8]
 * matches. Setting it is not sufficient on its own - the module id above is
 * what opens the block - but the vendor sets both and so should anything
 * expecting the block to actually clock pixels.
 */
#define SL6806_CRU_CAM_CLOCK        0x40080118u
#define SL6806_CRU_CAM_SRC_MASK     0x0030u
#define SL6806_CRU_CAM_DIV_MASK     0x0F00u
#define SL6806_CRU_CAM_ENABLE       0x0001u

/* ------------------------------------------------------------------ */
/* The clock tree the front end runs on - and MCLK with it             */
/* ------------------------------------------------------------------ */

/*
 * [V] `dvp_open` does not just set gates. Its first call, at 0x00D98232, is
 * to 0x00D9A7FC, and that routine builds a clock:
 *
 *     0x00D9A7AC()                  the request/ack walk below
 *     0x40080008 = 0xC0000C04       a PLL
 *     poll until bit 28             lock
 *     0x40080008 |= 0x10000         release it to the tree
 *     0x4008011C = 0x31             module clock, enabled, source 3
 *
 * The first hardware run of this framework's own bring-up skipped all of it
 * and enabled only the block and CRU +0x118 - which is why the block came up
 * awake, took every geometry write, and still produced no MCLK. Gates give a
 * peripheral its registers. They do not give it a clock.
 *
 * **0x40080008 is a PLL, and 7c said this unit had none.** "The clock and
 * reset unit at 0x40080000 holds dividers and gates, not a PLL multiplier" is
 * in the README as a caveat on the measured CPU clock. There is a multiplier
 * at +0x08 with a lock bit at 28, and the vendor programs it at camera-open
 * time on a running system - so it is not the core's, but it is a PLL and the
 * negative should not have been stated so broadly.
 */
#define SL6806_CRU_PLL              0x40080008u
#define SL6806_CRU_PLL_VALUE        0xC0000C04u /* [V] the vendor's word     */
#define SL6806_CRU_PLL_LOCKED       (1u << 28)  /* [V] polled after the write */
#define SL6806_CRU_PLL_RELEASE      (1u << 16)  /* [V] set once locked        */

/* [V] The media module clock: enabled, source 3, divider 0. 13b saw this
 * register holding 0x31 and read the fields; this is where it comes from. */
#define SL6806_CRU_MEDIA_CLOCK      0x4008011Cu
#define SL6806_CRU_MEDIA_VALUE      0x31u

/*
 * [V] 0x00D9A7AC, a request/acknowledge register in the pad/mux block:
 * ten request bits at [9:0] and their acknowledgements at [25:16]. For each
 * of the ten, if the ack is clear, set the request and spin until the ack
 * appears. Domains of some kind - power, clock or isolation - and the camera
 * path asks for all ten.
 */
#define SL6806_REQ_ACK              0x40000070u
#define SL6806_REQ_ACK_COUNT        10
#define SL6806_REQ_ACK_SHIFT        16          /* ack = request << 16 */

/* ------------------------------------------------------------------ */
/* [M] The output path - found by mapping, not by reading code         */
/* ------------------------------------------------------------------ */

/*
 * "Where do the frames go" was this file's open question, and no vendor
 * routine that has been read touches a destination address. Writing ones into
 * every word of the live block and reading back what stuck (examples/DvpProbe,
 * 2026-08-13) turned up four registers neither `configure` nor `open` writes:
 *
 *   +0x30   cold 0           writable 0x0000000B   bits 0, 1, 3
 *   +0x34   cold 0           writable 0x00FFFFFF   24 bits
 *   +0x38   cold 0x00800000  writable 0x000FFFFC   bits 2..19
 *   +0x3C   cold 0           writable 0x000FFFFF   20 bits
 *
 * **`+0x38` is a buffer pointer into SRAM.** Its cold value is `0x00800000`,
 * which is the SRAM base, and the bits that take writes are exactly [19:2] -
 * a 1 MB span at 4-byte alignment, with the `0x008` on top fixed. A register
 * whose resting value is the base of the only RAM on the chip, and which can
 * be moved anywhere within 1 MB but not outside it, is a DMA destination and
 * very little else.
 *
 * `+0x3C`'s twenty bits fit a length over the same span, `+0x34`'s
 * twenty-four a second address or a byte count, and `+0x30`'s three bits the
 * control that starts it. All [I] - the shapes fit and nothing has been run.
 *
 * THE BLOCK IS 0x80 BYTES AND ALIASES. `+0x80`-`+0xC0` read back exactly what
 * `+0x00`-`+0x40` hold, including values this framework had just written, so
 * the upper half is the same registers decoded twice rather than a second
 * channel. Worth knowing before anyone reads `+0x84` as a separate control.
 */
#define SL6806_DVP_DMA_CTRL         0x30    /* [I] bits 0, 1, 3            */
#define SL6806_DVP_DMA_LEN24        0x34    /* [I] 24 bits                 */
#define SL6806_DVP_DMA_ADDR         0x38    /* [M] SRAM pointer, [19:2]    */
#define SL6806_DVP_DMA_LEN20        0x3C    /* [I] 20 bits                 */
#define SL6806_DVP_ALIAS            0x80    /* [M] the block repeats here  */

/* [M] +0x38's writable span, from the DvpProbe census: the top nibble is
 * fixed at the SRAM base and only [19:2] takes writes - 1 MB at 4-byte
 * alignment. */
#define SL6806_DVP_SRAM_BASE        0x00800000u
#define SL6806_DVP_SRAM_SPAN        (1u << 20)
#define SL6806_DVP_DMA_ADDR_MASK    0x000FFFFCu

/* [I] +0x3C's field, sized to match +0x38's 20-bit span - the best-supported
 * reading of "a length over the same span". +0x34 is left alone by the
 * driver below: the census cannot tell a byte count from a second address,
 * and writing a length into what might be an address is a worse guess than
 * writing nothing. */
#define SL6806_DVP_DMA_LEN_MASK     0x000FFFFFu

/* [I] The three writable bits of +0x30, all set together until one is shown
 * unnecessary - there is no vendor sequence to transcribe here, since
 * nothing in the dump ever writes these four registers (see above). */
#define SL6806_DVP_DMA_CTRL_MASK    0x0Bu
#define SL6806_DVP_DMA_CTRL_START   SL6806_DVP_DMA_CTRL_MASK

#ifdef __cplusplus
extern "C" {
#endif

/*
 * sl6806_dvp_capture_start() - point the +0x30..0x3C cluster at a buffer
 * and set every writable CTRL bit.
 *
 * This is this driver's own convention, not a transcription: address, then
 * length, then start, matching the order every other DMA-shaped block in
 * this tree uses (see sl6806_audio.c), because nothing in the vendor's own
 * code ever touches these four registers to copy.
 *
 * dst must be a word-aligned address inside the chip's 1 MB SRAM span, and
 * len must be a nonzero multiple of 4 that fits the 20-bit length field.
 * Returns 0 on a syntactically valid request, -1 on a rejected one - neither
 * says whether the hardware moves a single byte, which no register in this
 * cluster reports (see sl6806_dvp_capture_writable()).
 */
int sl6806_dvp_capture_start(void *dst, uint32_t len);

/* Clears the CTRL bits this driver sets. Does not touch ADDR or the length
 * registers, so a stopped descriptor can be re-armed with the same
 * geometry by calling sl6806_dvp_capture_start() again. */
void sl6806_dvp_capture_stop(void);

/*
 * The write-and-read-back proof, same discipline as
 * sl6806_audio_writable(): a gated block reads zero and drops every write,
 * so a plausible-looking register proves nothing on its own. This writes a
 * distinguishing test pattern into ADDR and the length field, reads it back,
 * and restores whatever sl6806_dvp_capture_start() (or the cold reset value)
 * held before returning. 1 if the descriptor registers hold what they are
 * given, 0 if the block is gated or otherwise drops the write.
 */
int sl6806_dvp_capture_writable(void);

/* Read back what the descriptor registers currently hold, decoded to plain
 * values rather than the raw fixed-base/masked-field encoding. */
void *sl6806_dvp_capture_addr(void);
uint32_t sl6806_dvp_capture_len(void);

#ifdef __cplusplus
}
#endif

/* ------------------------------------------------------------------ */
/* [V] What the SENSOR's own init enables, which the DVP path does not  */
/* ------------------------------------------------------------------ */

/*
 * The camera bring-up is in two halves and this project had only read one of
 * them. `bind_dvp_channel` (0x00D44740) sets up the front end; **`sensor_init`
 * at 0x00D44CBC sets up two more clocks before it ever touches the bus**, and
 * neither is anywhere in the DVP path:
 *
 *     0x00D44CD2   module_clock_enable(80)      via ROM 0x1EC0
 *     0x00D44CD8   rom_clock_enable(58)         via SRAM veneer 0x008051EC
 *     ...          then the TWI0 pads, twi_init(0, 100k), power_on, chip id
 *
 * **ROM 0x20EC is a clock dispatcher**, and this is what it was hiding: an
 * 85-entry `tbh` table where each id maps to one CRU register and one bit.
 * Entry 58 is `0x400800C0 |= 1`. Its neighbours set +0xC4, +0xC8 and +0x104
 * the same way, so this is a whole second family of clock enables sitting
 * beside the module gates - and CRU +0xC0 has never been written by anything
 * in this repository.
 *
 * That makes these the only two things the vendor does for the camera that a
 * payload has not, after the rail, the register clock, the functional clock,
 * the PLL, the media clock, the camera module clock and the full geometry all
 * came up correctly and the sensor stayed mute with its outputs in Hi-Z.
 *
 * [I] as to what either one clocks. Module 80 is unidentified; CRU +0xC0
 * bit 0 is unidentified. What is [V] is that the sensor's own driver enables
 * both before it expects the part to answer.
 */
#define SL6806_SENSOR_MODULE_ID     80
#define SL6806_CRU_SENSOR_CLOCK     0x400800C0u
#define SL6806_CRU_SENSOR_CLOCK_EN  0x1u

/*
 * STILL NOT DECODED: MCLK. With registers, functional clock, PLL, media
 * clock, camera module clock and a full geometry configuration in place, the
 * sensor gets no clock and does not answer. Nothing in the mapped block looks
 * like a clock divider - the four registers above are an output path, not an
 * input one - so either MCLK is owned by something outside this block, or it
 * only runs once the block is actually streaming.
 */

#endif /* SL6806_DVP_H */
