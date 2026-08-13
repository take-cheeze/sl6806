/*
 * test_sd.c - native tests for the SD/MMC driver.
 *
 * Nothing in sl6806_sd.c has run on hardware. That makes these tests the same
 * kind of thing as test_regfile.c's: they cannot tell you the decode of
 * 0x40003000 is right, only that the code performs the sequence the mask ROM
 * performs. Everything checked below is something a plausible "tidy-up" would
 * break silently on a device nobody has:
 *
 *   1. The command words. Seventeen 16-bit constants transcribed out of ROM
 *      0x40F4, and they are not `0x2540 | index`: CMD0 is 0xA400, CMD2 and
 *      CMD9 carry the long-response bit, CMD12 swaps bit 13 for bit 14, and
 *      the read commands clear bit 10 where the write commands set it. A
 *      table this irregular is exactly the kind that gets "simplified".
 *
 *   2. Argument before command, and the start bit set in the same word the
 *      controller is given. ROM 0x40D2 writes ARG, waits out any command
 *      still in flight, then stores the word with bit 31 already in it - one
 *      store, unlike the register file's mailbox next door, which needs two.
 *
 *   3. The identification order, including that CMD7 comes before the
 *      run-speed clock change. Selecting the card and then raising the clock
 *      is the ROM's order; the other way round changes the bus rate while the
 *      card is still deselected.
 *
 *   4. The two CLKCR words, 0x20070008 to identify and 0x1003000C to run.
 *      Divider 7 then 3 - and if someone decides the identification value
 *      "looks slow" and drops it, a card that needs 400 kHz never answers.
 *
 *   5. Eight words per acknowledgement in the FIFO drain, and the
 *      acknowledgement itself. The status bit says eight words are there; the
 *      write that clears it is what asks for the next eight. A one-word loop
 *      would be a different conversation with the block.
 *
 *   6. Byte addressing for everything except a high-capacity card. Getting
 *      this backwards reads sector 0 for every sector on an SDHC, or reads
 *      wildly off the end of an SDSC.
 *
 *   7. Every poll is bounded. A controller that never answers must produce a
 *      timeout, because in payload mode an unbounded spin is inside the boot
 *      ROM's USB handler and takes the device off the bus.
 */

#include <stdio.h>
#include <string.h>

#include "sl6806_sd.h"
#include "sl6806_module.h"
#include "sl6806_padctl.h"

/* The driver's waits are in bounded loops, not wall-clock, so the host does
 * not have to sleep for these to mean anything. */
void delay(uint32_t ms) { (void)ms; }
void delayMicroseconds(uint32_t us) { (void)us; }

/* Frozen, which is the case wiring_time.h warns about and the one that must
 * still leave the ACMD41 loop bounded by its iteration count. */
uint32_t millis(void) { return 0; }

static int failures, checks;

#define CHECK(cond, what) do {                                          \
        checks++;                                                       \
        if (!(cond)) {                                                  \
            printf("  FAIL %s (%s:%d)\n", (what), __FILE__, __LINE__);  \
            failures++;                                                 \
        }                                                               \
    } while (0)

/* ------------------------------------------------------------------ */
/* A model of the controller, the gates in front of it, and a card      */
/* ------------------------------------------------------------------ */

#define CRU         0x40080000u
#define ROMCLK_REG  SL6806_SD_ROMCLK_REG

/* Generic backing store, so pad writes and anything unmodelled just land
 * somewhere instead of faulting. */
#define SLOTS 512
static struct { uint32_t addr, val; } slot[SLOTS];
static unsigned n_slots;

static uint32_t *cell(uint32_t addr)
{
    unsigned i;

    for (i = 0; i < n_slots; i++)
        if (slot[i].addr == addr)
            return &slot[i].val;
    if (n_slots == SLOTS) {
        printf("  model out of slots\n");
        return &slot[0].val;
    }
    slot[n_slots].addr = addr;
    slot[n_slots].val = 0;
    return &slot[n_slots++].val;
}

/* --- the module gate pair, same discipline as test_adc ---------------- */

static uint32_t mod_shadow, mod_gate;   /* ids 32..63, CRU +0x74 and +0x64 */

/* --- the card ---------------------------------------------------------- */

static struct {
    int      present;
    int      v2;            /* answers CMD8 */
    int      hc;            /* sets CCS in the OCR */
    uint32_t cid[4], csd[4];
    uint16_t rca;
    int      acmd41_busy;   /* how many ACMD41s report "still powering up" */
    int      dead;          /* never raises the command-done bit */
} card;

/* --- the controller ---------------------------------------------------- */

static uint32_t ctrl, cmdreg, argreg, blksize, dlen, dtimer, dctrl;
static uint32_t sta, sta2, clkcr, resp[4];
static unsigned fifo_words;
static uint32_t data_left;      /* words still to come after the FIFO */
static uint32_t data_seed;

/* What the model saw, so order can be asserted. */
static char trace[512];
static unsigned n_clkcr;
static uint32_t clkcr_seen[8];
static uint32_t last_arg_before_cmd;
static int      arg_written_first;
static int      dataen_before_dlen;
static unsigned rxhalf_acks;
static int      reset_waited;
static uint32_t acmd41_arg;

static void note(const char *fmt, unsigned v)
{
    char buf[32];

    snprintf(buf, sizeof(buf), fmt, v);
    if (strlen(trace) + strlen(buf) + 2 < sizeof(trace)) {
        strcat(trace, buf);
        strcat(trace, " ");
    }
}

static void model_reset(void)
{
    n_slots = 0;
    mod_shadow = mod_gate = 0;
    ctrl = cmdreg = argreg = blksize = dlen = dtimer = dctrl = 0;
    sta = sta2 = clkcr = 0;
    memset(resp, 0, sizeof(resp));
    fifo_words = 0;
    data_left = 0;
    data_seed = 0;
    trace[0] = 0;
    n_clkcr = 0;
    last_arg_before_cmd = 0xDEADBEEFu;
    arg_written_first = 1;
    dataen_before_dlen = 0;
    rxhalf_acks = 0;
    reset_waited = 0;
    acmd41_arg = 0;
}

/* The pattern the modelled card returns, so a drain can be checked word for
 * word rather than only for length. */
static uint32_t block_word(uint32_t lba, unsigned i)
{
    return (lba * 0x01000193u) ^ (i * 0x9E3779B9u);
}

static void execute(uint32_t word)
{
    unsigned index = word & 0x3Fu;
    static int acmd = 0;

    note("%u", index);

    if (card.dead)
        return;                 /* no command-done bit, ever */

    if (!card.present) {
        /* An empty slot: the command goes out and nothing answers. CMD0
         * expects no response, so it still completes. */
        if (index == 0u)
            sta |= SL6806_SD_STA_CMDDONE;
        else
            sta |= SL6806_SD_STA_CMDDONE | SL6806_SD_STA_CMDTMOUT;
        return;
    }

    sta |= SL6806_SD_STA_CMDDONE;
    sta2 = (sta2 & ~(0x3Fu << 11)) | (index << 11);
    memset(resp, 0, sizeof(resp));

    switch (index) {
    case 0:
        acmd = 0;
        break;
    case 8:
        if (!card.v2)
            sta |= SL6806_SD_STA_CMDTMOUT;
        else
            resp[0] = argreg;           /* R7 echoes the check pattern */
        break;
    case 55:
        acmd = 1;
        resp[0] = 0x00000120u;          /* APP_CMD, ready for data */
        break;
    case 41:
        acmd41_arg = argreg;
        if (card.acmd41_busy > 0) {
            card.acmd41_busy--;
            resp[0] = 0x00FF8000u;      /* powering up: bit 31 clear */
        } else {
            resp[0] = 0x80FF8000u | (card.hc ? 0x40000000u : 0u);
        }
        acmd = 0;
        break;
    case 2:
        resp[3] = card.cid[0];
        resp[2] = card.cid[1];
        resp[1] = card.cid[2];
        resp[0] = card.cid[3];
        break;
    case 3:
        resp[0] = (uint32_t)card.rca << 16;
        break;
    case 9:
        resp[3] = card.csd[0];
        resp[2] = card.csd[1];
        resp[1] = card.csd[2];
        resp[0] = card.csd[3];
        break;
    case 17:
        resp[0] = 0x00000900u;          /* R1, transfer state, no errors */
        data_seed = argreg;
        data_left = 128u;
        fifo_words = 8u;
        data_left -= 8u;
        break;
    default:
        resp[0] = 0x00000900u;
        break;
    }
    (void)acmd;
}

uint32_t sl6806_mmio_read(uint32_t addr)
{
    if (addr >= SL6806_SD_BASE && addr < SL6806_SD_BASE + 0x300u) {
        uint32_t off = addr - SL6806_SD_BASE;

        switch (off) {
        case SL6806_SD_CTRL:
            /* The reset bit clears itself, but only after it has been read
             * at least once - so a driver that does not wait would see it
             * still set and a driver that does sees it go. */
            if (ctrl & SL6806_SD_CTRL_RESET) {
                ctrl &= ~SL6806_SD_CTRL_RESET;
                reset_waited = 1;
                return ctrl | SL6806_SD_CTRL_RESET;
            }
            return ctrl;
        case SL6806_SD_CMD:     return cmdreg;
        case SL6806_SD_ARG:     return argreg;
        case SL6806_SD_BLKSIZE: return blksize;
        case SL6806_SD_DLEN:    return dlen;
        case SL6806_SD_DTIMER:  return dtimer;
        case SL6806_SD_DCTRL:   return dctrl;
        case SL6806_SD_STA:
            return sta | ((fifo_words == 8u) ? SL6806_SD_STA_RXHALF : 0u);
        case SL6806_SD_RESP0:   return resp[0];
        case SL6806_SD_RESP1:   return resp[1];
        case SL6806_SD_RESP2:   return resp[2];
        case SL6806_SD_RESP3:   return resp[3];
        case SL6806_SD_STA2:
            return sta2 | ((fifo_words == 0u) ? SL6806_SD_STA2_RXEMPTY : 0u);
        case SL6806_SD_CLKCR:   return clkcr;
        case SL6806_SD_FIFO: {
            uint32_t w = 0;

            if (fifo_words) {
                unsigned idx = 128u - (data_left + fifo_words);
                w = block_word(data_seed, idx);
                fifo_words--;
            }
            return w;
        }
        default:
            return *cell(addr);
        }
    }

    if (addr == CRU + 0x64u) return mod_gate;
    if (addr == CRU + 0x74u) return mod_shadow;

    return *cell(addr);
}

void sl6806_mmio_write(uint32_t addr, uint32_t value)
{
    if (addr >= SL6806_SD_BASE && addr < SL6806_SD_BASE + 0x300u) {
        uint32_t off = addr - SL6806_SD_BASE;

        switch (off) {
        case SL6806_SD_CTRL:    ctrl = value; return;
        case SL6806_SD_ARG:
            argreg = value;
            last_arg_before_cmd = value;
            return;
        case SL6806_SD_CMD:
            cmdreg = value & ~0x80000000u;   /* start bit clears when done */
            if (value & 0x80000000u) {
                if (last_arg_before_cmd != argreg)
                    arg_written_first = 0;
                execute(value);
            }
            return;
        case SL6806_SD_BLKSIZE: blksize = value; return;
        case SL6806_SD_DLEN:
            dlen = value;
            if (dctrl & SL6806_SD_DCTRL_DATAEN)
                dataen_before_dlen = 1;
            return;
        case SL6806_SD_DTIMER:  dtimer = value; return;
        case SL6806_SD_DCTRL:   dctrl = value; return;
        case SL6806_SD_STA:
            if (value & SL6806_SD_STA_RXHALF) {
                rxhalf_acks++;
                if (data_left >= 8u) { fifo_words = 8u; data_left -= 8u; }
                else                 { fifo_words = data_left; data_left = 0; }
            }
            sta &= ~(value & ~SL6806_SD_STA_RXHALF);
            return;
        case SL6806_SD_CLKCR:
            clkcr = value;
            if (n_clkcr < 8u)
                clkcr_seen[n_clkcr++] = value;
            return;
        default:
            *cell(addr) = value;
            return;
        }
    }

    if (addr == CRU + 0x74u) { mod_shadow = value; return; }
    if (addr == CRU + 0x64u) {
        /* The gate only latches bits the shadow already carries - the order
         * sl6806_module.h exists to protect. */
        mod_gate = value & mod_shadow;
        return;
    }

    *cell(addr) = value;
}

/* ------------------------------------------------------------------ */
/* Cards to identify                                                    */
/* ------------------------------------------------------------------ */

/* A 4 GB SDHC: CSD version 1, C_SIZE 7663 -> 7664 * 512 KB. */
static void card_sdhc_4g(void)
{
    memset(&card, 0, sizeof(card));
    card.present = 1;
    card.v2 = 1;
    card.hc = 1;
    card.rca = 0xAAAAu;
    card.cid[0] = 0x03534453u;
    card.cid[1] = 0x30303030u;
    card.cid[2] = 0x10123456u;
    card.cid[3] = 0x789A0000u;
    card.csd[0] = 0x400E0032u;
    /* C_SIZE = ((csd[1] & 0x3F) << 16) | (csd[2] >> 16) = 0x1DEEEF */
    card.csd[1] = 0x5B590000u | 0x0000001Du;
    card.csd[2] = 0xEEEF0000u;
    card.csd[3] = 0x0A404001u;
    card.acmd41_busy = 2;
}

/* A 2 GB SDSC: CSD version 0, READ_BL_LEN 10, C_SIZE 3823, C_SIZE_MULT 7. */
static void card_sdsc_2g(void)
{
    memset(&card, 0, sizeof(card));
    card.present = 1;
    card.v2 = 1;
    card.hc = 0;
    card.rca = 0x1234u;
    card.csd[0] = 0x002E0032u;      /* CSD_STRUCTURE = 0 */
    card.csd[1] = 0x5B5A83A9u;      /* READ_BL_LEN = 10, C_SIZE[11:2] */
    card.csd[2] = 0xFFFFC87Fu;
    card.csd[3] = 0x800A4040u;
    card.acmd41_busy = 1;
}

/* ------------------------------------------------------------------ */
/* Tests                                                                */
/* ------------------------------------------------------------------ */

static void test_command_words(void)
{
    /* Each of these is one line of ROM 0x40F4, checked by sending the index
     * and reading back what the controller was handed. */
    static const struct { unsigned index; uint32_t word; } tab[] = {
        {  0, 0xA400u }, {  1, 0x2441u }, {  2, 0x25C2u }, {  3, 0x2543u },
        {  6, 0x2546u }, {  7, 0x2547u }, {  8, 0x2548u }, {  9, 0x25C9u },
        { 12, 0x454Cu }, { 13, 0x254Du }, { 16, 0x2550u }, { 17, 0x2351u },
        { 18, 0x2352u }, { 24, 0x2758u }, { 25, 0x2759u }, { 41, 0x2569u },
        { 55, 0x2577u }
    };
    unsigned i;

    printf("command words\n");
    for (i = 0; i < sizeof(tab) / sizeof(tab[0]); i++) {
        char what[48];

        model_reset();
        card_sdhc_4g();
        card.acmd41_busy = 0;
        sl6806_sd_command(tab[i].index, 0x12345678u);

        snprintf(what, sizeof(what), "CMD%u word is %#06x",
                 tab[i].index, tab[i].word);
        CHECK(cmdreg == tab[i].word, what);
    }

    /* And an index the table does not carry is refused rather than sent as
     * something plausible. */
    model_reset();
    card_sdhc_4g();
    CHECK(sl6806_sd_command(42, 0) == SL6806_SD_ERR_STATE,
          "an unlisted index is refused");
    CHECK(trace[0] == 0, "and nothing goes out on the bus");
}

static void test_identify_sdhc(void)
{
    sl6806_sd_card_t info;
    int err;

    printf("identifying a 4 GB SDHC\n");
    model_reset();
    card_sdhc_4g();

    err = sl6806_sd_begin(&info);
    CHECK(err == SL6806_SD_OK, "begin succeeds");
    CHECK(info.type == SL6806_SD_TYPE_SDHC, "card reads as high capacity");
    CHECK(info.rca == 0xAAAAu, "the card's published address is kept");
    CHECK(info.csd_version == 1u, "CSD structure version 1");
    CHECK(info.cid[0] == 0x03534453u, "CID comes back most-significant first");
    CHECK(info.block_count == (0x1DEEEFu + 1u) * 1024u,
          "capacity from the version 1 CSD");

    /* CMD0, CMD8, then CMD55/ACMD41 three times (two of them refused), then
     * CMD2, CMD3, CMD9, CMD7, CMD16. */
    CHECK(strcmp(trace, "0 8 55 41 55 41 55 41 2 3 9 7 16 ") == 0,
          "the mask ROM's identification order");

    CHECK(acmd41_arg == 0xC0100000u,
          "ACMD41 offers host capacity support once CMD8 has answered");
    CHECK(arg_written_first, "the argument is written before the command");
    CHECK(reset_waited, "the driver waits out the self-clearing reset bit");
    CHECK(mod_gate & (1u << 4), "module 36 is gated on");
    CHECK(*cell(ROMCLK_REG) & SL6806_SD_ROMCLK_BIT, "ROM clock 17 is on");

    CHECK(n_clkcr == 2u, "the clock is configured exactly twice");
    CHECK(clkcr_seen[0] == 0x20070008u, "identification clock, divider 7");
    CHECK(clkcr_seen[1] == 0x1003000Cu, "run clock, divider 3");

    /* CMD7 selects the card while the bus is still slow. */
    CHECK(strstr(trace, "9 7 16") != NULL, "CMD7 follows the CSD read");
}

static void test_identify_sdsc(void)
{
    sl6806_sd_card_t info;

    printf("identifying a 2 GB SDSC\n");
    model_reset();
    card_sdsc_2g();

    CHECK(sl6806_sd_begin(&info) == SL6806_SD_OK, "begin succeeds");
    CHECK(info.type == SL6806_SD_TYPE_SDSC_V2,
          "answering CMD8 without CCS is a version 2 standard-capacity card");
    CHECK(info.csd_version == 0u, "CSD structure version 0");
    CHECK(info.block_count == sl6806_sd_capacity(info.csd),
          "the reported capacity is the CSD's");
    CHECK(info.block_count > 3000000u && info.block_count < 4200000u,
          "a 2 GB card comes out around four million blocks");
}

static void test_identify_v1(void)
{
    sl6806_sd_card_t info;

    printf("identifying a card that will not answer CMD8\n");
    model_reset();
    card_sdsc_2g();
    card.v2 = 0;

    CHECK(sl6806_sd_begin(&info) == SL6806_SD_OK, "begin succeeds");
    CHECK(info.type == SL6806_SD_TYPE_SDSC_V1,
          "a card that ignores CMD8 is a version 1 card");
    /* [V] ROM 0x4310 builds 0x80100000, and adds the host-capacity bit only
     * when CMD8 was answered. Offering high capacity to a card that never
     * said it understood version 2 is the way to confuse a version 1 card. */
    CHECK(acmd41_arg == 0x80100000u,
          "ACMD41 goes out without the host-capacity bit");
    CHECK(strncmp(trace, "0 8 55 41 ", 10) == 0,
          "CMD8 is still sent, and its silence is the answer");
}

static void test_no_card(void)
{
    printf("an empty slot\n");
    model_reset();
    memset(&card, 0, sizeof(card));
    card.present = 0;

    /* CMD0 completes into thin air; CMD8 and everything after it times out.
     * What matters is that this ends, and says so. */
    CHECK(sl6806_sd_begin(NULL) == SL6806_SD_ERR_NO_CARD,
          "begin says there is no card");
    CHECK(sl6806_sd_read_block(0, NULL) == SL6806_SD_ERR_STATE,
          "and a read afterwards is refused rather than attempted");
}

static void test_dead_controller(void)
{
    printf("a controller that never answers\n");
    model_reset();
    card_sdhc_4g();
    card.dead = 1;

    /* The point of this test is that it returns at all. A controller that
     * never raises the command-done bit fails at CMD0, which an empty slot
     * does not - so the two faults are told apart rather than both reported
     * as "no card". */
    CHECK(sl6806_sd_begin(NULL) == SL6806_SD_ERR_CMD_TMOUT,
          "every poll is bounded and the failure is reported");
}

static void test_read_sdhc(void)
{
    static uint32_t buf[128];
    sl6806_sd_card_t info;
    unsigned i;
    int ok = 1;

    printf("reading a block from an SDHC\n");
    model_reset();
    card_sdhc_4g();
    CHECK(sl6806_sd_begin(&info) == SL6806_SD_OK, "begin succeeds");

    trace[0] = 0;
    rxhalf_acks = 0;
    CHECK(sl6806_sd_read_block(0x1234u, buf) == SL6806_SD_OK, "the read succeeds");
    CHECK(strcmp(trace, "17 ") == 0, "one CMD17, and no CMD16 repeated");
    CHECK(argreg == 0x1234u, "a high-capacity card is addressed by block");
    CHECK(blksize == 512u && dlen == 512u, "block size and length are 512");
    CHECK(dataen_before_dlen, "the data-enable bit is set before the length");
    CHECK(rxhalf_acks == 16u, "sixteen acknowledgements of eight words each");

    for (i = 0; i < 128u; i++)
        if (buf[i] != block_word(0x1234u, i))
            ok = 0;
    CHECK(ok, "every word of the block arrives, in order");
}

static void test_read_sdsc(void)
{
    static uint32_t buf[128];
    sl6806_sd_card_t info;

    printf("reading a block from an SDSC\n");
    model_reset();
    card_sdsc_2g();
    CHECK(sl6806_sd_begin(&info) == SL6806_SD_OK, "begin succeeds");

    CHECK(sl6806_sd_read_block(3u, buf) == SL6806_SD_OK, "the read succeeds");
    CHECK(argreg == 3u * 512u, "a standard-capacity card is addressed by byte");
}

static void test_read_guards(void)
{
    static uint32_t buf[128];
    unsigned char misaligned[600];
    sl6806_sd_card_t info;
    uint32_t done = 99;

    printf("guards on the read path\n");
    model_reset();
    card_sdhc_4g();
    sl6806_sd_end();            /* whatever the last test left up */

    CHECK(sl6806_sd_read_block(0, buf) == SL6806_SD_ERR_STATE,
          "a read before begin is refused");

    CHECK(sl6806_sd_begin(&info) == SL6806_SD_OK, "begin succeeds");
    CHECK(sl6806_sd_read_block(0, misaligned + 1) == SL6806_SD_ERR_STATE,
          "an unaligned buffer is refused, not read into a word at a time");

    CHECK(sl6806_sd_read_blocks(0, buf, 1u, &done) == SL6806_SD_OK
          && done == 1u, "read_blocks reports how many landed");
}

static void test_capacity(void)
{
    /* Independent of any controller: four words in, a block count out. */
    uint32_t csd[4];

    printf("capacity arithmetic\n");

    /* Version 1, C_SIZE = 0x1DEEEF -> (C_SIZE+1) * 1024 blocks. */
    csd[0] = 0x40000000u;
    csd[1] = 0x0000001Du;
    csd[2] = 0xEEEF0000u;
    csd[3] = 0;
    CHECK(sl6806_sd_capacity(csd) == (0x1DEEEFu + 1u) * 1024u,
          "version 1 CSD, 512 KB units");

    /* Version 1, the smallest legal SDHC: C_SIZE 0 -> 1024 blocks. */
    csd[1] = 0; csd[2] = 0;
    CHECK(sl6806_sd_capacity(csd) == 1024u, "version 1 CSD, C_SIZE zero");

    /* Version 0: READ_BL_LEN 9, C_SIZE 3, C_SIZE_MULT 0
     * -> (3+1) << 2 = 16 blocks of 512 bytes. */
    csd[0] = 0x00000000u;
    csd[1] = 0x00090000u | 0x00000000u;   /* READ_BL_LEN = 9, C_SIZE[11:2]=0 */
    csd[2] = (3u << 30);                  /* C_SIZE[1:0] = 3 */
    CHECK(sl6806_sd_capacity(csd) == 16u, "version 0 CSD, 512-byte blocks");

    /* Version 0 with READ_BL_LEN 10: the same C_SIZE covers twice as much. */
    csd[1] = 0x000A0000u;
    CHECK(sl6806_sd_capacity(csd) == 32u, "version 0 CSD, 1024-byte blocks");

    /* A READ_BL_LEN no card may use is refused rather than shifted by a
     * negative amount. */
    csd[1] = 0x000F0000u;
    CHECK(sl6806_sd_capacity(csd) == 0u, "an impossible READ_BL_LEN gives 0");

    /* An unknown CSD structure version is refused too. */
    csd[0] = 0xC0000000u;
    CHECK(sl6806_sd_capacity(csd) == 0u, "an unknown CSD version gives 0");
}

static void test_end(void)
{
    sl6806_sd_card_t info;

    printf("shutting down\n");
    model_reset();
    card_sdhc_4g();
    CHECK(sl6806_sd_begin(&info) == SL6806_SD_OK, "begin succeeds");

    sl6806_sd_end();
    CHECK(!(mod_gate & (1u << 4)), "module 36 is gated off again");
    CHECK(!(*cell(ROMCLK_REG) & SL6806_SD_ROMCLK_BIT), "ROM clock 17 is off");
    CHECK(sl6806_sd_read_block(0, NULL) == SL6806_SD_ERR_STATE,
          "and the driver knows it is no longer up");
}

int main(void)
{
    printf("test_sd\n");

    test_command_words();
    test_identify_sdhc();
    test_identify_sdsc();
    test_identify_v1();
    test_no_card();
    test_dead_controller();
    test_read_sdhc();
    test_read_sdsc();
    test_read_guards();
    test_capacity();
    test_end();

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
