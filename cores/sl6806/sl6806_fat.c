/*
 * sl6806_fat.c - see sl6806_fat.h. Read-only FAT16/FAT32.
 *
 * Every multi-byte field in FAT is little-endian and unaligned, so they are
 * assembled byte by byte rather than cast. A struct overlay would be shorter
 * and would fault on a Cortex-M the first time a BPB field landed on an odd
 * offset.
 */

#include "sl6806_fat.h"

#define RD16(p, o) ((uint16_t)((p)[(o)] | ((uint16_t)(p)[(o) + 1] << 8)))
#define RD32(p, o) ((uint32_t)((p)[(o)] | ((uint32_t)(p)[(o) + 1] << 8) \
                             | ((uint32_t)(p)[(o) + 2] << 16)           \
                             | ((uint32_t)(p)[(o) + 3] << 24)))

/* One scratch sector. This layer is single-threaded and called from loop(),
 * and 512 bytes of BSS is cheaper than making every caller supply one. */
static uint8_t g_sector[SL6806_FAT_SECTOR];

static int fat_read(const sl6806_fat_t *fs, uint32_t lba, void *buf)
{
    int err = fs->read(lba, buf, fs->ctx);

    return err ? SL6806_FAT_ERR_IO : SL6806_FAT_OK;
}

/* ------------------------------------------------------------------ */
/* Mounting                                                            */
/* ------------------------------------------------------------------ */

/* Is this sector a FAT boot sector rather than a partition table? Both end in
 * 0x55AA, so the signature alone decides nothing. A boot sector opens with a
 * jump and carries a sector size of 512. */
static int looks_like_bpb(const uint8_t *s)
{
    if (!(s[0] == 0xEB || s[0] == 0xE9))
        return 0;
    if (RD16(s, 11) != SL6806_FAT_SECTOR)
        return 0;
    return s[13] != 0;                     /* sectors per cluster */
}

static int parse_bpb(sl6806_fat_t *fs, const uint8_t *s, uint32_t lba)
{
    uint32_t root_sectors, data_sectors, first_data;
    unsigned i;

    if (RD16(s, 11) != SL6806_FAT_SECTOR)
        return SL6806_FAT_ERR_PARAM;

    fs->part_lba            = lba;
    fs->sectors_per_cluster = s[13];
    fs->reserved_sectors    = RD16(s, 14);
    fs->num_fats            = s[16];
    fs->root_entries        = RD16(s, 17);

    fs->total_sectors = RD16(s, 19);
    if (fs->total_sectors == 0)
        fs->total_sectors = RD32(s, 32);

    fs->fat_sectors = RD16(s, 22);
    if (fs->fat_sectors == 0)
        fs->fat_sectors = RD32(s, 36);     /* FAT32's 32-bit count */

    if (fs->sectors_per_cluster == 0 || fs->num_fats == 0
        || fs->reserved_sectors == 0 || fs->fat_sectors == 0
        || fs->total_sectors == 0)
        return SL6806_FAT_ERR_PARAM;

    /* The root directory is a fixed region on FAT16 and a cluster chain on
     * FAT32, where root_entries is zero and this rounds to nothing. */
    root_sectors = ((uint32_t)fs->root_entries * 32u
                    + SL6806_FAT_SECTOR - 1u) / SL6806_FAT_SECTOR;

    first_data = fs->reserved_sectors
               + (uint32_t)fs->num_fats * fs->fat_sectors
               + root_sectors;
    if (first_data >= fs->total_sectors)
        return SL6806_FAT_ERR_PARAM;

    data_sectors = fs->total_sectors - first_data;
    fs->clusters = data_sectors / fs->sectors_per_cluster;

    /* The count of data clusters is what decides the FAT width - not the
     * string in the boot sector, which is a comment and is routinely wrong.
     * These are the specification's own thresholds. */
    if (fs->clusters < 4085u)
        return SL6806_FAT_ERR_FAT12;
    fs->type = (fs->clusters < 65525u) ? 16u : 32u;

    fs->fat_lba  = lba + fs->reserved_sectors;
    fs->root_lba = lba + first_data - root_sectors;
    fs->data_lba = lba + first_data;
    fs->root_cluster = (fs->type == 32u) ? RD32(s, 44) : 0u;

    if (fs->type == 32u && fs->root_cluster < 2u)
        return SL6806_FAT_ERR_PARAM;

    /* The label lives at a different offset in each, and is space-padded. */
    {
        const uint8_t *lab = (fs->type == 32u) ? s + 71 : s + 43;

        for (i = 0; i < 11u; i++)
            fs->label[i] = (char)lab[i];
        fs->label[11] = 0;
        for (i = 11u; i-- > 0;) {
            if (fs->label[i] == ' ')
                fs->label[i] = 0;
            else
                break;
        }
    }
    return SL6806_FAT_OK;
}

int sl6806_fat_mount(sl6806_fat_t *fs, sl6806_fat_read_fn read, void *ctx)
{
    unsigned p;
    int err;

    if (!fs || !read)
        return SL6806_FAT_ERR_PARAM;

    fs->read = read;
    fs->ctx = ctx;

    err = fat_read(fs, 0, g_sector);
    if (err != SL6806_FAT_OK)
        return err;

    if (g_sector[510] != 0x55 || g_sector[511] != 0xAA)
        return SL6806_FAT_ERR_NOFS;

    if (looks_like_bpb(g_sector))
        return parse_bpb(fs, g_sector, 0);

    /* An MBR: four sixteen-byte entries at 446. Take the first one with a
     * non-zero size whose boot sector actually parses - a type byte is a
     * hint, and cards come formatted with all sorts of them. */
    for (p = 0; p < 4u; p++) {
        const uint8_t *e = g_sector + 446u + p * 16u;
        uint32_t start = RD32(e, 8);
        uint32_t count = RD32(e, 12);

        if (count == 0u || start == 0u)
            continue;

        err = fat_read(fs, start, g_sector);
        if (err != SL6806_FAT_OK)
            return err;
        if (g_sector[510] != 0x55 || g_sector[511] != 0xAA)
            continue;
        if (!looks_like_bpb(g_sector))
            continue;
        return parse_bpb(fs, g_sector, start);
    }
    return SL6806_FAT_ERR_NOFS;
}

uint32_t sl6806_fat_root(const sl6806_fat_t *fs)
{
    return (fs->type == 32u) ? fs->root_cluster : 0u;
}

/* ------------------------------------------------------------------ */
/* Cluster chains                                                      */
/* ------------------------------------------------------------------ */

int32_t sl6806_fat_next_cluster(const sl6806_fat_t *fs, uint32_t cluster)
{
    uint32_t per_sector, lba, next;
    unsigned off;
    int err;

    if (cluster < 2u || cluster >= fs->clusters + 2u)
        return 0;

    if (fs->type == 32u) {
        per_sector = SL6806_FAT_SECTOR / 4u;
        lba = fs->fat_lba + cluster / per_sector;
        off = (unsigned)(cluster % per_sector) * 4u;
    } else {
        per_sector = SL6806_FAT_SECTOR / 2u;
        lba = fs->fat_lba + cluster / per_sector;
        off = (unsigned)(cluster % per_sector) * 2u;
    }

    err = fat_read(fs, lba, g_sector);
    if (err != SL6806_FAT_OK)
        return err;

    if (fs->type == 32u)
        next = RD32(g_sector, off) & 0x0FFFFFFFu;   /* top four bits reserved */
    else
        next = RD16(g_sector, off);

    /* End-of-chain and bad-cluster markers are the top of the range. */
    if (fs->type == 32u ? (next >= 0x0FFFFFF8u) : (next >= 0xFFF8u))
        return 0;
    if (next < 2u || next >= fs->clusters + 2u)
        return 0;
    return (int32_t)next;
}

static uint32_t cluster_lba(const sl6806_fat_t *fs, uint32_t cluster)
{
    return fs->data_lba + (cluster - 2u) * fs->sectors_per_cluster;
}

/* ------------------------------------------------------------------ */
/* Directories                                                         */
/* ------------------------------------------------------------------ */

/* "NAME    EXT" -> "NAME.EXT". */
static void format_name(const uint8_t *raw, char *out)
{
    unsigned i, n = 0;

    for (i = 0; i < 8u && raw[i] != ' '; i++)
        out[n++] = (char)raw[i];
    if (raw[8] != ' ') {
        out[n++] = '.';
        for (i = 8u; i < 11u && raw[i] != ' '; i++)
            out[n++] = (char)raw[i];
    }
    out[n] = 0;
}

int sl6806_fat_readdir(const sl6806_fat_t *fs, uint32_t dir_cluster,
                       unsigned index, sl6806_fat_entry_t *out)
{
    uint32_t cluster = dir_cluster;
    uint32_t lba, limit;
    unsigned seen = 0, s, e;
    int err;

    if (!out)
        return SL6806_FAT_ERR_PARAM;

    /* FAT16's root is a flat run of sectors; everything else is a chain. */
    if (dir_cluster == 0u && fs->type == 16u) {
        lba = fs->root_lba;
        limit = ((uint32_t)fs->root_entries * 32u + SL6806_FAT_SECTOR - 1u)
                / SL6806_FAT_SECTOR;
    } else {
        if (cluster < 2u)
            return SL6806_FAT_ERR_PARAM;
        lba = cluster_lba(fs, cluster);
        limit = fs->sectors_per_cluster;
    }

    for (;;) {
        for (s = 0; s < limit; s++) {
            err = fat_read(fs, lba + s, g_sector);
            if (err != SL6806_FAT_OK)
                return err;

            for (e = 0; e < SL6806_FAT_SECTOR / 32u; e++) {
                const uint8_t *d = g_sector + e * 32u;
                uint8_t attr = d[11];

                if (d[0] == 0x00)
                    return SL6806_FAT_ERR_END;   /* no entry ever follows */
                if (d[0] == 0xE5)
                    continue;                    /* deleted */
                if ((attr & SL6806_FAT_ATTR_LFN) == SL6806_FAT_ATTR_LFN)
                    continue;                    /* a long-name fragment */
                if (attr & SL6806_FAT_ATTR_LABEL)
                    continue;                    /* the volume label */

                if (seen++ != index)
                    continue;

                format_name(d, out->name);
                out->attr = attr;
                out->size = RD32(d, 28);
                out->cluster = ((uint32_t)RD16(d, 20) << 16) | RD16(d, 26);
                return SL6806_FAT_OK;
            }
        }

        /* Off the end of this cluster: follow the chain, if there is one. */
        if (dir_cluster == 0u && fs->type == 16u)
            return SL6806_FAT_ERR_END;

        {
            int32_t next = sl6806_fat_next_cluster(fs, cluster);

            if (next < 0)
                return (int)next;
            if (next == 0)
                return SL6806_FAT_ERR_END;
            cluster = (uint32_t)next;
            lba = cluster_lba(fs, cluster);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Files                                                               */
/* ------------------------------------------------------------------ */

int32_t sl6806_fat_read(const sl6806_fat_t *fs, uint32_t cluster,
                        uint32_t offset, void *buf, uint32_t len)
{
    uint8_t *dst = (uint8_t *)buf;
    uint32_t cluster_bytes, done = 0;
    int err;

    if (!buf || cluster < 2u)
        return SL6806_FAT_ERR_PARAM;

    cluster_bytes = (uint32_t)fs->sectors_per_cluster * SL6806_FAT_SECTOR;

    /* Walk the chain to the cluster the offset lands in. */
    while (offset >= cluster_bytes) {
        int32_t next = sl6806_fat_next_cluster(fs, cluster);

        if (next < 0)
            return next;
        if (next == 0)
            return 0;                        /* offset past the end */
        cluster = (uint32_t)next;
        offset -= cluster_bytes;
    }

    while (len > 0u) {
        uint32_t in_cluster = offset;
        uint32_t sector = in_cluster / SL6806_FAT_SECTOR;
        uint32_t in_sector = in_cluster % SL6806_FAT_SECTOR;
        uint32_t chunk = SL6806_FAT_SECTOR - in_sector;
        uint32_t i;

        if (chunk > len)
            chunk = len;

        err = fat_read(fs, cluster_lba(fs, cluster) + sector, g_sector);
        if (err != SL6806_FAT_OK)
            return err;

        for (i = 0; i < chunk; i++)
            dst[done + i] = g_sector[in_sector + i];

        done += chunk;
        len -= chunk;
        offset += chunk;

        if (len > 0u && offset >= cluster_bytes) {
            int32_t next = sl6806_fat_next_cluster(fs, cluster);

            if (next < 0)
                return next;
            if (next == 0)
                break;                       /* short read at end of chain */
            cluster = (uint32_t)next;
            offset = 0;
        }
    }
    return (int32_t)done;
}
