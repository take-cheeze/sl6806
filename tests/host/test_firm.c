/*
 * test_firm.c - the FIRM header parse, which is the part that can be tested.
 *
 * The jump itself cannot be: it ends in the application running and the USB
 * link gone. What *can* be checked is everything that decides whether to
 * jump, and that is where the damage would be - a loader that computes the
 * load address wrongly copies the image 0x400 bytes off and branches into the
 * middle of a function, which on a Cortex-M is a hard fault with no console
 * left to report it.
 *
 * So the tests are all refusals, plus one acceptance of the real header:
 *
 *   1. **The load address is derived, then checked against the image.** The
 *      header names an entry; the load address is that minus the vector
 *      table; and the segment's own word 1 must agree that the entry is the
 *      reset vector. Two independent facts that have to line up.
 *   2. **A non-Thumb entry is refused.** Branching to an even address faults
 *      immediately on this core, and the header is the cheapest place to
 *      notice.
 *   3. **An absurd length is refused**, because a corrupt header is more
 *      likely than a 200 KB application.
 *   4. **The real header parses**, with the values notes §6 and §7m record.
 */

#include <stdio.h>
#include <string.h>

#define SL6806_FIRM_HOSTED 1
#include "sl6806_firm.h"

static int failures, checks;

#define CHECK(cond, what) do {                                          \
        checks++;                                                       \
        if (!(cond)) {                                                  \
            printf("  FAIL %s (%s:%d)\n", (what), __FILE__, __LINE__);  \
            failures++;                                                 \
        }                                                               \
    } while (0)

static uint8_t hdr[0x30];
static uint8_t vec[8];

static void put32(uint8_t *p, unsigned off, uint32_t v)
{
    p[off] = (uint8_t)v;
    p[off + 1] = (uint8_t)(v >> 8);
    p[off + 2] = (uint8_t)(v >> 16);
    p[off + 3] = (uint8_t)(v >> 24);
}

/* The header and vectors the shipped image actually carries, from
 * dump.bin at file 0x10000 and 0x10030. */
static void real_image(void)
{
    memset(hdr, 0, sizeof(hdr));
    memset(vec, 0, sizeof(vec));

    put32(hdr, 0x00, 0x00000030u);      /* header length */
    put32(hdr, 0x04, 0x8F4E4434u);      /* timestamp     */
    put32(hdr, 0x10, 0x00804C00u);
    put32(hdr, 0x14, 0x00804C01u);      /* entry, Thumb  */
    put32(hdr, 0x18, 0x00001000u);
    put32(hdr, 0x1C, 0x00005862u);      /* length        */

    put32(vec, 0, 0x0082D63Cu);         /* initial SP    */
    put32(vec, 4, 0x00804C01u);         /* reset vector  */
}

static void test_real(void)
{
    sl6806_firm_t f;

    printf("the shipped image\n");
    real_image();

    CHECK(sl6806_firm_parse(hdr, vec, &f) == SL6806_FIRM_OK, "it parses");
    CHECK(f.entry == 0x00804C01u, "the entry is the header's, Thumb bit and all");
    CHECK(f.load == 0x00804800u,
          "the load address is the entry less the vector table - §7m's value");
    CHECK(f.length == 0x5862u, "the length is the header's");
    CHECK(f.stack == 0x0082D63Cu, "the stack comes from vector 0");
    CHECK(f.timestamp == 0x8F4E4434u, "and the build stamp is carried");
}

static void test_refusals(void)
{
    sl6806_firm_t f;

    printf("headers it must refuse\n");

    /* Not a header at all. */
    real_image();
    put32(hdr, 0x00, 0x00000040u);
    CHECK(sl6806_firm_parse(hdr, vec, &f) == SL6806_FIRM_ERR_HDR,
          "a wrong header length");

    /* An entry with the Thumb bit clear: an immediate fault if taken. */
    real_image();
    put32(hdr, 0x14, 0x00804C00u);
    CHECK(sl6806_firm_parse(hdr, vec, &f) == SL6806_FIRM_ERR_ENTRY,
          "an entry that is not Thumb");

    /* An entry outside SRAM. */
    real_image();
    put32(hdr, 0x14, 0x00C10001u);
    CHECK(sl6806_firm_parse(hdr, vec, &f) == SL6806_FIRM_ERR_ENTRY,
          "an entry in flash rather than SRAM");

    real_image();
    put32(hdr, 0x1C, 0u);
    CHECK(sl6806_firm_parse(hdr, vec, &f) == SL6806_FIRM_ERR_LENGTH,
          "a zero length");

    real_image();
    put32(hdr, 0x1C, 0x00100000u);
    CHECK(sl6806_firm_parse(hdr, vec, &f) == SL6806_FIRM_ERR_LENGTH,
          "a length bigger than SRAM");

    /*
     * The important one. If the segment does not begin with a vector table
     * whose reset entry matches the header, the load address derivation is
     * wrong and the copy would land 0x400 bytes off.
     */
    real_image();
    put32(vec, 4, 0x00805000u);
    CHECK(sl6806_firm_parse(hdr, vec, &f) == SL6806_FIRM_ERR_VECTORS,
          "a reset vector that disagrees with the header's entry");

    real_image();
    put32(vec, 0, 0x00000000u);
    CHECK(sl6806_firm_parse(hdr, vec, &f) == SL6806_FIRM_ERR_VECTORS,
          "a stack pointer that is not in SRAM");

    real_image();
    put32(vec, 0, 0x20000000u);
    CHECK(sl6806_firm_parse(hdr, vec, &f) == SL6806_FIRM_ERR_VECTORS,
          "a stack pointer from some other chip's memory map");

    CHECK(sl6806_firm_parse(NULL, vec, &f) == SL6806_FIRM_ERR_HDR,
          "and a null header");
}

int main(void)
{
    printf("test_firm\n");

    test_real();
    test_refusals();

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
