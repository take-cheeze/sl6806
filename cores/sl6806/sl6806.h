/*
 * sl6806.h - SoC definition for the Zhuhai Smartlink SL6806 ("spark2").
 *
 * Provenance of every constant in this file is marked:
 *   [V] verified   - confirmed against a real 4 MiB flash dump or against the
 *                    smartlink_flash tool source (3rd/smartlink_flash).
 *   [A] arch       - defined by the ARMv7E-M architecture, not by the vendor.
 *                    True for any Cortex-M4, so safe to rely on.
 *   [I] inferred   - reasoned from evidence but not directly proven.
 *   [?] unknown    - NOT established. Do not trust without measuring.
 *
 * Core: ARM Cortex-M4F, ARMv7E-M, Thumb-2, single-precision FPU.        [V]
 */
#ifndef SL6806_H
#define SL6806_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Address map                                                         */
/* ------------------------------------------------------------------ */

/* Mask ROM: boot ROM + USB download mode. Survives a bad flash, which is
 * what makes this chip recoverable. Size is the SL6806 figure from the
 * smartlink_flash payload README. */
#define SL6806_ROM_BASE        0x00000000u  /* [V] */
#define SL6806_ROM_SIZE        0x0007D000u  /* [V] */

/* SRAM. 256 KiB. The application's initial stack pointer (0x0082D63C)
 * and its load address (0x00804C00) both fall inside this window. */
#define SL6806_SRAM_BASE       0x00800000u  /* [V] */
#define SL6806_SRAM_SIZE       0x00040000u  /* [V] */
#define SL6806_SRAM_END        (SL6806_SRAM_BASE + SL6806_SRAM_SIZE)

/* SPI flash, execute-in-place. File offset O in a dump maps to CPU
 * address SL6806_FLASH_BASE + O. Confirmed by resolving 11725/13114
 * string pointers at this base. */
#define SL6806_FLASH_BASE      0x00C00000u  /* [V] */
#define SL6806_FLASH_SIZE      0x00400000u  /* [V] 4 MiB */

/* Scratch buffer the boot ROM uses for its own RAM-exec staging. The
 * stock tool writes a 0x100-byte trampoline here. Below the main SRAM
 * window, so this is a separate internal RAM/TCM block. Treat as
 * reserved: the ROM clobbers it on every write_mem/read_mem. */
#define SL6806_ROM_SCRATCH     0x003FB000u  /* [V] */
#define SL6806_ROM_TRAMPOLINE  0x00800E81u  /* [V] thumb entry used with it */

/* Peripheral MMIO region. NOT established for this SoC - see hal_gpio.h.
 * 0x40000000 is only the ARM-recommended peripheral window, and this SoC
 * already ignores the recommended code/SRAM windows, so do not assume it. */
/* #define SL6806_PERIPH_BASE  [?] unknown */

/* ------------------------------------------------------------------ */
/* Flash layout (partition table lives at flash offset 0x0000F000)      */
/* ------------------------------------------------------------------ */

#define SL6806_PART_TABLE_OFF  0x0000F000u  /* [V] */

#define SL6806_BOOT_OFF        0x00000000u  /* [V] HLKJ first-stage loader */
#define SL6806_FIRM_OFF        0x00010000u  /* [V] application */
#define SL6806_FIRM_SIZE       0x001B80D0u  /* [V] */
#define SL6806_FIRM_ADDR       0x00C10000u  /* [V] XIP run address */
#define SL6806_PICS_OFF        0x001DB000u  /* [V] UI bitmaps */
#define SL6806_PICS_SIZE       0x000E447Cu  /* [V] */
#define SL6806_PICS_ADDR       0x00DDB000u  /* [V] */
#define SL6806_FONT_OFF        0x002C0000u  /* [V] glyph data */
#define SL6806_FONT_ADDR       0x00EC0000u  /* [V] */

/* ------------------------------------------------------------------ */
/* Cortex-M4 core peripherals - architectural, identical on every M4 [A] */
/* ------------------------------------------------------------------ */

#define SL6806_SCS_BASE        0xE000E000u
#define SL6806_SYSTICK_BASE    (SL6806_SCS_BASE + 0x0010u)
#define SL6806_NVIC_BASE       (SL6806_SCS_BASE + 0x0100u)
#define SL6806_SCB_BASE        (SL6806_SCS_BASE + 0x0D00u)
#define SL6806_DWT_BASE        0xE0001000u
#define SL6806_COREDEBUG_BASE  0xE000EDF0u

typedef struct {
    volatile uint32_t CTRL;
    volatile uint32_t LOAD;
    volatile uint32_t VAL;
    volatile const uint32_t CALIB;
} SysTick_Type;

typedef struct {
    volatile uint32_t ISER[8];
    uint32_t  _r0[24];
    volatile uint32_t ICER[8];
    uint32_t  _r1[24];
    volatile uint32_t ISPR[8];
    uint32_t  _r2[24];
    volatile uint32_t ICPR[8];
    uint32_t  _r3[24];
    volatile uint32_t IABR[8];
    uint32_t  _r4[56];
    volatile uint8_t  IP[240];
} NVIC_Type;

typedef struct {
    volatile const uint32_t CPUID;
    volatile uint32_t ICSR;
    volatile uint32_t VTOR;
    volatile uint32_t AIRCR;
    volatile uint32_t SCR;
    volatile uint32_t CCR;
    volatile uint8_t  SHP[12];
    volatile uint32_t SHCSR;
    volatile uint32_t CFSR;
    volatile uint32_t HFSR;
    volatile uint32_t DFSR;
    volatile uint32_t MMFAR;
    volatile uint32_t BFAR;
    volatile uint32_t AFSR;
} SCB_Type;

typedef struct {
    volatile uint32_t CTRL;
    volatile uint32_t CYCCNT;
} DWT_Type;

#define SysTick ((SysTick_Type *)SL6806_SYSTICK_BASE)
#define NVIC    ((NVIC_Type    *)SL6806_NVIC_BASE)
#define SCB     ((SCB_Type     *)SL6806_SCB_BASE)
#define DWT     ((DWT_Type     *)SL6806_DWT_BASE)

#define SYSTICK_CTRL_ENABLE     (1u << 0)
#define SYSTICK_CTRL_TICKINT    (1u << 1)
#define SYSTICK_CTRL_CLKSOURCE  (1u << 2)
#define SYSTICK_CTRL_COUNTFLAG  (1u << 16)

#define DWT_CTRL_CYCCNTENA      (1u << 0)
#define COREDEBUG_DEMCR_TRCENA  (1u << 24)

/* ------------------------------------------------------------------ */
/* Core clock                                                          */
/* ------------------------------------------------------------------ */

/*
 * [?] The CPU clock frequency is NOT known. Nothing in the dump or in the
 * dumper source establishes it, and the PLL registers have not been found.
 *
 * F_CPU below is a placeholder. Every millis()/micros()/delay() result is
 * wrong by exactly the ratio of this value to the real clock. It is a pure
 * scale factor, so one measurement fixes all of them:
 *
 *   1. flash and run examples/ClockCalibrate
 *   2. it reports the measured tick rate against a wall clock
 *   3. rebuild with -DF_CPU=<measured>
 *
 * Until you do that, treat all timing as relative, not absolute.
 */
#ifndef F_CPU
#define F_CPU 120000000UL  /* [?] PLACEHOLDER - calibrate before trusting */
#define SL6806_F_CPU_IS_A_GUESS 1
#endif

/* ------------------------------------------------------------------ */
/* Core intrinsics                                                     */
/* ------------------------------------------------------------------ */

static inline void sl6806_enable_irq(void)  { __asm__ volatile ("cpsie i" ::: "memory"); }
static inline void sl6806_disable_irq(void) { __asm__ volatile ("cpsid i" ::: "memory"); }
static inline void sl6806_dsb(void)         { __asm__ volatile ("dsb" ::: "memory"); }
static inline void sl6806_isb(void)         { __asm__ volatile ("isb" ::: "memory"); }
static inline void sl6806_wfi(void)         { __asm__ volatile ("wfi"); }
static inline void sl6806_nop(void)         { __asm__ volatile ("nop"); }

static inline uint32_t sl6806_irq_save(void) {
    uint32_t pm;
    __asm__ volatile ("mrs %0, primask" : "=r"(pm));
    __asm__ volatile ("cpsid i" ::: "memory");
    return pm;
}

static inline void sl6806_irq_restore(uint32_t pm) {
    if (!(pm & 1u)) __asm__ volatile ("cpsie i" ::: "memory");
}

#ifdef __cplusplus
}
#endif

#endif /* SL6806_H */
