/*
 * host_stub.h - lets core sources compile natively for testing.
 *
 * Parts of the core are pure logic - ring arithmetic, overflow accounting,
 * drawing primitives - and that logic is worth testing without a device
 * attached. The obstacle is cores/sl6806/sl6806.h, which is full of Cortex-M
 * inline assembly that a host compiler cannot assemble.
 *
 * A stub file named sl6806.h would not work: a quoted #include resolves
 * against the including file's own directory first, so the real header always
 * wins. Instead this is force-included with -include, claims the real
 * header's include guard so its body is skipped entirely, and supplies host
 * equivalents of the few things the tested code actually uses.
 *
 * No core source needs a single #ifdef for testing.
 */
#ifndef SL6806_HOST_STUB_H
#define SL6806_HOST_STUB_H

/* Claim the real header's guard so including it becomes a no-op. */
#define SL6806_H

#include <stdint.h>

/* Interrupt masking is meaningless in a single-threaded host test, but the
 * calls must still exist and balance. */
static inline uint32_t sl6806_irq_save(void)          { return 0; }
static inline void     sl6806_irq_restore(uint32_t p) { (void)p; }
static inline void     sl6806_dsb(void)               { }
static inline void     sl6806_isb(void)               { }

#endif /* SL6806_HOST_STUB_H */
