/*
 * test_fat.c - the FAT reader against filesystems built here.
 *
 * WHY THIS FILE IS THE POINT OF THE FAT LAYER. The SD host on this board has
 * never completed a command, so a filesystem reader wired straight to it
 * could not be checked at all - it would be a fourth layer of untested code
 * on top of three. Because sl6806_fat.c takes a block-reader callback and
 * knows nothing about SD, the images below are real FAT16 and FAT32 volumes
 * built in memory, and every path through the reader runs here.
 *
 * So when a card finally answers, this part is already known to work and the
 * question is only whether the SD driver does.
 *
 * The images are constructed rather than copied from a fixture, which is
 * deliberate: a fixture proves the reader agrees with whatever produced it,
 * while a constructed image proves it agrees with the *specification*, field
 * offset by field offset. Four things get pinned down that a fixture would
 * let slide:
 *
 *   1. **The FAT width comes from the cluster count**, not the "FAT16   "
 *      string in the boot sector, which is a comment and is routinely wrong.
 *      The two images below differ only in geometry.
 *   2. **The MBR is optional.** A card straight from a camera has a partition
 *      table; one formatted by a phone may not. Both are tested.
 *   3. **Long-name entries are skipped, not parsed as files.** An LFN
 *      fragment has attribute 0x0F and would otherwise appear as a file with
 *      a garbage name.
 *   4. **Chains are followed.** Files and directories bigger than one cluster
 *      are the case that a single-cluster test never reaches.
 */

#include <stdio.h>
#include <string.h>

#include "sl6806_fat.h"

static int failures, checks;

#define CHECK(cond, what) do {                                          \
        checks++;                                                       \
        if (!(cond)) {                                                  \
            printf("  FAIL %s (%s:%d)\n", (what), __FILE__, __LINE__);  \
            failures++;                                                 \
        }                                                               \
    } while (0)

/* ------------------------------------------------------------------ */
/* A card, in memory                                                   */
/* ------------------------------------------------------------------ */

/*
 * A sparse card. The FAT type is decided by the *cluster count*, and the
 * thresholds are 4085 and 65525 - so a test image has to be big enough to
 * land on the right side of them, and a FAT32 volume needs a couple of
 * hundred thousand sectors. Backing only the sectors actually written keeps
 * that to a few kilobytes, and unwritten sectors read as zeros exactly as a
 * freshly formatted card's would.
 */
#define CARD_SECTORS 200000u
#define BACKED       96u

static struct { uint32_t lba; uint8_t data[512]; } backed[BACKED];
static unsigned n_backed;
static unsigned reads;

/* The sector at `lba`, allocating a backing store for it. */
static uint8_t *sector(uint32_t lba)
{
    unsigned i;

    for (i = 0; i < n_backed; i++)
        if (backed[i].lba == lba)
            return backed[i].data;
    if (n_backed == BACKED) {
        printf("  test image needs more than %u backed sectors\n", BACKED);
        return backed[0].data;
    }
    backed[n_backed].lba = lba;
    memset(backed[n_backed].data, 0, 512);
    return backed[n_backed++].data;
}

static int card_read(uint32_t lba, void *buf, void *ctx)
{
    unsigned i;

    (void)ctx;
    if (lba >= CARD_SECTORS)
        return -1;                  /* off the end: a real I/O failure */
    reads++;
    memset(buf, 0, 512);
    for (i = 0; i < n_backed; i++)
        if (backed[i].lba == lba) {
            memcpy(buf, backed[i].data, 512);
            break;
        }
    return 0;
}

/* A reader that always fails, to check errors propagate rather than being
 * mistaken for an empty directory. */
static int dead_read(uint32_t lba, void *buf, void *ctx)
{
    (void)lba; (void)buf; (void)ctx;
    return -99;
}

static void card_reset(void)
{
    n_backed = 0;
    reads = 0;
}

static void put16(uint8_t *p, unsigned off, uint16_t v)
{
    p[off] = (uint8_t)v;
    p[off + 1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, unsigned off, uint32_t v)
{
    p[off] = (uint8_t)v;
    p[off + 1] = (uint8_t)(v >> 8);
    p[off + 2] = (uint8_t)(v >> 16);
    p[off + 3] = (uint8_t)(v >> 24);
}

/* --- geometry of whichever image is currently built ----------------- */

static struct {
    uint32_t part_lba, fat_lba, root_lba, data_lba;
    uint32_t root_cluster;
    unsigned spc, type;
} img;

static void set_fat_entry(uint32_t cluster, uint32_t value)
{
    if (img.type == 32u) {
        uint32_t lba = img.fat_lba + cluster / 128u;
        put32(sector(lba), (cluster % 128u) * 4u, value);
    } else {
        uint32_t lba = img.fat_lba + cluster / 256u;
        put16(sector(lba), (cluster % 256u) * 2u, (uint16_t)value);
    }
}

static uint32_t cluster_lba(uint32_t cluster)
{
    return img.data_lba + (cluster - 2u) * img.spc;
}

/* Write a directory entry into `sector` at slot `slot`. */
static void put_entry(uint32_t lba, unsigned slot, const char *name83,
                      uint8_t attr, uint32_t cluster, uint32_t size)
{
    uint8_t *d = sector(lba) + slot * 32u;
    unsigned i;

    for (i = 0; i < 11u; i++)
        d[i] = (uint8_t)name83[i];
    d[11] = attr;
    put16(d, 20, (uint16_t)(cluster >> 16));
    put16(d, 26, (uint16_t)cluster);
    put32(d, 28, size);
}

/*
 * Build a FAT16 volume inside an MBR partition.
 *
 * Geometry chosen so the cluster count lands between 4085 and 65524, which is
 * what makes it FAT16 by the specification's own rule.
 */
static void build_fat16(void)
{
    uint8_t *mbr, *bpb;
    uint32_t part = 64u, total = 40000u, fat_sectors = 160u, i;

    card_reset();
    memset(&img, 0, sizeof(img));

    /* MBR: one partition, type 0x06, starting at 64. */
    mbr = sector(0);
    mbr[446 + 4] = 0x06;
    put32(mbr, 446 + 8, part);
    put32(mbr, 446 + 12, total);
    mbr[510] = 0x55; mbr[511] = 0xAA;

    bpb = sector(part);
    bpb[0] = 0xEB; bpb[1] = 0x3C; bpb[2] = 0x90;
    memcpy(bpb + 3, "MSDOS5.0", 8);
    put16(bpb, 11, 512);
    bpb[13] = 1;                        /* sectors per cluster */
    put16(bpb, 14, 4);                  /* reserved */
    bpb[16] = 2;                        /* two FATs */
    put16(bpb, 17, 512);                /* root entries */
    put16(bpb, 19, (uint16_t)total);
    put16(bpb, 22, (uint16_t)fat_sectors);
    memcpy(bpb + 43, "TESTVOL16  ", 11);
    memcpy(bpb + 54, "FAT12   ", 8);    /* deliberately a lie - see the header */
    bpb[510] = 0x55; bpb[511] = 0xAA;

    img.part_lba = part;
    img.spc = 1;
    img.type = 16;
    img.fat_lba = part + 4u;
    img.root_lba = img.fat_lba + 2u * fat_sectors;
    img.data_lba = img.root_lba + (512u * 32u) / 512u;

    /* ~39,700 clusters: comfortably inside FAT16's 4085..65524 window. */

    /* Root: a long-name fragment (must be skipped), the volume label (must be
     * skipped), a file of two clusters, and a subdirectory. */
    put_entry(img.root_lba, 0, "\x41LONGNAM\x00\x00\x00", SL6806_FAT_ATTR_LFN, 0, 0);
    put_entry(img.root_lba, 1, "TESTVOL16  ", SL6806_FAT_ATTR_LABEL, 0, 0);
    put_entry(img.root_lba, 2, "HELLO   TXT", 0, 2, 600);
    put_entry(img.root_lba, 3, "SUBDIR     ", SL6806_FAT_ATTR_DIR, 5, 0);

    /* HELLO.TXT spans clusters 2 and 3 at one sector each. */
    set_fat_entry(2, 3);
    set_fat_entry(3, 0xFFFF);
    for (i = 0; i < 512u; i++)
        sector(cluster_lba(2))[i] = (uint8_t)('A' + (i % 26u));
    for (i = 0; i < 512u; i++)
        sector(cluster_lba(3))[i] = (uint8_t)('a' + (i % 26u));

    /* SUBDIR holds one file. */
    set_fat_entry(5, 0xFFFF);
    put_entry(cluster_lba(5), 0, ".          ", SL6806_FAT_ATTR_DIR, 5, 0);
    put_entry(cluster_lba(5), 1, "..         ", SL6806_FAT_ATTR_DIR, 0, 0);
    put_entry(cluster_lba(5), 2, "INNER   BIN", 0, 6, 4);
    set_fat_entry(6, 0xFFFF);
    memcpy(sector(cluster_lba(6)), "\xDE\xAD\xBE\xEF", 4);
}

/*
 * Build a FAT32 volume with no partition table - the boot sector is LBA 0.
 * Four sectors per cluster, and a root directory that spans two clusters so
 * the chain-following path in readdir() is exercised.
 */
static void build_fat32(void)
{
    uint8_t *bpb;
    uint32_t total = 200000u, fat_sectors = 1536u, i;

    card_reset();
    memset(&img, 0, sizeof(img));

    bpb = sector(0);
    bpb[0] = 0xEB; bpb[1] = 0x58; bpb[2] = 0x90;
    put16(bpb, 11, 512);
    bpb[13] = 1;                        /* sectors per cluster */
    put16(bpb, 14, 32);                 /* reserved */
    bpb[16] = 2;
    put16(bpb, 17, 0);                  /* no fixed root: FAT32 */
    put16(bpb, 19, 0);
    put32(bpb, 32, total);
    put16(bpb, 22, 0);
    put32(bpb, 36, fat_sectors);
    put32(bpb, 44, 2);                  /* root cluster */
    memcpy(bpb + 71, "TESTVOL32  ", 11);
    bpb[510] = 0x55; bpb[511] = 0xAA;

    img.part_lba = 0;
    img.spc = 1;
    img.type = 32;
    img.fat_lba = 32u;
    img.data_lba = 32u + 2u * fat_sectors;
    img.root_cluster = 2;

    /* ~196,000 clusters, so the count says FAT32 rather than the BPB string.
     * The root spans clusters 2 -> 3, one sector each, so the sixteen entries
     * of cluster 2 fill it and the seventeenth is only reachable by following
     * the chain. */
    set_fat_entry(2, 3);
    set_fat_entry(3, 0x0FFFFFFF);

    for (i = 0; i < 16u; i++) {
        char name[12];

        snprintf(name, sizeof(name), "FILE%03u TXT", (unsigned)i);
        put_entry(cluster_lba(2), i, name, 0, 0, i);
    }
    put_entry(cluster_lba(3), 0, "LAST    TXT", 0, 10, 9);

    set_fat_entry(10, 0x0FFFFFFF);
    memcpy(sector(cluster_lba(10)), "penultima", 9);
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

static void test_fat16(void)
{
    sl6806_fat_t fs;
    sl6806_fat_entry_t e;
    static uint8_t buf[1024];
    int32_t n;

    printf("FAT16 in an MBR partition\n");
    build_fat16();

    CHECK(sl6806_fat_mount(&fs, card_read, NULL) == SL6806_FAT_OK, "it mounts");
    CHECK(fs.type == 16u, "the cluster count says FAT16");
    CHECK(fs.part_lba == 64u, "the partition was found in the MBR");
    CHECK(fs.sectors_per_cluster == 1u, "geometry came from the BPB");
    CHECK(strcmp(fs.label, "TESTVOL16") == 0, "the label is trimmed");
    CHECK(sl6806_fat_root(&fs) == 0u, "FAT16's root is the fixed region");

    /* The long-name fragment and the volume label must not be entries. */
    CHECK(sl6806_fat_readdir(&fs, 0, 0, &e) == SL6806_FAT_OK, "first entry");
    CHECK(strcmp(e.name, "HELLO.TXT") == 0, "long names and labels are skipped");
    CHECK(e.size == 600u, "size comes back");
    CHECK(!(e.attr & SL6806_FAT_ATTR_DIR), "and it is not a directory");

    CHECK(sl6806_fat_readdir(&fs, 0, 1, &e) == SL6806_FAT_OK, "second entry");
    CHECK(strcmp(e.name, "SUBDIR") == 0, "a name with no extension has no dot");
    CHECK((e.attr & SL6806_FAT_ATTR_DIR) != 0, "and is a directory");

    CHECK(sl6806_fat_readdir(&fs, 0, 2, &e) == SL6806_FAT_ERR_END,
          "past the last entry is END, not an error");

    /* A read that crosses a cluster boundary. */
    n = sl6806_fat_read(&fs, 2, 0, buf, 600);
    CHECK(n == 600, "600 bytes read across two clusters");
    CHECK(buf[0] == 'A' && buf[25] == 'Z', "the first cluster's contents");
    CHECK(buf[512] == 'a', "and the second's, following the chain");

    /* An offset that lands in the second cluster. */
    n = sl6806_fat_read(&fs, 2, 520, buf, 8);
    CHECK(n == 8, "a read starting past the first cluster");
    CHECK(buf[0] == (uint8_t)('a' + 8), "lands at the right byte");

    /* The subdirectory. */
    CHECK(sl6806_fat_readdir(&fs, 5, 2, &e) == SL6806_FAT_OK, "into a subdir");
    CHECK(strcmp(e.name, "INNER.BIN") == 0, "past its . and .. entries");
    n = sl6806_fat_read(&fs, e.cluster, 0, buf, 4);
    CHECK(n == 4 && buf[0] == 0xDE && buf[3] == 0xEF, "and its contents");
}

static void test_fat32(void)
{
    sl6806_fat_t fs;
    sl6806_fat_entry_t e;
    static uint8_t buf[32];
    int32_t n;

    printf("FAT32 with no partition table\n");
    build_fat32();

    CHECK(sl6806_fat_mount(&fs, card_read, NULL) == SL6806_FAT_OK,
          "a boot sector at LBA 0 mounts without an MBR");
    CHECK(fs.type == 32u, "the cluster count says FAT32");
    CHECK(fs.part_lba == 0u, "the volume starts at zero");
    CHECK(sl6806_fat_root(&fs) == 2u, "the root cluster comes from the BPB");
    CHECK(strcmp(fs.label, "TESTVOL32") == 0, "the FAT32 label offset");

    CHECK(sl6806_fat_readdir(&fs, 2, 0, &e) == SL6806_FAT_OK, "first entry");
    CHECK(strcmp(e.name, "FILE000.TXT") == 0, "named as expected");

    CHECK(sl6806_fat_readdir(&fs, 2, 15, &e) == SL6806_FAT_OK,
          "the last entry of the first cluster");
    CHECK(strcmp(e.name, "FILE015.TXT") == 0, "still right at the boundary");

    /* This one is only reachable by following the root's chain. */
    CHECK(sl6806_fat_readdir(&fs, 2, 16, &e) == SL6806_FAT_OK,
          "an entry in the root's SECOND cluster");
    CHECK(strcmp(e.name, "LAST.TXT") == 0, "the chain was followed");

    n = sl6806_fat_read(&fs, e.cluster, 0, buf, 9);
    CHECK(n == 9 && memcmp(buf, "penultima", 9) == 0, "and its contents");

    CHECK(sl6806_fat_readdir(&fs, 2, 17, &e) == SL6806_FAT_ERR_END,
          "and then the end");
}

static void test_refusals(void)
{
    sl6806_fat_t fs;
    sl6806_fat_entry_t e;

    printf("things it should refuse\n");

    /* No signature at all. */
    card_reset();
    CHECK(sl6806_fat_mount(&fs, card_read, NULL) == SL6806_FAT_ERR_NOFS,
          "an unformatted card is refused");

    /* A signature but nothing behind it - the case that matters, because
     * 0x55AA appears on any card that was ever partitioned. */
    card_reset();
    sector(0)[510] = 0x55; sector(0)[511] = 0xAA;
    CHECK(sl6806_fat_mount(&fs, card_read, NULL) == SL6806_FAT_ERR_NOFS,
          "a boot signature alone is not a filesystem");

    /* FAT12 geometry: too few clusters. Refused rather than half-read, and
     * the refusal is specific so a caller can say why. */
    build_fat16();
    put16(sector(64), 19, 1000);        /* shrink the volume */
    CHECK(sl6806_fat_mount(&fs, card_read, NULL) == SL6806_FAT_ERR_FAT12,
          "FAT12 is refused by cluster count, not by the BPB string");

    /* A BPB that looks like one and is not usable: zero reserved sectors
     * would put the FAT on top of the boot sector. */
    build_fat16();
    put16(sector(64), 14, 0);
    CHECK(sl6806_fat_mount(&fs, card_read, NULL) == SL6806_FAT_ERR_PARAM,
          "zero reserved sectors is refused");

    /* Zero sectors per cluster does not even look like a BPB, so the mount
     * falls through the partition scan and finds nothing. Different error,
     * and worth pinning: it must not divide by it. */
    build_fat16();
    sector(64)[13] = 0;
    CHECK(sl6806_fat_mount(&fs, card_read, NULL) == SL6806_FAT_ERR_NOFS,
          "zero sectors per cluster never reaches the arithmetic");

    /* I/O failures must propagate, not read as an empty filesystem. */
    CHECK(sl6806_fat_mount(&fs, dead_read, NULL) == SL6806_FAT_ERR_IO,
          "a failing block reader is an I/O error");

    build_fat16();
    CHECK(sl6806_fat_mount(&fs, card_read, NULL) == SL6806_FAT_OK, "remount");
    fs.read = dead_read;
    CHECK(sl6806_fat_readdir(&fs, 0, 0, &e) == SL6806_FAT_ERR_IO,
          "and so is one that fails later, rather than END");
}

int main(void)
{
    printf("test_fat\n");

    test_fat16();
    test_fat32();
    test_refusals();

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
