/* Host test of the Realtek Bluetooth firmware handling (kernel/bluetooth/bt_rtl.c): the file parser and the 0xFC20 download.
 * A synthetic firmware file is always tested; if the real linux-firmware files are given (argv[1], argv[2]) they are tested too. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bt_rtl.h"

static int failures, checks;
#define CHECK(c)                                                  \
    do                                                            \
    {                                                             \
        checks++;                                                 \
        if (!(c))                                                 \
        {                                                         \
            failures++;                                           \
            printf("FAIL line %d: %s\n", __LINE__, #c);           \
        }                                                         \
    } while (0)

/* ---- a fake chip that records the download ---- */
static struct
{
    unsigned char got[200000];
    unsigned ngot;
    unsigned char idx[400];
    unsigned plen[400];
    unsigned nfrag;
    int fail_at;   /* fragment number that is answered with fail_status (-1 = never) */
    int fail_status;
    int short_answer;
    unsigned char pend[270];
    int have;
} F;

static void cc(unsigned op, const unsigned char *ret, int rl)
{
    F.pend[0] = 0x0E;
    F.pend[1] = (unsigned char)(3 + rl);
    F.pend[2] = 1;
    F.pend[3] = (unsigned char)op;
    F.pend[4] = (unsigned char)(op >> 8);
    memcpy(F.pend + 5, ret, (size_t)rl);
    F.have = 5 + rl;
}

static int fake_send(void *ctx, const unsigned char *pkt, unsigned len)
{
    (void)ctx;
    unsigned op = pkt[0] | ((unsigned)pkt[1] << 8);
    if (op == 0xFC20)
    {
        unsigned n = pkt[2];
        if (len != 3 + n || n < 1)
            return -1;
        if (F.nfrag < 400)
        {
            F.idx[F.nfrag] = pkt[3];
            F.plen[F.nfrag] = n;
        }
        if ((int)F.nfrag == F.fail_at)
        {
            unsigned char r[2] = {(unsigned char)F.fail_status, pkt[3]};
            cc(op, r, 2);
        }
        else if (F.short_answer)
        {
            unsigned char r[1] = {0};
            cc(op, r, 1);
        }
        else
        {
            unsigned char r[2] = {0, pkt[3]};
            cc(op, r, 2);
        }
        if (F.ngot + n - 1 <= sizeof F.got)
        {
            memcpy(F.got + F.ngot, pkt + 4, n - 1);
            F.ngot += n - 1;
        }
        F.nfrag++;
    }
    else if (op == 0x0C03)
    {
        unsigned char r[1] = {0};
        cc(op, r, 1);
    }
    else if (op == 0x1001)
    {
        const unsigned char r[9] = {0, 8, 0x0C, 0, 8, 0x5D, 0, 0x21, 0x88};
        cc(op, r, 9);
    }
    else if (op == 0xFC6D)
    {
        const unsigned char r[2] = {0, 1};
        cc(op, r, 2);
    }
    else
    {
        const unsigned char r[7] = {0, 1, 2, 3, 4, 5, 6};
        cc(op, r, 7);
    }
    return 0;
}

static void fake_wait(void *ctx)
{
    (void)ctx;
    if (F.have)
    {
        bt_rx_event(F.pend, (unsigned)F.have);
        F.have = 0;
    }
}

static int fake_alive(void *ctx)
{
    (void)ctx;
    return 1;
}

static const BtTransport T = {fake_send, fake_wait, fake_alive, 0, 0};

/* ---- a synthetic firmware file in the real layout ---- */
static unsigned put16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    return 2;
}

static unsigned put32(unsigned char *p, unsigned v)
{
    put16(p, v);
    put16(p + 2, v >> 16);
    return 4;
}

/* two patches (chip ids 1 and 2) of the given sizes, filled with 0x11.. / 0x22.., project id `project` */
static unsigned make_fw(unsigned char *fw, unsigned len1, unsigned len2, int project)
{
    unsigned n = 0;
    memcpy(fw, "Realtech", 8);
    n = 8;
    n += put32(fw + n, 0xAABBCCDD); /* fw_version */
    n += put16(fw + n, 2);
    n += put16(fw + n, 1);
    n += put16(fw + n, 2);
    n += put16(fw + n, len1);
    n += put16(fw + n, len2);
    unsigned off1 = 48, off2 = 48 + len1 + 100; /* a gap between them, like the real file has */
    n += put32(fw + n, off1);
    n += put32(fw + n, off2);
    memset(fw + n, 0, off1 - n);
    memset(fw + off1, 0x11, len1);
    memset(fw + off1 + len1, 0x00, 100);
    memset(fw + off2, 0x22, len2);
    n = off2 + len2;
    fw[n++] = 0xFF;
    fw[n++] = (unsigned char)project;
    fw[n++] = 1;
    fw[n++] = 0;
    fw[n++] = 0x51;
    fw[n++] = 0x04;
    fw[n++] = 0xFD;
    fw[n++] = 0x77;
    return n;
}

static void test_build(const unsigned char *fw, unsigned fwl, const unsigned char *cfg, unsigned cl, unsigned len1, unsigned len2, unsigned ver,
                       unsigned off1, unsigned off2, int is_real)
{
    static unsigned char out[100000];
    const char *why;
    unsigned v = 0;

    /* ROM version 1 -> chip id 2 -> the second patch, config appended, last 4 bytes of the patch = file version */
    unsigned n = bt_rtl_build(fw, fwl, cfg, cl, 0x8821, 1, out, sizeof out, &v, &why);
    CHECK(n == len2 + cl);
    CHECK(v == ver);
    if (n == len2 + cl)
    {
        CHECK(memcmp(out, fw + off2, len2 - 4) == 0);
        CHECK(out[len2 - 4] == (ver & 0xFF) && out[len2 - 3] == ((ver >> 8) & 0xFF) && out[len2 - 2] == ((ver >> 16) & 0xFF) && out[len2 - 1] == (ver >> 24));
        CHECK(memcmp(out + len2, cfg, cl) == 0);
    }
    /* ROM version 0 -> chip id 1 -> the first patch */
    n = bt_rtl_build(fw, fwl, cfg, cl, 0x8821, 0, out, sizeof out, &v, &why);
    CHECK(n == len1 + cl && memcmp(out, fw + off1, len1 - 4) == 0);
    /* no config at all */
    n = bt_rtl_build(fw, fwl, 0, 0, 0x8821, 1, out, sizeof out, &v, &why);
    CHECK(n == len2);
    /* ROM versions the file has no patch for */
    CHECK(bt_rtl_build(fw, fwl, cfg, cl, 0x8821, 2, out, sizeof out, &v, &why) == 0 && strcmp(why, "no patch") == 0);
    CHECK(bt_rtl_build(fw, fwl, cfg, cl, 0x8821, 255, out, sizeof out, &v, &why) == 0 && strcmp(why, "no patch") == 0);
    /* wrong chip family */
    CHECK(bt_rtl_build(fw, fwl, cfg, cl, 0x8723, 1, out, sizeof out, &v, &why) == 0 && strcmp(why, "wrong chip") == 0);
    /* output buffer too small */
    CHECK(bt_rtl_build(fw, fwl, cfg, cl, 0x8821, 1, out, len2 + cl - 1, &v, &why) == 0 && strcmp(why, "too big") == 0);
    CHECK(bt_rtl_build(fw, fwl, cfg, cl, 0x8821, 1, out, len2 + cl, &v, &why) == len2 + cl);
    /* config too long */
    static unsigned char bigcfg[2000];
    CHECK(bt_rtl_build(fw, fwl, bigcfg, sizeof bigcfg, 0x8821, 1, out, sizeof out, &v, &why) == 0 && strcmp(why, "config size") == 0);

    /* damaged files */
    static unsigned char bad[100000];
    memcpy(bad, fw, fwl);
    bad[0] = 'X';
    CHECK(bt_rtl_build(bad, fwl, cfg, cl, 0x8821, 1, out, sizeof out, &v, &why) == 0 && strcmp(why, "signature") == 0);
    memcpy(bad, fw, fwl);
    bad[fwl - 1] ^= 1;
    CHECK(bt_rtl_build(bad, fwl, cfg, cl, 0x8821, 1, out, sizeof out, &v, &why) == 0 && strcmp(why, "ext sig") == 0);
    CHECK(bt_rtl_build(fw, 10, cfg, cl, 0x8821, 1, out, sizeof out, &v, &why) == 0);
    CHECK(bt_rtl_build(fw, 4, cfg, cl, 0x8821, 1, out, sizeof out, &v, &why) == 0 && strcmp(why, "short") == 0);
    memcpy(bad, fw, fwl);
    put32(bad + 22 + 4, 0x7FFFFFF0); /* second patch offset far outside the file */
    CHECK(bt_rtl_build(bad, fwl, cfg, cl, 0x8821, 1, out, sizeof out, &v, &why) == 0 && strcmp(why, "patch range") == 0);
    memcpy(bad, fw, fwl);
    put16(bad + 18 + 2, 0xFFFF); /* second patch longer than the file */
    CHECK(bt_rtl_build(bad, fwl, cfg, cl, 0x8821, 1, out, sizeof out, &v, &why) == 0 && strcmp(why, "patch range") == 0);
    memcpy(bad, fw, fwl);
    put16(bad + 12, 0); /* no patches */
    CHECK(bt_rtl_build(bad, fwl, cfg, cl, 0x8821, 1, out, sizeof out, &v, &why) == 0 && strcmp(why, "patch table") == 0);
    memcpy(bad, fw, fwl);
    put16(bad + 12, 60000); /* table does not fit */
    CHECK(bt_rtl_build(bad, fwl, cfg, cl, 0x8821, 1, out, sizeof out, &v, &why) == 0 && strcmp(why, "patch table") == 0);
    memcpy(bad, fw, fwl);
    bad[fwl - 7] = 9; /* project id 9 is not one of the 0x8821 chips */
    CHECK(bt_rtl_build(bad, fwl, cfg, cl, 0x8821, 1, out, sizeof out, &v, &why) == 0 && strcmp(why, "wrong chip") == 0);
    memcpy(bad, fw, fwl);
    bad[fwl - 6] = 0; /* extension entry with length 0 */
    CHECK(bt_rtl_build(bad, fwl, cfg, cl, 0x8821, 1, out, sizeof out, &v, &why) == 0);
    (void)is_real;
}

static void test_download(void)
{
    static unsigned char data[40000];
    for (unsigned i = 0; i < sizeof data; i++)
        data[i] = (unsigned char)(i * 7 + (i >> 8));
    unsigned done, total;

    bt_attach(&T, 0x0BDA, 0xB00A);

    /* 34926 bytes (what the real RTL8821C patch + config come to): 139 pieces, index 0..0x7f, then 1.., last with bit 7 */
    memset(&F, 0, sizeof F);
    F.fail_at = -1;
    CHECK(bt_rtl_download(data, 34926, &done, &total) == 0);
    CHECK(total == 139 && done == 139 && F.nfrag == 139);
    CHECK(F.ngot == 34926 && memcmp(F.got, data, 34926) == 0);
    int idx_ok = 1;
    for (unsigned i = 0; i < 138; i++)
    {
        unsigned want = i < 128 ? i : 1 + (i - 128);
        if (F.idx[i] != want || F.plen[i] != 253)
            idx_ok = 0;
    }
    CHECK(idx_ok);
    CHECK(F.idx[127] == 0x7F && F.idx[128] == 1);
    CHECK(F.idx[138] == (0x80 | 11) && F.plen[138] == 1 + 150);

    /* a length that is a multiple of 252: the last piece is empty (index byte only), as in Linux */
    memset(&F, 0, sizeof F);
    F.fail_at = -1;
    CHECK(bt_rtl_download(data, 252 * 3, &done, &total) == 0);
    CHECK(total == 4 && F.nfrag == 4 && F.plen[3] == 1 && F.idx[3] == (0x80 | 3) && F.ngot == 252 * 3);

    /* shorter than one piece */
    memset(&F, 0, sizeof F);
    F.fail_at = -1;
    CHECK(bt_rtl_download(data, 10, &done, &total) == 0 && total == 1 && F.idx[0] == 0x80 && F.plen[0] == 11);

    /* the chip refuses a piece */
    memset(&F, 0, sizeof F);
    F.fail_at = 5;
    F.fail_status = 0x0C;
    CHECK(bt_rtl_download(data, 34926, &done, &total) == 0x0C);
    CHECK(done == 5 && total == 139 && F.nfrag == 6);

    /* an answer without the index byte */
    memset(&F, 0, sizeof F);
    F.fail_at = -1;
    F.short_answer = 1;
    CHECK(bt_rtl_download(data, 1000, &done, &total) == BT_E_SHORT && done == 0);

    bt_detach();
}

static unsigned char *slurp(const char *path, unsigned *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    static unsigned char buf[2][200000];
    static int which;
    unsigned char *b = buf[which++ & 1];
    *len = (unsigned)fread(b, 1, 200000, f);
    fclose(f);
    return b;
}

int main(int argc, char **argv)
{
    /* needs_firmware */
    BtInfo bi;
    memset(&bi, 0, sizeof bi);
    bi.lmp_subver = 0x8821;
    bi.hci_rev = 0x000C;
    bi.hci_ver = 8;
    CHECK(bt_rtl_needs_firmware(&bi));
    bi.hci_rev = 0x1234; /* a patch is already running */
    CHECK(!bt_rtl_needs_firmware(&bi));

    /* synthetic file */
    static unsigned char fw[100000];
    unsigned fwl = make_fw(fw, 1000, 2000, 10);
    const unsigned char cfg[10] = {0x55, 0xab, 0x23, 0x87, 4, 0, 0xd9, 0, 1, 0x0f};
    test_build(fw, fwl, cfg, sizeof cfg, 1000, 2000, 0xAABBCCDD, 48, 48 + 1000 + 100, 0);
    fwl = make_fw(fw, 1000, 2000, 2); /* project 2 = RTL8821A is accepted for lmp_subver 0x8821 like in Linux */
    static unsigned char out[10000];
    const char *why;
    unsigned v;
    CHECK(bt_rtl_build(fw, fwl, cfg, sizeof cfg, 0x8821, 1, out, sizeof out, &v, &why) == 2010);

    test_download();

    /* the real files, when given */
    if (argc >= 3)
    {
        unsigned rl = 0, cl = 0;
        unsigned char *rfw = slurp(argv[1], &rl);
        unsigned char *rcfg = slurp(argv[2], &cl);
        if (rfw && rcfg)
        {
            printf("real files: firmware %u bytes, config %u bytes\n", rl, cl);
            CHECK(rl == 54636 && cl == 10);
            /* what an independent look at the real file showed: patches (chip 1: 19476 bytes at 48; chip 2: 34916 bytes at 19648),
             * version 0x75b8f098, project id 10 */
            test_build(rfw, rl, rcfg, cl, 19476, 34916, 0x75B8F098, 48, 19648, 1);
            unsigned n = bt_rtl_build(rfw, rl, rcfg, cl, 0x8821, 1, out, 0, &v, &why);
            CHECK(n == 0);
            static unsigned char big[70000];
            n = bt_rtl_build(rfw, rl, rcfg, cl, 0x8821, 1, big, sizeof big, &v, &why);
            CHECK(n == 34926);
            /* through the fake chip: the chip receives exactly these 34926 bytes */
            bt_attach(&T, 0x0BDA, 0xB00A);
            memset(&F, 0, sizeof F);
            F.fail_at = -1;
            unsigned done, total;
            CHECK(bt_rtl_download(big, n, &done, &total) == 0 && total == 139 && F.ngot == 34926 && memcmp(F.got, big, n) == 0);
            bt_detach();
        }
        else
            printf("real files not found: skipped\n");
    }
    else
        printf("(real firmware files not given: only the synthetic file was tested)\n");

    printf("bt_rtl: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
