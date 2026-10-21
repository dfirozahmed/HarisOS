/* Test helper: builds a clean NTFS volume that a driver may write to: $MFT with a bitmap (resident or not),
 * $MFTMirr, a clean $LogFile (two restart pages), $UpCase, $Bitmap, $Volume, a root folder whose index is a two-level
 * tree, and a few files that were "there before" (resident, non-resident, compressed, sparse, with an attribute
 * list, with a DOS name, a folder, a name with a non-ASCII letter). 16 MiB, 4 KiB clusters, 1 KiB records.
 * wb_build(mbm_nonres) fills WB_BASE (the image, NT_SECTORS * NT_SS bytes) and WB_UPC (what $UpCase holds).
 * Needs ntfsimg.h included first. Used by test_ntfsw.c and test_volume.c. */
#ifndef HARIS_TEST_NTFSWIMG_H
#define HARIS_TEST_NTFSWIMG_H

#define WB_REC_N 2048
#define WB_MFT_CL (WB_REC_N * NT_REC / NT_CS)
#define WB_T0 0x01D9000000000000ULL

/* ---- the volume ---- */
static NImg WB;
static unsigned short WB_UPC[65536];
static unsigned char *WB_BASE;           /* the freshly built image */
static unsigned wb_lcn_log, wb_lcn_up, wb_lcn_mirr, wb_lcn_bm, wb_lcn_mbm;

static unsigned char wb_pat(unsigned seed, unsigned i) { return (unsigned char)((i * 29 + seed * 11 + (i >> 8) * 3) & 0xFF); }
static void wb_fill(unsigned char *p, unsigned n, unsigned seed) { for (unsigned i = 0; i < n; i++) p[i] = wb_pat(seed, i); }
static unsigned long long wb_ref(unsigned n) { return (unsigned long long)n | (1ULL << 48); }

static void wb_si(NRec *r, unsigned flags, unsigned sid)
{
    unsigned char si[72];
    memset(si, 0, sizeof si);
    for (int i = 0; i < 4; i++) np64(si + 8 * i, WB_T0);
    np32(si + 32, flags);
    np32(si + 52, sid);
    nrec_res(r, 0x10, 0, si, sizeof si);
}

static void wb_file_res(unsigned recno, const char *name, const unsigned char *data, unsigned len, unsigned long long parent, const char *dos)
{
    NRec r;
    nrec_begin(&r, recno, 1, 0);
    wb_si(&r, 0x20, 0x101);
    NChild c = {name, recno, 0, len, 1};
    nfilename_attrs(&r, parent, &c, dos);
    nrec_res(&r, 0x80, 0, data, len);
    nrec_finish(&WB, &r);
}

static void wb_system(unsigned recno, const char *name, const NRunSpec *run, unsigned long long real)
{
    NRec r;
    nrec_begin(&r, recno, 1, 0);
    wb_si(&r, 0x6, 0);
    NChild c = {name, recno, 0, 0, 1};
    nfilename_attrs(&r, wb_ref(5), &c, 0);
    if (run) nrec_nonres(&r, 0x80, 0, run, 1, 0, run->len * NT_CS, real, real, 0);
    else nrec_res(&r, 0x80, 0, 0, 0);
    nrec_finish(&WB, &r);
}

static void wb_upcase(void)
{
    for (unsigned i = 0; i < 65536; i++) WB_UPC[i] = (unsigned short)i;
    for (unsigned c = 'a'; c <= 'z'; c++) WB_UPC[c] = (unsigned short)(c - 32);
    for (unsigned c = 0xE0; c <= 0xFE; c++) if (c != 0xF7) WB_UPC[c] = (unsigned short)(c - 32);
    WB_UPC[0xFF] = 0x178;
}

static void wb_build(int mbm_nonres)
{
    free(WB.img); free(WB.mft);
    memset(&WB, 0, sizeof WB);
    WB.img = calloc(NT_SECTORS, NT_SS);
    WB.mft = calloc(WB_REC_N, NT_REC);
    wb_upcase();
    nboot(&WB, 0);
    nmark(&WB, 0, 4);
    WB.next_lcn = 4;
    unsigned lcn_mft = nalloc(&WB, WB_MFT_CL);
    wb_lcn_mirr = nalloc(&WB, 1);
    wb_lcn_log = nalloc(&WB, 8);
    wb_lcn_up = nalloc(&WB, 32);
    wb_lcn_bm = nalloc(&WB, 1);
    wb_lcn_mbm = mbm_nonres ? nalloc(&WB, 1) : 0;
    np64(WB.img + 56, wb_lcn_mirr);

    /* system records 1..11 (0 and 6 are done last) */
    NRunSpec mirr = {1, wb_lcn_mirr}, logr = {8, wb_lcn_log}, upr = {32, wb_lcn_up};
    wb_system(1, "$MFTMirr", &mirr, 4 * NT_REC);
    wb_system(2, "$LogFile", &logr, 8 * NT_CS);
    NRec r;
    nrec_begin(&r, 3, 1, 0);
    wb_si(&r, 0x6, 0);
    NChild volc = {"$Volume", 3, 0, 0, 1};
    nfilename_attrs(&r, wb_ref(5), &volc, 0);
    unsigned char label[12];
    for (int i = 0; i < 6; i++) np16(label + 2 * i, (unsigned char)"NTTEST"[i]);
    nrec_res(&r, 0x60, 0, label, 12);
    unsigned char vi[12] = {0}; vi[8] = 3; vi[9] = 1;
    nrec_res(&r, 0x70, 0, vi, 12);
    nrec_res(&r, 0x80, 0, 0, 0);
    nrec_finish(&WB, &r);
    wb_system(4, "$AttrDef", 0, 0);
    wb_system(7, "$Boot", 0, 0);
    wb_system(8, "$BadClus", 0, 0);
    wb_system(9, "$Secure", 0, 0);
    wb_system(10, "$UpCase", &upr, 65536 * 2);
    {   /* $Extend: an empty folder */
        NRec d;
        nrec_begin(&d, 11, 3, 0);
        wb_si(&d, 0x6 | 0x10, 0);
        NChild c = {"$Extend", 11, 1, 0, 1};
        nfilename_attrs(&d, wb_ref(5), &c, 0);
        nindex(&WB, &d, 0, 0, wb_ref(11), 0);
        nrec_finish(&WB, &d);
    }

    /* files that were there before */
    unsigned char buf[40000];
    wb_fill(buf, 300, 1); wb_file_res(24, "hello.txt", buf, 300, wb_ref(5), 0);
    {
        unsigned lcn = nalloc(&WB, 3);
        wb_fill(buf, 10000, 2);
        for (int i = 0; i < 3; i++) memcpy(ncl(&WB, lcn + (unsigned)i), buf + i * NT_CS, i < 2 ? NT_CS : 10000 - 2 * NT_CS);
        NRec f;
        nrec_begin(&f, 25, 1, 0);
        wb_si(&f, 0x20, 0x101);
        NChild c = {"big.bin", 25, 0, 10000, 1};
        nfilename_attrs(&f, wb_ref(5), &c, 0);
        NRunSpec run = {3, lcn};
        nrec_nonres(&f, 0x80, 0, &run, 1, 0, 3ULL * NT_CS, 10000, 10000, 0);
        nrec_finish(&WB, &f);
    }
    {   /* Docs with note.txt */
        NRec d;
        nrec_begin(&d, 26, 3, 0);
        wb_si(&d, 0x10, 0x101);
        NChild c = {"Docs", 26, 1, 0, 1};
        nfilename_attrs(&d, wb_ref(5), &c, 0);
        wb_fill(buf, 50, 3); wb_file_res(27, "note.txt", buf, 50, wb_ref(26), 0);
        NChild k[1] = {{"note.txt", 27, 0, 50, 1}};
        nindex(&WB, &d, k, 1, wb_ref(26), 0);
        nrec_finish(&WB, &d);
    }
    wb_fill(buf, 10, 4); wb_file_res(28, "caf\x01.txt", buf, 10, wb_ref(5), 0);
    {   /* compressed and sparse: writing must be refused */
        unsigned a = nalloc(&WB, 1), b = nalloc(&WB, 1);
        NRec f;
        nrec_begin(&f, 29, 1, 0); wb_si(&f, 0x20, 0x101);
        NChild c = {"compressed.dat", 29, 0, 3000, 1};
        nfilename_attrs(&f, wb_ref(5), &c, 0);
        NRunSpec run = {1, a};
        nrec_nonres(&f, 0x80, 0, &run, 1, 0, NT_CS, 3000, 3000, 0x0001);
        nrec_finish(&WB, &f);
        NRec g;
        nrec_begin(&g, 30, 1, 0); wb_si(&g, 0x20, 0x101);
        NChild c2 = {"sparse.dat", 30, 0, 3 * NT_CS, 1};
        nfilename_attrs(&g, wb_ref(5), &c2, 0);
        NRunSpec runs[2] = {{1, b}, {2, -1}};
        nrec_nonres(&g, 0x80, 0, runs, 2, 0, 3ULL * NT_CS, 3 * NT_CS, 3 * NT_CS, 0x8000);
        nrec_finish(&WB, &g);
    }
    {   /* an attribute list with extension records (31 base, 32, 33) */
        unsigned a = nalloc(&WB, 2), b = nalloc(&WB, 1);
        NRunSpec e1[1] = {{2, a}}, e2[1] = {{1, b}};
        NRec base, x1, x2;
        nrec_begin(&base, 31, 1, 0); wb_si(&base, 0x20, 0x101);
        NChild c = {"attrlist.bin", 31, 0, 3 * NT_CS, 1};
        unsigned char list[4 * 32];
        memset(list, 0, sizeof list);
        struct { unsigned type, vcn, rec, id; } L[4] = {{0x10, 0, 31, 0}, {0x30, 0, 31, 1}, {0x80, 0, 32, 0}, {0x80, 2, 33, 0}};
        for (int i = 0; i < 4; i++) {
            unsigned char *e = list + i * 32;
            np32(e, L[i].type); np16(e + 4, 32); e[7] = 26; np64(e + 8, L[i].vcn); np64(e + 16, wb_ref(L[i].rec)); np16(e + 24, L[i].id);
        }
        nrec_res(&base, 0x20, 0, list, sizeof list);
        nfilename_attrs(&base, wb_ref(5), &c, 0);
        nrec_finish(&WB, &base);
        nrec_begin(&x1, 32, 1, wb_ref(31));
        nrec_nonres(&x1, 0x80, 0, e1, 1, 0, 3ULL * NT_CS, 3 * NT_CS, 3 * NT_CS, 0);
        nrec_finish(&WB, &x1);
        nrec_begin(&x2, 33, 1, wb_ref(31));
        nrec_nonres(&x2, 0x80, 0, e2, 1, 2, 0, 0, 0, 0);
        nrec_finish(&WB, &x2);
    }
    wb_fill(buf, 20, 5); wb_file_res(34, "Long File Name.txt", buf, 20, wb_ref(5), "LONGFI~1.TXT");

    /* the root folder: every name of the system files and of the files above */
    {
        static NChild kids[40];
        int nk = 0;
        static const char *const sys[] = {"$MFT", "$MFTMirr", "$LogFile", "$Volume", "$AttrDef", ".", "$Bitmap", "$Boot", "$BadClus", "$Secure", "$UpCase", "$Extend"};
        for (unsigned i = 0; i < 12; i++) kids[nk++] = (NChild){sys[i], i, i == 5 || i == 11, 0, 1};
        kids[nk++] = (NChild){"hello.txt", 24, 0, 300, 1};
        kids[nk++] = (NChild){"big.bin", 25, 0, 10000, 1};
        kids[nk++] = (NChild){"Docs", 26, 1, 0, 1};
        kids[nk++] = (NChild){"caf\x01.txt", 28, 0, 10, 1};
        kids[nk++] = (NChild){"compressed.dat", 29, 0, 3000, 1};
        kids[nk++] = (NChild){"sparse.dat", 30, 0, 3 * NT_CS, 1};
        kids[nk++] = (NChild){"attrlist.bin", 31, 0, 3 * NT_CS, 1};
        kids[nk++] = (NChild){"Long File Name.txt", 34, 0, 20, 1};
        kids[nk++] = (NChild){"LONGFI~1.TXT", 34, 0, 20, 2};
        NRec d;
        nrec_begin(&d, 5, 3, 0);
        wb_si(&d, 0x6 | 0x10, 0x101);
        NChild c = {".", 5, 1, 0, 1};
        nfilename_attrs(&d, wb_ref(5), &c, 0);
        nindex(&WB, &d, kids, nk, wb_ref(5), 0);
        nrec_finish(&WB, &d);
    }

    /* $Bitmap (6) and $MFT (0) last: they describe everything above */
    {
        NRec bm;
        nrec_begin(&bm, 6, 1, 0);
        wb_si(&bm, 0x6, 0);
        NChild c = {"$Bitmap", 6, 0, 0, 1};
        nfilename_attrs(&bm, wb_ref(5), &c, 0);
        NRunSpec run = {1, wb_lcn_bm};
        nrec_nonres(&bm, 0x80, 0, &run, 1, 0, NT_CS, (NT_CLUSTERS - 1 + 7) / 8, (NT_CLUSTERS - 1 + 7) / 8, 0);
        nrec_finish(&WB, &bm);
    }
    {
        NRec m;
        nrec_begin(&m, 0, 1, 0);
        wb_si(&m, 0x6, 0);
        NChild c = {"$MFT", 0, 0, 0, 1};
        nfilename_attrs(&m, wb_ref(5), &c, 0);
        NRunSpec run = {WB_MFT_CL, lcn_mft};
        nrec_nonres(&m, 0x80, 0, &run, 1, 0, (unsigned long long)WB_MFT_CL * NT_CS, (unsigned long long)WB_REC_N * NT_REC, (unsigned long long)WB_REC_N * NT_REC, 0);
        unsigned char mbm[WB_REC_N / 8];
        memset(mbm, 0, sizeof mbm);
        for (unsigned i = 0; i < WB_REC_N; i++)
            if (i < 24 || (memcmp(WB.mft + (size_t)i * NT_REC, "FILE", 4) == 0 && (WB.mft[(size_t)i * NT_REC + 22] & 1))) mbm[i / 8] |= (unsigned char)(1u << (i % 8));
        if (mbm_nonres) {
            NRunSpec br = {1, wb_lcn_mbm};
            nrec_nonres(&m, 0xB0, 0, &br, 1, 0, NT_CS, sizeof mbm, sizeof mbm, 0);
            memcpy(ncl(&WB, wb_lcn_mbm), mbm, sizeof mbm);
        } else nrec_res(&m, 0xB0, 0, mbm, sizeof mbm);
        nrec_finish(&WB, &m);
    }
    /* clusters of the system files */
    {
        unsigned char *lg = ncl(&WB, wb_lcn_log);
        memset(lg, 0xFF, 8 * NT_CS);
        for (int page = 0; page < 2; page++) {
            unsigned char *p = lg + page * 4096;
            memset(p, 0, 4096);
            memcpy(p, "RSTR", 4);
            np16(p + 4, 0x1E); np16(p + 6, 9);
            np32(p + 16, 4096); np32(p + 20, 4096);
            np16(p + 0x18, 0x30);
            np16(p + 0x1A, 1); np16(p + 0x1C, 1);        /* version 1.1 */
            np64(p + 0x30, 0x1234 + (unsigned)page);     /* current LSN */
            np16(p + 0x38, 1); np16(p + 0x3A, 0xFFFF); np16(p + 0x3C, 0); np16(p + 0x3E, 0x0002); /* clients, free, in use, flags: clean */
        }
        unsigned char *up = ncl(&WB, wb_lcn_up);
        for (unsigned i = 0; i < 65536; i++) np16(up + 2 * i, WB_UPC[i]);
        memcpy(ncl(&WB, wb_lcn_bm), WB.bitmap, sizeof WB.bitmap);
        for (unsigned i = (NT_CLUSTERS - 1); i < 8 * sizeof WB.bitmap; i++) ncl(&WB, wb_lcn_bm)[i / 8] |= (unsigned char)(1u << (i % 8));
        memcpy(ncl(&WB, wb_lcn_mirr), WB.mft, 4 * NT_REC);
    }
    memcpy(WB.img + (size_t)lcn_mft * NT_CS, WB.mft, (size_t)WB_REC_N * NT_REC);
    memcpy(WB.img + (size_t)(NT_SECTORS - 1) * NT_SS, WB.img, NT_SS);
    free(WB_BASE);
    WB_BASE = malloc((size_t)NT_SECTORS * NT_SS);
    memcpy(WB_BASE, WB.img, (size_t)NT_SECTORS * NT_SS);
}


#endif
