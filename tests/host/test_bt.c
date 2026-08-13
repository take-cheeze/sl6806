/*
 * test_bt.c - native tests for the gate in front of 0x400E2000.
 *
 * sl6806_bt_begin() is a transcription of 0x00D9A7FC plus the first four
 * instructions of the module registration at 0x00D98C9C. It has never run.
 * What these tests hold it to is the ORDER, because the order is the whole
 * content of the function and every step of it is easy to lose:
 *
 *   1. **The power handshake comes before the PLL.** 0x00D9A7AC runs first,
 *      and it is at 0x40000070 - the pad-mux base, not the CRU - which makes
 *      it look like it belongs to something else entirely.
 *
 *   2. **The PLL locks before the module clock.** Enabling module 46 while
 *      the PLL is still settling is the difference between a block that
 *      answers and one that reads zero, which is the state notes 14a and 15
 *      recorded for two whole sessions.
 *
 *   3. **The group's functional bit before this block's.** 0x400E0000 bit 5
 *      is the shared one and bit 0 is Bluetooth's; the vendor takes them in
 *      that order.
 *
 *   4. **The reset is a different register.** 0x400E0008, not 0x400E0000 -
 *      the mistake sl6806_module.h says nothing in this tree noticed until
 *      the camera front end was decoded.
 *
 *   5. **The once-guard.** If module 46 is already up the whole sequence is
 *      skipped, because the vendor skips it. Re-running a PLL start on a
 *      chip that is already using that PLL is not obviously harmless.
 *
 *   6. **Every poll is bounded.** In payload mode this runs inside the boot
 *      ROM's USB handler, and the PLL's lock bit is *known* not to set in
 *      bootloader mode - so an unbounded spin here is the expected outcome,
 *      not a hypothetical one.
 */

#include <stdio.h>
#include <string.h>

#include "sl6806_bt.h"
#include "sl6806_module.h"
#include "sl6806_pwm.h"

static int failures, checks;

#define CHECK(cond, what) do {                                          \
        checks++;                                                       \
        if (!(cond)) {                                                  \
            printf("  FAIL %s (%s:%d)\n", (what), __FILE__, __LINE__);  \
            failures++;                                                 \
        }                                                               \
    } while (0)

/* ------------------------------------------------------------------ */
/* A model of the CRU, the power handshake and the 0x400E0000 gate     */
/* ------------------------------------------------------------------ */

#define CRU_BASE    0x40080000u

static uint32_t cru[0x80];
static uint32_t pwr, pwr_release;
static uint32_t periph_en, periph_rst;

static int pll_locks;       /* does the PLL ever report lock?           */
static int pwr_answers;     /* does the handshake ever acknowledge?     */
static int gate_holds;      /* does 0x400E0000 keep the bits written?   */

static char trace[1024];

static void note(const char *s)
{
    if (strlen(trace) + strlen(s) + 2 < sizeof(trace)) {
        strcat(trace, s);
        strcat(trace, " ");
    }
}

static int before(const char *a, const char *b)
{
    const char *pa = strstr(trace, a);
    const char *pb = strstr(trace, b);

    return pa && pb && pa < pb;
}

uint32_t sl6806_mmio_read(uint32_t a)
{
    if (a >= CRU_BASE && a < CRU_BASE + sizeof(cru))
        return cru[(a - CRU_BASE) / 4];
    if (a == SL6806_BT_PWR_REQ)          return pwr;
    if (a == SL6806_BT_PWR_RELEASE)      return pwr_release;
    if (a == SL6806_PERIPH_ENABLE_REG)   return periph_en;
    if (a == SL6806_PERIPH_RESET_REG)    return periph_rst;
    return 0;
}

void sl6806_mmio_write(uint32_t a, uint32_t v)
{
    char buf[32];

    if (a >= CRU_BASE && a < CRU_BASE + sizeof(cru)) {
        snprintf(buf, sizeof(buf), "C%03X", a - CRU_BASE);
        note(buf);
        cru[(a - CRU_BASE) / 4] = v;
        /* The PLL reports lock as soon as it is configured, if it ever
         * does. Bit 28 is read-only from the driver's side. */
        if (a == SL6806_PLL_CTRL && pll_locks)
            cru[(a - CRU_BASE) / 4] |= SL6806_PLL_LOCK;
        return;
    }
    if (a == SL6806_BT_PWR_REQ) {
        note("PWR");
        pwr = v;
        /* Acknowledge every request that came in, in [25:16]. */
        if (pwr_answers)
            pwr |= (v & 0x3FFu) << SL6806_BT_PWR_ACK_SHIFT;
        return;
    }
    if (a == SL6806_BT_PWR_RELEASE)    { note("REL"); pwr_release = v; return; }
    if (a == SL6806_PERIPH_ENABLE_REG) {
        snprintf(buf, sizeof(buf), "EN%08X", v);
        note(buf);
        periph_en = gate_holds ? v : 0;
        return;
    }
    if (a == SL6806_PERIPH_RESET_REG)  {
        snprintf(buf, sizeof(buf), "RST%08X", v);
        note(buf);
        periph_rst = v;
        return;
    }
}

void delay(uint32_t ms) { (void)ms; }

static void reset_model(void)
{
    memset(cru, 0, sizeof(cru));
    pwr = pwr_release = periph_en = periph_rst = 0;
    pll_locks = pwr_answers = gate_holds = 1;
    trace[0] = '\0';
}

/* ------------------------------------------------------------------ */

static void test_sequence(void)
{
    printf("the gate sequence\n");

    reset_model();
    CHECK(sl6806_bt_begin() == 1, "begin() succeeds when the gate holds");

    CHECK(before("PWR", "C008"), "the power handshake precedes the PLL");
    CHECK(before("C008", "C074"), "the PLL precedes module 46's shadow");
    CHECK(before("C074", "C064"), "shadow before gate - see sl6806_module.h");
    CHECK(before("C064", "C11C"), "the module clock precedes 0x4008011C");
    CHECK(before("C11C", "EN"),   "and 0x4008011C precedes 0x400E0000");

    CHECK(cru[0x08 / 4] == (SL6806_PLL_CONFIG | SL6806_PLL_LOCK
                            | SL6806_PLL_OUT_ENABLE),
          "the PLL got the vendor's word and then its output enable");
    CHECK(cru[0x11C / 4] == SL6806_CRU_CLK_ON, "0x4008011C = 0x31");

    /* Module 46 is bank 1: gate +0x64, shadow +0x74, bit 46 - 32 = 14. */
    CHECK(cru[0x64 / 4] == (1u << 14), "module 46 is bank 1 bit 14");
    CHECK(cru[0x74 / 4] == (1u << 14), "in both registers");

    CHECK(periph_en & (1u << SL6806_BT_GROUP_PERIPH_ID), "0x400E0000 bit 5");
    CHECK(periph_en & (1u << SL6806_BT_PERIPH_ID),       "0x400E0000 bit 0");
    CHECK(before("EN00000020", "EN00000021"),
          "bit 5 is taken before bit 0, and neither replaces the other");

    /* The reset is a pulse on the OTHER register, low then high. */
    CHECK(before("RST00000000", "RST00000001"),
          "0x400E0008 bit 0 is pulsed low then high");
    CHECK(periph_rst & (1u << SL6806_BT_RESET_ID), "and left released");
}

static void test_handshake(void)
{
    printf("the power handshake\n");

    reset_model();
    sl6806_bt_begin();
    CHECK((pwr & 0x3FFu) == 0x3FFu, "all ten domains requested");
    CHECK(pwr_release == 0, "0x40000074 is cleared at the end");

    /* A domain already acknowledged is skipped, not re-requested - the
     * vendor's own `if`, and the thing that makes a second call cheap. */
    reset_model();
    pwr = 0x000F0000u;              /* domains 0..3 already up */
    sl6806_bt_begin();
    CHECK((pwr & 0xFu) == 0, "an acknowledged domain is not re-requested");
    CHECK((pwr & 0x3F0u) == 0x3F0u, "and the rest still are");
}

static void test_bounded(void)
{
    printf("bounded polls\n");

    /* Each of these hangs the device if the poll is unbounded, and the first
     * one is not hypothetical: 14a read the PLL stopped in bootloader mode. */
    reset_model();
    pll_locks = 0;
    CHECK(sl6806_bt_begin() == 1, "a PLL that never locks does not hang");
    CHECK((cru[0x08 / 4] & SL6806_PLL_OUT_ENABLE) == 0,
          "and its output enable is not set on a lock that never came");

    reset_model();
    pwr_answers = 0;
    CHECK(sl6806_bt_begin() == 1, "a domain that never answers does not hang");

    reset_model();
    gate_holds = 0;
    CHECK(sl6806_bt_begin() == 0, "a gate that drops its bits is reported");
}

static void test_guard(void)
{
    printf("the once-guard\n");

    reset_model();
    sl6806_bt_begin();
    CHECK(sl6806_module_enabled(SL6806_BT_GROUP_MODULE_ID),
          "module 46 is up after the first call");

    trace[0] = '\0';
    cru[0x08 / 4] = 0;
    CHECK(sl6806_bt_begin() == 1, "a second call still succeeds");
    CHECK(cru[0x08 / 4] == 0, "but does not restart the PLL");
    CHECK(strstr(trace, "PWR") == NULL, "or redo the power handshake");
    CHECK(strstr(trace, "EN") != NULL, "while still taking the block's bit");
}

static void test_counters(void)
{
    printf("the counters\n");

    /* sl6806_bt_clocks() reads real MMIO, which the host cannot do, so all
     * this can check is the masking - which is the part that matters, since
     * +0x22C carries two control bits above its 30-bit counter and reporting
     * those as part of the value would make a still block look busy. */
    CHECK(SL6806_BT_CLK0_MASK == 0x01FFFFFFu, "CLK0 is 25 bits");
    CHECK(SL6806_BT_CLK_MASK == 0x3FFFFFFFu, "the others are 30");
    CHECK((SL6806_BT_CLK_MASK & SL6806_BT_REQ_BIT) == 0,
          "the request bit is masked out of the counter");
    CHECK((SL6806_BT_CLK_MASK & SL6806_BT_GRANT_BIT) == 0,
          "and so is the grant bit");
}

int main(void)
{
    printf("bluetooth gate tests\n");

    test_sequence();
    test_handshake();
    test_bounded();
    test_guard();
    test_counters();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
