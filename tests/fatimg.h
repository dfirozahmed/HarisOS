/* Test helpers: a FAT32 image in RAM as a block device, a formatter, and an independent consistency checker.
 * Included by test_fat32.c and test_volume.c. */

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

#define SS 512
static unsigned char *DISK;
static unsigned long long NSEC;
static unsigned long long WRITES;

/* the disk's memory is in d->ctx (so a test can have several images); DISK is the one the checker looks at */
static inline int rd(BlkDev *d, unsigned long long lba, unsigned n, void *buf)
{
    memcpy(buf, (unsigned char *)d->ctx + lba * SS, (size_t)n * SS);
    return 0;
}

static inline int wr(BlkDev *d, unsigned long long lba, unsigned n, const void *buf)
{
    memcpy((unsigned char *)d->ctx + lba * SS, buf, (size_t)n * SS);
    WRITES++;
    return 0;
}

static inline void put16(unsigned char *p, unsigned v) { p[0] = v; p[1] = v >> 8; }
static inline void put32(unsigned char *p, unsigned v) { put16(p, v); put16(p + 2, v >> 16); }
static inline unsigned get16(const unsigned char *p) { return p[0] | p[1] << 8; }
static inline unsigned get32(const unsigned char *p) { return get16(p) | (unsigned)get16(p + 2) << 16; }

/* ---- mkfs ---- */
static unsigned SPC, RESERVED = 32, FATSZ, NCLUSTERS, ROOTC = 2;

static inline void mkfs(unsigned long long sectors, unsigned spc)
{
    free(DISK);
    NSEC = sectors;
    DISK = calloc((size_t)sectors, SS);
    SPC = spc;
    unsigned clusters = (unsigned)((sectors - RESERVED) / spc);
    FATSZ = ((clusters + 2) * 4 + SS - 1) / SS;
    clusters = (unsigned)((sectors - RESERVED - 2 * FATSZ) / spc);
    NCLUSTERS = clusters;
    unsigned char *b = DISK;
    b[0] = 0xEB; b[1] = 0x58; b[2] = 0x90;
    memcpy(b + 3, "MSWIN4.1", 8);
    put16(b + 11, SS); b[13] = (unsigned char)spc; put16(b + 14, RESERVED); b[16] = 2;
    b[21] = 0xF8; put16(b + 24, 63); put16(b + 26, 255);
    put32(b + 32, (unsigned)sectors); put32(b + 36, FATSZ);
    put32(b + 44, 2); put16(b + 48, 1); put16(b + 50, 6);
    b[66] = 0x29; put32(b + 67, 0x12345678);
    memcpy(b + 71, "TESTVOL    ", 11);
    memcpy(b + 82, "FAT32   ", 8);
    b[510] = 0x55; b[511] = 0xAA;
    unsigned char *fs = DISK + SS;
    put32(fs, 0x41615252); put32(fs + 484, 0x61417272);
    put32(fs + 488, clusters - 1); put32(fs + 492, 3); /* the root folder uses cluster 2 */
    put32(fs + 508, 0xAA550000);
    for (int k = 0; k < 2; k++) {
        unsigned char *fat = DISK + ((size_t)RESERVED + (size_t)k * FATSZ) * SS;
        put32(fat, 0x0FFFFFF8); put32(fat + 4, 0x0FFFFFFF); put32(fat + 8, 0x0FFFFFFF);
    }
    memcpy(DISK + 6 * SS, DISK, SS); /* backup boot sector */
}

static int disk_idx;
static Fat F;

static inline void new_volume(unsigned long long sectors, unsigned spc)
{
    mkfs(sectors, spc);
    blk_reset();
    BlkDev d;
    memset(&d, 0, sizeof d);
    strcpy(d.name, "ram0");
    d.kind = BLK_RAM; d.parent = -1; d.sector_size = SS; d.sectors = NSEC; d.writable = 1; d.read = rd; d.write = wr;
    d.ctx = DISK;
    disk_idx = blk_register(&d);
    blk_set_locked(disk_idx, 0);
    CHECK(fat_mount(&F, disk_idx) == 0);
}

/* ---- the independent checker ---- */
static unsigned char *SEEN;
static int fsck_errors;
#define BAD(...) do { printf("  fsck: " __VA_ARGS__); printf("\n"); fsck_errors++; } while (0)

static inline unsigned fat_entry(unsigned c) { return get32(DISK + ((size_t)RESERVED) * SS + (size_t)c * 4) & 0x0FFFFFFF; }
static inline unsigned fat_entry2(unsigned c) { return get32(DISK + ((size_t)RESERVED + FATSZ) * SS + (size_t)c * 4) & 0x0FFFFFFF; }
static inline unsigned char *clus(unsigned c) { return DISK + ((size_t)RESERVED + 2 * FATSZ + (size_t)(c - 2) * SPC) * SS; }
static inline unsigned csize(void) { return SPC * SS; }

static inline unsigned chain_len(unsigned first, const char *what)
{
    unsigned n = 0, c = first;
    while (c >= 2 && c < 0x0FFFFFF8) {
        if (c >= NCLUSTERS + 2) { BAD("%s: cluster %u out of range", what, c); return n; }
        if (SEEN[c]) { BAD("%s: cluster %u used twice", what, c); return n; }
        SEEN[c] = 1;
        n++;
        c = fat_entry(c);
        if (n > NCLUSTERS) { BAD("%s: loop", what); return n; }
    }
    if (c == 0 && first) BAD("%s: chain ends in a free cluster", what);
    return n;
}

static inline void check_dir(unsigned first, unsigned parent, const char *path, int depth)
{
    unsigned n = chain_len(first, path);
    unsigned total = n * csize() / 32;
    char lfn[300];
    int expect = 0, ok = 0;
    unsigned char sum = 0;
    char seen_names[2048][16];
    int nseen = 0;
    unsigned idx = 0;
    /* collect the chain */
    unsigned cl = first;
    unsigned char *base[4096];
    for (unsigned i = 0; i < n; i++) { base[i] = clus(cl); cl = fat_entry(cl); }
    int ended = 0;
    for (; idx < total; idx++) {
        const unsigned char *e = base[idx / (csize() / 32)] + (idx % (csize() / 32)) * 32;
        if (e[0] == 0) { ended = 1; continue; }
        if (ended) { BAD("%s: entry after the end marker", path); break; }
        if (e[0] == 0xE5) { ok = 0; continue; }
        if (e[11] == 0x0F) {
            int seq = e[0] & 0x1F;
            if (e[0] & 0x40) { ok = 1; expect = seq; sum = e[13]; memset(lfn, 0, sizeof lfn); }
            else if (ok && seq == expect - 1 && e[13] == sum) expect = seq;
            else { BAD("%s: broken long-name sequence at %u", path, idx); ok = 0; }
            continue;
        }
        char sname[16];
        memcpy(sname, e, 11); sname[11] = 0;
        if (ok) {
            unsigned char s = 0;
            for (int i = 0; i < 11; i++) s = (unsigned char)(((s & 1) << 7) + (s >> 1) + e[i]);
            if (expect != 1 || s != sum) BAD("%s: long name does not match short name %s", path, sname);
        }
        ok = 0;
        for (int i = 0; i < nseen; i++) if (memcmp(seen_names[i], sname, 11) == 0) BAD("%s: duplicate short name %s", path, sname);
        if (nseen < 2048) { memcpy(seen_names[nseen], sname, 11); nseen++; }
        unsigned cluster = get16(e + 20) << 16 | get16(e + 26), size = get32(e + 28);
        if (e[11] & 0x08) continue;
        if (e[0] == '.') {
            if (idx == 0 && cluster != first) BAD("%s: '.' points to %u, not %u", path, cluster, first);
            if (idx == 1 && cluster != parent) BAD("%s: '..' points to %u, not %u", path, cluster, parent);
            continue;
        }
        char sub[400];
        snprintf(sub, sizeof sub, "%s/%.11s", path, sname);
        if (e[11] & 0x10) {
            if (size != 0) BAD("%s: folder with size", sub);
            if (depth < 20) check_dir(cluster, first == ROOTC ? 0 : first, sub, depth + 1);
        } else {
            unsigned len = chain_len(cluster, sub);
            unsigned want = (size + csize() - 1) / csize();
            if (len != want) BAD("%s: %u clusters for %u bytes (want %u)", sub, len, size, want);
            if (size == 0 && cluster != 0) BAD("%s: empty file owns cluster %u", sub, cluster);
        }
    }
}

static inline int fsck(void)
{
    fsck_errors = 0;
    SEEN = calloc(NCLUSTERS + 2, 1);
    for (unsigned c = 0; c < NCLUSTERS + 2; c++)
        if (fat_entry(c) != fat_entry2(c)) { BAD("FAT copies differ at %u", c); break; }
    SEEN[0] = SEEN[1] = 1;
    check_dir(ROOTC, 0, "", 0);
    unsigned freec = 0;
    for (unsigned c = 2; c < NCLUSTERS + 2; c++) {
        if (fat_entry(c) != 0) { if (!SEEN[c]) { BAD("cluster %u is in use but nobody owns it", c); break; } }
        else freec++;
    }
    unsigned fsi = get32(DISK + SS + 488);
    if (fsi != 0xFFFFFFFF && fsi != freec) BAD("FSInfo says %u free clusters, there are %u", fsi, freec);
    free(SEEN);
    return fsck_errors;
}


/* The checker on a volume that starts at sector `base_lba` of the image in DISK (a partition): its sizes
 * come from the boot sector, not from mkfs(). */
static inline int fsck_volume_at(unsigned long long base_lba)
{
    unsigned char *save = DISK;
    DISK = save + base_lba * SS;
    SPC = DISK[13];
    RESERVED = get16(DISK + 14);
    FATSZ = get32(DISK + 36);
    ROOTC = get32(DISK + 44);
    NCLUSTERS = (get32(DISK + 32) - RESERVED - DISK[16] * FATSZ) / SPC;
    int r = fsck();
    DISK = save;
    return r;
}
