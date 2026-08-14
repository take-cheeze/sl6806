/*
 * sl6806_fat.h - read-only FAT16/FAT32, on top of anything that reads blocks.
 *
 * =====================================================================
 *  WHAT THIS IS, AND WHAT IT IS NOT
 * =====================================================================
 * A card in the slot is a partition table, a boot sector, a file allocation
 * table and some directories. This turns those into a directory listing and a
 * file read, and it is the last layer between `sl6806_sd_read_block()` and
 * "read the filesystem".
 *
 * It is **read-only, and deliberately**. Writing to FAT means updating two
 * copies of the allocation table and a directory entry in the right order, and
 * getting that wrong on a card that is somebody's only copy of something is a
 * different class of mistake from a driver that does not work. Nothing here
 * writes a byte.
 *
 * =====================================================================
 *  IT DOES NOT KNOW ABOUT SD
 * =====================================================================
 * The only thing it needs is a function that reads a 512-byte block. That is
 * not squeamishness about layering - it is what makes this testable. The SD
 * host on this board has never completed a command (docs/SD.md), so a FAT
 * reader wired directly to it could not be checked at all. Given a block
 * reader, `tests/host/test_fat.c` builds a real FAT16 and FAT32 image in
 * memory and reads them back, and every line below is exercised on the host.
 *
 * So when a card finally answers, the question is whether the *SD* driver
 * works. This part will already have been tested.
 *
 * =====================================================================
 *  WHAT IT SUPPORTS
 * =====================================================================
 *   - 512-byte sectors, which is every SD card;
 *   - an MBR partition table, or a boot sector at LBA 0 with no table;
 *   - FAT16 and FAT32, including the FAT32 root directory as a cluster
 *     chain and the FAT16 root as a fixed region;
 *   - 8.3 names. Long file names are skipped rather than assembled, and the
 *     short name is what you get - which is enough to find a file and read
 *     it, and honest about what it is doing.
 *
 * FAT12 is refused rather than half-supported: its 12-bit entries straddle
 * sector boundaries, and no SD card is formatted that way.
 */
#ifndef SL6806_FAT_H
#define SL6806_FAT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Read one 512-byte block into `buf`. Return 0 on success, anything else on
 * failure - the value is passed straight back to the caller, so an
 * SL6806_SD_ERR_* code survives all the way up.
 */
typedef int (*sl6806_fat_read_fn)(uint32_t lba, void *buf, void *ctx);

enum {
    SL6806_FAT_OK        = 0,
    SL6806_FAT_ERR_IO    = -1,   /* the block reader failed */
    SL6806_FAT_ERR_NOFS  = -2,   /* no boot signature, or not FAT */
    SL6806_FAT_ERR_FAT12 = -3,   /* FAT12, refused */
    SL6806_FAT_ERR_PARAM = -4,   /* a BPB this code will not believe */
    SL6806_FAT_ERR_END   = -5    /* no more directory entries */
};

#define SL6806_FAT_SECTOR 512u

typedef struct {
    sl6806_fat_read_fn read;
    void              *ctx;

    uint32_t part_lba;            /* where the volume starts on the card   */
    uint8_t  type;                /* 16 or 32                              */
    uint8_t  sectors_per_cluster;
    uint8_t  num_fats;
    uint16_t reserved_sectors;
    uint16_t root_entries;        /* FAT16 only                            */
    uint32_t fat_sectors;
    uint32_t total_sectors;

    uint32_t fat_lba;             /* first FAT                             */
    uint32_t root_lba;            /* FAT16: the fixed root region          */
    uint32_t data_lba;            /* cluster 2 starts here                 */
    uint32_t root_cluster;        /* FAT32                                 */
    uint32_t clusters;            /* count of data clusters                */

    char     label[12];           /* from the BPB, trimmed; may be empty   */
} sl6806_fat_t;

typedef struct {
    char     name[13];            /* "NAME.EXT", NUL-terminated            */
    uint32_t size;
    uint32_t cluster;
    uint8_t  attr;
} sl6806_fat_entry_t;

#define SL6806_FAT_ATTR_READONLY  0x01u
#define SL6806_FAT_ATTR_HIDDEN    0x02u
#define SL6806_FAT_ATTR_SYSTEM    0x04u
#define SL6806_FAT_ATTR_LABEL     0x08u
#define SL6806_FAT_ATTR_DIR       0x10u
#define SL6806_FAT_ATTR_LFN       0x0Fu

/*
 * Find the filesystem and fill in `fs`. Looks at LBA 0: if it is a partition
 * table, the first FAT partition in it is used; if it is a boot sector
 * itself, that is used. Returns SL6806_FAT_OK or one of the errors above.
 */
int sl6806_fat_mount(sl6806_fat_t *fs, sl6806_fat_read_fn read, void *ctx);

/* The root directory's starting cluster, or 0 for a FAT16 fixed root. Pass
 * the result to sl6806_fat_readdir(). */
uint32_t sl6806_fat_root(const sl6806_fat_t *fs);

/*
 * Read the `index`-th usable entry of a directory - deleted entries, long-name
 * fragments and the volume label are skipped, so indices are dense.
 *
 * Returns SL6806_FAT_OK, SL6806_FAT_ERR_END past the last entry, or an I/O
 * error. `dir_cluster` of 0 means the FAT16 fixed root.
 */
int sl6806_fat_readdir(const sl6806_fat_t *fs, uint32_t dir_cluster,
                       unsigned index, sl6806_fat_entry_t *out);

/*
 * Read `len` bytes from `offset` within a file. Returns the number of bytes
 * read, or a negative error. Reads that run past the end of the chain stop
 * short rather than failing.
 */
int32_t sl6806_fat_read(const sl6806_fat_t *fs, uint32_t cluster,
                        uint32_t offset, void *buf, uint32_t len);

/* The next cluster in a chain, 0 for "end of chain", or a negative error. */
int32_t sl6806_fat_next_cluster(const sl6806_fat_t *fs, uint32_t cluster);

#ifdef __cplusplus
}
#endif

#endif /* SL6806_FAT_H */
