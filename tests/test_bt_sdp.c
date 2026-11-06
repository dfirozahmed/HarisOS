/* Host test of the SDP request builder and answer parser (kernel/bluetooth/bt_sdp.c). The parser reads data from a device, so it is also
 * run on every cut-off and every damaged version of a valid answer, with the data placed at the very end of a page that is followed by a
 * page nobody may touch: one byte read too many and the test dies. */
#include <stdio.h>
#include <string.h>

#include "bluetooth.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

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

/* n bytes that end exactly where the memory ends */
static unsigned char *guarded(unsigned n)
{
    static unsigned char *base;
    if (!base)
    {
#ifdef _WIN32
        base = VirtualAlloc(0, 8192, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        DWORD old;
        VirtualProtect(base + 4096, 4096, PAGE_NOACCESS, &old);
#else
        base = mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        mprotect(base + 4096, 4096, PROT_NONE);
#endif
    }
    return base + 4096 - n;
}

#include "bt_sdp_fixture.h"

int main(void)
{
    BtSdpService s[16];

    /* ---- the request ---- */
    unsigned char req[64];
    unsigned n = bt_sdp_request(req, sizeof req, 0x1234, 400, 0, 0);
    static const unsigned char want[] = {0x06, 0x12, 0x34, 0x00, 0x16,                               /* PDU, tid, parameter length 22 */
                                         0x35, 0x03, 0x19, 0x01, 0x00,                               /* search for UUID 0x0100 */
                                         0x01, 0x90,                                                 /* at most 400 bytes */
                                         0x35, 0x0C, 0x09, 0x00, 0x01, 0x09, 0x00, 0x04, 0x09, 0x00, 0x09, 0x09, 0x01, 0x00, /* attributes 1, 4, 9, 0x100 */
                                         0x00};                                                      /* no continuation */
    CHECK(n == sizeof want && memcmp(req, want, n) == 0);
    unsigned char cont[3] = {0xAA, 0xBB, 0xCC};
    n = bt_sdp_request(req, sizeof req, 2, 400, cont, 3);
    CHECK(n == sizeof want + 3 && req[4] == 0x19 && req[n - 4] == 3 && memcmp(req + n - 3, cont, 3) == 0 && req[2] == 2);
    CHECK(bt_sdp_request(req, sizeof want - 1, 1, 400, 0, 0) == 0);   /* output one byte too small */
    CHECK(bt_sdp_request(req, sizeof want, 1, 400, 0, 0) == sizeof want); /* exactly big enough */
    CHECK(bt_sdp_request(req, sizeof req, 1, 400, cont, 17) == 0);    /* a continuation state is at most 16 bytes */

    /* ---- two records ---- */
    Buf recs = {{0}, 0}, all = {{0}, 0};
    rec_a2dp(&recs);
    rec_headset(&recs);
    add_seq(&all, &recs, 0);
    int k = bt_sdp_parse(all.b, all.n, s, 16);
    CHECK(k == 2);
    CHECK(s[0].class_id[0] == 0x110B && s[0].class_id[1] == 0);
    CHECK(s[0].has_psm && s[0].psm == 0x19 && !s[0].has_rfcomm && s[0].avdtp_ver == 0x0103);
    CHECK(s[0].profile == 0x110D && s[0].profile_ver == 0x0102);
    CHECK(strcmp(s[0].name, "Audio Sink") == 0);
    CHECK(s[1].class_id[0] == 0x1108 && s[1].class_id[1] == 0x1203);
    CHECK(!s[1].has_psm && s[1].has_rfcomm && s[1].rfcomm == 1);
    CHECK(s[1].profile == 0x1108 && s[1].profile_ver == 0x0102 && strcmp(s[1].name, "Headset") == 0);

    /* ---- other ways to write the same things ---- */
    {
        /* a 128-bit UUID with the Bluetooth base, a 32-bit UUID, the sequence lengths in 2 and 4 bytes */
        Buf r = {{0}, 0}, t = {{0}, 0}, list = {{0}, 0}, whole = {{0}, 0};
        add_u16(&r, 0x0001);
        static const unsigned char u128[17] = {0x1C, 0x00, 0x00, 0x11, 0x0B, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB};
        static const unsigned char u32[5] = {0x1A, 0x00, 0x00, 0x11, 0x0A};
        static const unsigned char other128[17] = {0x1C, 0x12, 0x34, 0x56, 0x78, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB};
        add(&t, u128, 17);
        add(&t, u32, 5);
        add_seq(&r, &t, 2);
        add_seq(&list, &r, 4);
        add_seq(&whole, &list, 2);
        k = bt_sdp_parse(whole.b, whole.n, s, 16);
        CHECK(k == 1 && s[0].class_id[0] == 0x110B && s[0].class_id[1] == 0x110A);
        t.n = r.n = list.n = whole.n = 0;
        add_u16(&r, 0x0001);
        add(&t, other128, 17); /* a vendor UUID: no 16-bit form */
        add_seq(&r, &t, 0);
        add_seq(&list, &r, 0);
        add_seq(&whole, &list, 0);
        k = bt_sdp_parse(whole.b, whole.n, s, 16);
        CHECK(k == 1 && s[0].class_id[0] == 0xFFFF);
    }

    /* ---- limits ---- */
    {
        Buf many = {{0}, 0}, whole = {{0}, 0};
        for (int i = 0; i < 20; i++)
            rec_a2dp(&many);
        add_seq(&whole, &many, 0);
        k = bt_sdp_parse(whole.b, whole.n, s, 16);
        CHECK(k == 16); /* as many as there is room for */
        k = bt_sdp_parse(whole.b, whole.n, s, 3);
        CHECK(k == 3);
        k = bt_sdp_parse(whole.b, whole.n, s, 0);
        CHECK(k == 0);
        /* a long name is cut between characters: 30 bytes, the 24th byte lies inside a 3-byte character after one ASCII byte */
        Buf r = {{0}, 0}, rec = {{0}, 0}, wh = {{0}, 0};
        add_u16(&r, 0x0100);
        char name[31];
        name[0] = 'A';
        for (int i = 0; i < 9; i++)
        {
            name[1 + 3 * i] = (char)0xE0;
            name[2 + 3 * i] = (char)0xA6;
            name[3 + 3 * i] = (char)0x85;
        }
        name[28] = 'Z';
        name[29] = 0;
        add_text(&r, name);
        add_seq(&rec, &r, 0);
        add_seq(&wh, &rec, 0);
        k = bt_sdp_parse(wh.b, wh.n, s, 16);
        CHECK(k == 1 && strlen(s[0].name) == 22 && s[0].name[0] == 'A' && (unsigned char)s[0].name[21] == 0x85); /* 1 + 7 characters */
        /* a name with a NUL inside ends there; a name of 300 bytes is read through a 2-byte length */
        r.n = rec.n = wh.n = 0;
        add_u16(&r, 0x0100);
        char big[301];
        memset(big, 'x', 300);
        big[300] = 0;
        add_text(&r, big);
        add_seq(&rec, &r, 0);
        add_seq(&wh, &rec, 0);
        k = bt_sdp_parse(wh.b, wh.n, s, 16);
        CHECK(k == 1 && strlen(s[0].name) == 23);
    }

    /* ---- not what we expect ---- */
    {
        unsigned char a[] = {0x09, 0x00, 0x01};              /* a number, not a sequence */
        CHECK(bt_sdp_parse(a, sizeof a, s, 16) == -1);
        CHECK(bt_sdp_parse(a, 0, s, 16) == -1);
        unsigned char b[] = {0x35, 0x00};                    /* an empty sequence: no records */
        CHECK(bt_sdp_parse(b, sizeof b, s, 16) == 0);
        unsigned char c[] = {0x35, 0x03, 0x09, 0x00, 0x01};  /* a sequence of numbers: not records, none is read */
        CHECK(bt_sdp_parse(c, sizeof c, s, 16) == 0);
        unsigned char d[] = {0x35, 0x09, 0x35, 0x07, 0x09, 0x00, 0x01, 0x35, 0x02, 0x19, 0x11}; /* a class list cut short inside */
        CHECK(bt_sdp_parse(d, sizeof d, s, 16) >= -1);       /* must not crash; what it finds is not the point */
        unsigned char e[] = {0x35, 0x05, 0x35, 0x03, 0x2D, 0x00, 0x00}; /* element type 5 with size index 5: not valid */
        CHECK(bt_sdp_parse(e, sizeof e, s, 16) >= 0);
        unsigned char f[] = {0x37, 0xFF, 0xFF, 0xFF, 0xFF}; /* a sequence of 4 GB */
        CHECK(bt_sdp_parse(f, sizeof f, s, 16) == -1);
    }

    /* ---- every cut-off and every damaged byte of a valid answer, on guarded memory ---- */
    {
        unsigned total = all.n;
        for (unsigned len = 0; len <= total; len++)
        {
            unsigned char *g = guarded(len);
            memcpy(g, all.b, len);
            int r = bt_sdp_parse(g, len, s, 16);
            CHECK(r >= -1 && r <= 2);
        }
        int cases = 0;
        for (unsigned i = 0; i < total; i++)
        {
            static const unsigned char vals[] = {0x00, 0x01, 0x07, 0x1F, 0x25, 0x35, 0x36, 0x37, 0x80, 0xFF};
            for (unsigned v = 0; v < sizeof vals; v++)
            {
                unsigned char *g = guarded(total);
                memcpy(g, all.b, total);
                g[i] = vals[v];
                int r = bt_sdp_parse(g, total, s, 16);
                CHECK(r >= -1 && r <= 2);
                cases++;
            }
        }
        CHECK(cases > 1000); /* the loops really ran */
    }

    /* ---- the server side: our one record, Object Push ---- */
    {
        unsigned char out[200];
        BtSdpService s[2];
        /* ServiceSearchAttributeRequest as a phone sends it: pattern {Object Push}, all attributes */
        const unsigned char rq6[] = {0x06, 0x00, 0x07, 0x00, 0x0F, 0x35, 0x03, 0x19, 0x11, 0x05, 0xFF, 0xFF, 0x35, 0x05, 0x0A, 0x00, 0x00, 0xFF, 0xFF, 0x00};
        unsigned n = bt_sdps_answer(rq6, sizeof rq6, out, sizeof out, 9);
        CHECK(n > 40 && out[0] == 0x07 && out[1] == 0 && out[2] == 7);
        unsigned plen = ((unsigned)out[3] << 8) | out[4], cnt = ((unsigned)out[5] << 8) | out[6];
        CHECK(plen + 5 == n && cnt + 3 == plen && out[n - 1] == 0); /* byte count, list, no continuation */
        CHECK(bt_sdp_parse(out + 7, cnt, s, 2) == 1);
        CHECK(s[0].class_id[0] == 0x1105 && s[0].has_rfcomm && s[0].rfcomm == 9 && s[0].profile == 0x1105 && s[0].profile_ver == 0x0100);
        CHECK(strcmp(s[0].name, "HarisOS") == 0);
        /* another channel number is reflected */
        n = bt_sdps_answer(rq6, sizeof rq6, out, sizeof out, 17);
        CHECK(n > 40 && bt_sdp_parse(out + 7, ((unsigned)out[5] << 8) | out[6], s, 2) == 1 && s[0].rfcomm == 17);
        /* the generic pattern phones use to browse (L2CAP) and the public browse group match too; a service we do not have does not */
        unsigned char r2[sizeof rq6];
        memcpy(r2, rq6, sizeof rq6);
        r2[8] = 0x01;
        r2[9] = 0x00; /* UUID 0x0100 */
        n = bt_sdps_answer(r2, sizeof r2, out, sizeof out, 9);
        CHECK(n > 40 && ((unsigned)out[5] << 8 | out[6]) > 10);
        r2[8] = 0x10;
        r2[9] = 0x02;
        n = bt_sdps_answer(r2, sizeof r2, out, sizeof out, 9);
        CHECK(n > 40);
        r2[8] = 0x11;
        r2[9] = 0x0B; /* A2DP sink: not ours */
        n = bt_sdps_answer(r2, sizeof r2, out, sizeof out, 9);
        CHECK(n == 10 && out[0] == 0x07 && out[5] == 0 && out[6] == 2 && out[7] == 0x35 && out[8] == 0 && out[9] == 0); /* an empty list */

        /* ServiceSearchRequest: the handle */
        const unsigned char rq2[] = {0x02, 0x00, 0x09, 0x00, 0x08, 0x35, 0x03, 0x19, 0x11, 0x05, 0x00, 0x10, 0x00};
        n = bt_sdps_answer(rq2, sizeof rq2, out, sizeof out, 9);
        CHECK(n == 14 && out[0] == 0x03 && out[2] == 9 && out[4] == 9 && out[6] == 1 && out[8] == 1);
        CHECK(out[9] == 0 && out[10] == 1 && out[11] == 0 && out[12] == 1 && out[13] == 0);
        unsigned char rq2b[sizeof rq2];
        memcpy(rq2b, rq2, sizeof rq2);
        rq2b[9] = 0x0B; /* UUID 0x110B: A2DP sink */
        n = bt_sdps_answer(rq2b, sizeof rq2b, out, sizeof out, 9);
        CHECK(n == 10 && out[0] == 0x03 && out[6] == 0 && out[8] == 0);

        /* ServiceAttributeRequest: only the name */
        const unsigned char rq4[] = {0x04, 0x00, 0x0A, 0x00, 0x0C, 0x00, 0x01, 0x00, 0x01, 0x00, 0xFF, 0x35, 0x03, 0x09, 0x01, 0x00, 0x00};
        n = bt_sdps_answer(rq4, sizeof rq4, out, sizeof out, 9);
        CHECK(n > 10 && out[0] == 0x05 && out[2] == 10);
        cnt = ((unsigned)out[5] << 8) | out[6];
        CHECK(cnt == 14 && out[7] == 0x35 && out[8] == 12 && out[9] == 0x09 && out[10] == 0x01 && out[11] == 0x00 && out[12] == 0x25 && memcmp(out + 14, "HarisOS", 7) == 0);
        CHECK(n == 7 + cnt + 1 && out[n - 1] == 0);
        /* a range of attribute ids picks several */
        const unsigned char rq4r[] = {0x04, 0x00, 0x0B, 0x00, 0x0E, 0x00, 0x01, 0x00, 0x01, 0x00, 0xFF, 0x35, 0x05, 0x0A, 0x00, 0x00, 0x00, 0x05, 0x00};
        n = bt_sdps_answer(rq4r, sizeof rq4r, out, sizeof out, 9);
        CHECK(n > 40 && out[0] == 0x05 && out[8] > 30); /* handle, classes, protocols, browse groups: four of the eight */
        /* wrong record handle */
        unsigned char rq4h[sizeof rq4];
        memcpy(rq4h, rq4, sizeof rq4);
        rq4h[8] = 0x77;
        n = bt_sdps_answer(rq4h, sizeof rq4h, out, sizeof out, 9);
        CHECK(n == 7 && out[0] == 0x01 && out[5] == 0 && out[6] == 2);
        /* a continuation state we never gave */
        unsigned char rqc[sizeof rq6];
        memcpy(rqc, rq6, sizeof rq6);
        rqc[sizeof rq6 - 1] = 1;
        n = bt_sdps_answer(rqc, sizeof rqc, out, sizeof out, 9);
        CHECK(n == 7 && out[0] == 0x01 && out[6] == 5);
        /* a request that is not one */
        const unsigned char junk[] = {0x77, 0x00, 0x01, 0x00, 0x00};
        n = bt_sdps_answer(junk, sizeof junk, out, sizeof out, 9);
        CHECK(n == 7 && out[0] == 0x01 && out[6] == 3);
        CHECK(bt_sdps_answer(junk, 3, out, sizeof out, 9) == 0 && bt_sdps_answer(rq6, sizeof rq6, out, 8, 9) == 0);

        /* every cut-off and every damaged byte of the three requests: some answer or none, never a crash or an overrun (the request sits at the end of a page) */
        const unsigned char *reqs[3] = {rq6, rq2, rq4};
        const unsigned sizes[3] = {sizeof rq6, sizeof rq2, sizeof rq4};
        int cases = 0;
        for (int r = 0; r < 3; r++)
        {
            for (unsigned cut = 0; cut <= sizes[r]; cut++)
            {
                unsigned char *g = guarded(cut ? cut : 1);
                memcpy(g, reqs[r], cut);
                unsigned m = bt_sdps_answer(g, cut, out, sizeof out, 9);
                CHECK(m <= sizeof out);
                cases++;
            }
            for (unsigned i = 0; i < sizes[r]; i++)
            {
                static const unsigned char vals[] = {0x00, 0x01, 0x7F, 0x80, 0xFF, 0x35, 0x25, 0x09};
                for (unsigned v = 0; v < sizeof vals; v++)
                {
                    unsigned char *g = guarded(sizes[r]);
                    memcpy(g, reqs[r], sizes[r]);
                    g[i] = vals[v];
                    unsigned m = bt_sdps_answer(g, sizes[r], out, sizeof out, 9);
                    CHECK(m <= sizeof out);
                    cases++;
                }
            }
        }
        CHECK(cases > 100);
        CHECK(strcmp(bt_sdp_uuid_name(0x1105), "file push (OPP)") == 0 && strcmp(bt_sdp_uuid_name(0x1106), "file transfer (FTP)") == 0);
    }

    /* ---- the names ---- */
    CHECK(strcmp(bt_sdp_uuid_name(0x110B), "A2DP sink") == 0 && strcmp(bt_sdp_uuid_name(0x110A), "A2DP source") == 0);
    CHECK(strcmp(bt_sdp_uuid_name(0x1108), "headset") == 0 && strcmp(bt_sdp_uuid_name(0x111E), "hands-free") == 0);
    CHECK(strcmp(bt_sdp_uuid_name(0x110D), "A2DP") == 0 && strcmp(bt_sdp_uuid_name(0x110E), "AVRCP") == 0);
    CHECK(bt_sdp_uuid_name(0x1234)[0] == 0);

    printf("bt_sdp: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
