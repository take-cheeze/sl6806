/*
 * SdFiles - read the card's filesystem, as far as it gets.
 *
 * ===================================================================
 *  WHAT THIS WILL DO ON THIS BOARD, HONESTLY
 * ===================================================================
 * It will almost certainly stop at the first block read, because the SD host
 * on this board has never completed a command: CMD0 latches its start bit and
 * drops it in under a microsecond with the status register empty
 * (docs/SD.md). Everything downstream of that is unreachable.
 *
 * It is worth running anyway, and it is built to fail informatively rather
 * than to fail:
 *
 *   - it reports **where** it stops, and the places it can stop are now
 *     distinct: bring-up, identification, the block read, the partition
 *     table, the boot sector, the directory;
 *   - it tries the block read **even when identification fails**, through
 *     sl6806_sd_read_block_unchecked(). Identification needs six commands to
 *     work; a data phase needs one. If the FIFO ever fills, that is a
 *     different and much better failure than the one we have;
 *   - and everything above the block layer has been tested. sl6806_fat.c
 *     takes a block reader and knows nothing about SD, so
 *     tests/host/test_fat.c builds real FAT16 and FAT32 volumes in memory and
 *     reads them back. **If a sector ever arrives, the filesystem code
 *     already works.**
 *
 * So this is not a hope that the whole stack works. It is one unknown - does
 * a sector arrive - with a tested reader waiting behind it.
 *
 *     make SKETCH=examples/SdFiles RUN_MODE=poll run
 *
 * Read-only throughout: sl6806_fat.c cannot write, and this never asks it to.
 *
 * ===================================================================
 *  IF YOU JUST WANT THE FILES
 * ===================================================================
 * Unplug the device and plug it back in **without** holding a boot key. The
 * stock firmware enumerates as USB mass storage (`301a:2801`) and the card
 * mounts on your PC. That is the product's own card-reader mode, it works
 * today, and it is the right way to get data off the card.
 *
 * Doing that is also a measurement worth having: it proves the socket, the
 * card and the SD host all work under the vendor's firmware, which would
 * narrow docs/SD.md's open question to "and a payload cannot drive it".
 */

#include <Arduino.h>

extern "C" {
#include "sl6806_sd.h"
#include "sl6806_fat.h"
#include "sl6806_console.h"
}

static int  phase;
static bool done;
static bool identified;
static bool high_capacity;
static sl6806_fat_t fs;
static sl6806_sd_card_t card;

static void printHex(uint32_t v, int digits = 8)
{
    static const char *hex = "0123456789ABCDEF";

    Serial.print("0x");
    for (int i = digits - 1; i >= 0; i--)
        Serial.print(hex[(v >> (i * 4)) & 0xF]);
}

static bool drained()
{
    return sl6806_console_space() >= (int)(SL6806_CONSOLE_SIZE / 2);
}

/*
 * The block reader the filesystem is given. It goes through the driver's
 * unchecked path, because on this board the checked one refuses: it requires
 * a successful begin(), and begin() has never succeeded.
 */
static int block_read(uint32_t lba, void *buf, void *ctx)
{
    (void)ctx;
    if (identified)
        return sl6806_sd_read_block(lba, buf);
    return sl6806_sd_read_block_unchecked(lba, buf, high_capacity ? 1 : 0);
}

static void report(const char *what, int err)
{
    Serial.print("  ");
    Serial.print(what);
    Serial.print(": ");
    Serial.print(err);
    Serial.print(" (");
    Serial.print(sl6806_sd_strerror(err));
    Serial.println(")");
}

static const char *fat_error(int err)
{
    switch (err) {
    case SL6806_FAT_OK:        return "ok";
    case SL6806_FAT_ERR_IO:    return "the card would not read";
    case SL6806_FAT_ERR_NOFS:  return "no filesystem found";
    case SL6806_FAT_ERR_FAT12: return "FAT12, not supported";
    case SL6806_FAT_ERR_PARAM: return "a boot sector this code will not believe";
    case SL6806_FAT_ERR_END:   return "end of directory";
    default:                   return "unknown";
    }
}

void setup()
{
    Serial.begin(115200);
    Serial.println();
    Serial.println("=== SL6806: read the card's filesystem ===");
    Serial.println("Expected to stop at the first block read - no command has");
    Serial.println("ever completed on this host. The filesystem code behind");
    Serial.println("it is tested; see the sketch header.");
    Serial.flush();

    phase = 0;
    done = false;
    identified = false;
    high_capacity = true;
}

void loop()
{
    static uint32_t sec[128];
    static unsigned index;

    if (done || !drained())
        return;

    switch (phase) {

    /* --- 1: bring the card up ------------------------------------ */
    case 0: {
        int err;

        Serial.println();
        Serial.println("--- the card ---");
        err = sl6806_sd_begin(&card);
        report("begin", err);

        if (err == SL6806_SD_OK) {
            identified = true;
            high_capacity = (card.type == SL6806_SD_TYPE_SDHC);
            Serial.print("  capacity: ");
            Serial.print(card.block_count / 2048u);
            Serial.println(" MiB");
        } else {
            Serial.println("  Identification failed, so the block read below");
            Serial.println("  goes through the unchecked path and assumes");
            Serial.println("  high-capacity addressing.");
        }
        phase++;
        return;
    }

    /* --- 2: one sector ------------------------------------------- */
    case 1: {
        int err = block_read(0, sec, NULL);

        Serial.println();
        Serial.println("--- sector 0 ---");
        report("read", err);

        if (err != SL6806_SD_OK) {
            Serial.println();
            Serial.println("  This is where it was always going to stop. The");
            Serial.println("  host takes the command and reports nothing; see");
            Serial.println("  docs/SD.md for the four hypotheses already ruled");
            Serial.println("  out and what is left.");
            phase = 99;
            return;
        }

        {
            const uint8_t *b = (const uint8_t *)sec;

            Serial.print("  first bytes: ");
            for (unsigned i = 0; i < 8u; i++) {
                printHex(b[i], 2);
                Serial.print(' ');
            }
            Serial.print("  signature ");
            printHex(((uint32_t)b[510] << 8) | b[511], 4);
            Serial.println();
        }
        phase++;
        return;
    }

    /* --- 3: mount ------------------------------------------------ */
    case 2: {
        int err = sl6806_fat_mount(&fs, block_read, NULL);

        Serial.println();
        Serial.println("--- the filesystem ---");
        Serial.print("  mount: ");
        Serial.print(err);
        Serial.print(" (");
        Serial.print(fat_error(err));
        Serial.println(")");

        if (err != SL6806_FAT_OK) {
            phase = 99;
            return;
        }

        Serial.print("  FAT");
        Serial.print(fs.type);
        Serial.print(", label \"");
        Serial.print(fs.label);
        Serial.print("\", ");
        Serial.print(fs.clusters);
        Serial.print(" clusters of ");
        Serial.print((uint32_t)fs.sectors_per_cluster * 512u);
        Serial.println(" bytes");
        Serial.print("  volume starts at LBA ");
        Serial.println(fs.part_lba);

        index = 0;
        phase++;
        return;
    }

    /* --- 4: the root directory, a few entries per poll ------------ */
    case 3: {
        sl6806_fat_entry_t e;

        if (index == 0) {
            Serial.println();
            Serial.println("--- the root directory ---");
        }

        for (unsigned n = 0; n < 6u; n++) {
            int err = sl6806_fat_readdir(&fs, sl6806_fat_root(&fs), index, &e);

            if (err == SL6806_FAT_ERR_END) {
                Serial.print("  ");
                Serial.print(index);
                Serial.println(" entries.");
                phase++;
                return;
            }
            if (err != SL6806_FAT_OK) {
                Serial.print("  readdir failed: ");
                Serial.println(fat_error(err));
                phase = 99;
                return;
            }

            Serial.print("  ");
            Serial.print((e.attr & SL6806_FAT_ATTR_DIR) ? "[DIR] " : "      ");
            Serial.print(e.name);
            if (!(e.attr & SL6806_FAT_ATTR_DIR)) {
                Serial.print("   ");
                Serial.print(e.size);
                Serial.print(" bytes");
            }
            Serial.println();
            index++;
        }
        return;                     /* more next time round */
    }

    /* --- 5: the first file's opening bytes ------------------------ */
    case 4: {
        sl6806_fat_entry_t e;
        static uint8_t buf[64];
        unsigned i;
        int32_t n;

        Serial.println();
        Serial.println("--- the first file ---");

        for (i = 0; ; i++) {
            int err = sl6806_fat_readdir(&fs, sl6806_fat_root(&fs), i, &e);

            if (err != SL6806_FAT_OK) {
                Serial.println("  no regular file in the root.");
                phase = 99;
                return;
            }
            if (!(e.attr & SL6806_FAT_ATTR_DIR))
                break;
        }

        Serial.print("  ");
        Serial.print(e.name);
        Serial.print(", ");
        Serial.print(e.size);
        Serial.println(" bytes");

        n = sl6806_fat_read(&fs, e.cluster, 0, buf, sizeof(buf));
        if (n < 0) {
            Serial.print("  read failed: ");
            Serial.println(fat_error((int)n));
            phase = 99;
            return;
        }

        Serial.print("  first ");
        Serial.print((uint32_t)n);
        Serial.println(" bytes:");
        Serial.print("    ");
        for (i = 0; i < (unsigned)n; i++) {
            char c = (char)buf[i];

            Serial.print((c >= 0x20 && c < 0x7F) ? c : '.');
        }
        Serial.println();
        phase = 99;
        return;
    }

    default:
        Serial.println();
        Serial.println("done.");
        done = true;
        return;
    }
}
