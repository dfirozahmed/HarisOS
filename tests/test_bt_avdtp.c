/* Host test of the AVDTP message functions (kernel/bluetooth/bt_avdtp.c): reading the answers of a sink and building the configuration. The
 * readers get data from a device, so they also run on every cut-off of a valid answer on guarded memory (the data ends exactly where the
 * memory ends, the page after it cannot be touched: one byte read too many and the test dies). */
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

/* the capabilities of a typical headset: Media Transport, SBC with everything, bitpool 2 .. 53, Delay Reporting */
static const unsigned char CAPS[] = {0x01, 0x00, 0x07, 0x06, 0x00, 0x00, 0x3F, 0xFF, 0x02, 0x35, 0x08, 0x00};

static BtSbcCaps caps(unsigned freq, unsigned mode, unsigned blocks, unsigned bands, unsigned alloc, unsigned minbp, unsigned maxbp)
{
    BtSbcCaps c;
    memset(&c, 0, sizeof c);
    c.has_sbc = 1;
    c.freq = freq;
    c.mode = mode;
    c.blocks = blocks;
    c.bands = bands;
    c.alloc = alloc;
    c.min_bitpool = minbp;
    c.max_bitpool = maxbp;
    return c;
}

int main(void)
{
    /* ---- Discover ---- */
    BtAvSep s[8];
    {
        /* endpoint 1: a source, free; 2: a sink in use; 3: a sink, free; 4: video */
        static const unsigned char d[] = {0x04, 0x00, 0x0A, 0x08, 0x0C, 0x08, 0x10, 0x18};
        int k = bt_avdtp_parse_discover(d, sizeof d, s, 8);
        CHECK(k == 4);
        CHECK(s[0].seid == 1 && !s[0].in_use && s[0].media_type == 0 && !s[0].sink);
        CHECK(s[1].seid == 2 && s[1].in_use && s[1].media_type == 0 && s[1].sink);
        CHECK(s[2].seid == 3 && !s[2].in_use && s[2].sink);
        CHECK(s[3].seid == 4 && s[3].media_type == 1 && s[3].sink);
        CHECK(bt_avdtp_parse_discover(d, sizeof d, s, 2) == 2);   /* as many as there is room for */
        CHECK(bt_avdtp_parse_discover(d, 7, s, 8) == -1);         /* an odd length */
        CHECK(bt_avdtp_parse_discover(d, 0, s, 8) == 0);          /* no endpoints at all */
        for (unsigned len = 0; len <= sizeof d; len++)            /* every cut-off, on guarded memory */
        {
            unsigned char *g = guarded(len);
            memcpy(g, d, len);
            int r = bt_avdtp_parse_discover(g, len, s, 8);
            CHECK(r == -1 || r == (int)(len / 2));
        }
    }

    /* ---- Get Capabilities ---- */
    BtSbcCaps c;
    CHECK(bt_avdtp_parse_caps(CAPS, sizeof CAPS, &c) == 3);
    CHECK(c.has_sbc && c.has_transport && c.has_delay);
    CHECK(c.freq == 3 && c.mode == 15 && c.blocks == 15 && c.bands == 3 && c.alloc == 3 && c.min_bitpool == 2 && c.max_bitpool == 53);
    {
        /* other categories in between are skipped; a second codec of another kind is not SBC */
        static const unsigned char x[] = {0x04, 0x02, 0x00, 0x00, 0x07, 0x08, 0x00, 0x02, 0xF0, 0x01, 0x80, 0x00, 0x00, 0x00}; /* content protection, AAC */
        CHECK(bt_avdtp_parse_caps(x, sizeof x, &c) == 2 && !c.has_sbc);
        static const unsigned char v[] = {0x07, 0x06, 0x10, 0x00, 0x3F, 0xFF, 0x02, 0x35}; /* media type video, codec 0: not audio SBC */
        CHECK(bt_avdtp_parse_caps(v, sizeof v, &c) == 1 && !c.has_sbc);
        static const unsigned char w[] = {0x07, 0x05, 0x00, 0x00, 0x3F, 0xFF, 0x02}; /* an SBC capability of the wrong length */
        CHECK(bt_avdtp_parse_caps(w, sizeof w, &c) == 1 && !c.has_sbc);
        CHECK(bt_avdtp_parse_caps(CAPS, 0, &c) == 0 && !c.has_sbc);
        static const unsigned char o[] = {0x07, 0xFF, 0x00};     /* a length that runs past the end */
        CHECK(bt_avdtp_parse_caps(o, sizeof o, &c) == -1);
        static const unsigned char one[] = {0x07};               /* a single byte */
        CHECK(bt_avdtp_parse_caps(one, sizeof one, &c) == -1);
    }
    for (unsigned len = 0; len <= sizeof CAPS; len++) /* every cut-off */
    {
        unsigned char *g = guarded(len);
        memcpy(g, CAPS, len);
        int r = bt_avdtp_parse_caps(g, len, &c);
        CHECK(r >= -1 && r <= 3);
    }
    {
        int cases = 0;
        for (unsigned i = 0; i < sizeof CAPS; i++) /* every damaged byte */
        {
            static const unsigned char vals[] = {0x00, 0x01, 0x07, 0x7F, 0x80, 0xFF};
            for (unsigned v = 0; v < sizeof vals; v++)
            {
                unsigned char *g = guarded(sizeof CAPS);
                memcpy(g, CAPS, sizeof CAPS);
                g[i] = vals[v];
                int r = bt_avdtp_parse_caps(g, sizeof CAPS, &c);
                CHECK(r >= -1 && r <= 6);
                cases++;
            }
        }
        CHECK(cases == (int)sizeof CAPS * 6);
    }

    /* ---- the choice ---- */
    BtSbcConfig cfg;
    c = caps(3, 15, 15, 3, 3, 2, 53);
    CHECK(bt_avdtp_choose(&c, &cfg) == 0);
    CHECK(cfg.freq == 1 && cfg.mode == 1 && cfg.blocks == 1 && cfg.bands == 1 && cfg.alloc == 1 && cfg.bitpool == 51 && cfg.min_bitpool == 2); /* the best of everything */
    c = caps(2, 15, 15, 3, 3, 2, 53); /* only 44.1 kHz: its bitpool aim is 53 */
    CHECK(bt_avdtp_choose(&c, &cfg) == 0 && cfg.freq == 2 && cfg.bitpool == 53);
    c = caps(2, 2, 2, 2, 2, 2, 35); /* only stereo, 12 blocks, 4 subbands, SNR, bitpool up to 35 */
    CHECK(bt_avdtp_choose(&c, &cfg) == 0 && cfg.mode == 2 && cfg.blocks == 2 && cfg.bands == 2 && cfg.alloc == 2 && cfg.bitpool == 35);
    c = caps(1, 3, 4, 1, 1, 2, 53); /* 8 blocks only */
    CHECK(bt_avdtp_choose(&c, &cfg) == 0 && cfg.blocks == 4 && cfg.mode == 1);
    c = caps(1, 1, 8, 1, 1, 2, 53); /* 4 blocks only */
    CHECK(bt_avdtp_choose(&c, &cfg) == 0 && cfg.blocks == 8);
    c = caps(1, 1, 1, 1, 1, 40, 45); /* a narrow bitpool range below our aim: its top */
    CHECK(bt_avdtp_choose(&c, &cfg) == 0 && cfg.bitpool == 45 && cfg.min_bitpool == 40);
    c = caps(1, 1, 1, 1, 1, 60, 70); /* a range above our aim: not used */
    CHECK(bt_avdtp_choose(&c, &cfg) == BT_AV_NO_BITPOOL);
    c = caps(1, 1, 1, 1, 1, 50, 40); /* a range that cannot be */
    CHECK(bt_avdtp_choose(&c, &cfg) == BT_AV_NO_BITPOOL);
    c = caps(4, 15, 15, 3, 3, 2, 53); /* 32 kHz only: we play 48 and 44.1 */
    CHECK(bt_avdtp_choose(&c, &cfg) == BT_AV_NO_FREQ);
    c = caps(0, 15, 15, 3, 3, 2, 53);
    CHECK(bt_avdtp_choose(&c, &cfg) == BT_AV_NO_FREQ);
    c = caps(3, 12, 15, 3, 3, 2, 53); /* dual channel and mono only */
    CHECK(bt_avdtp_choose(&c, &cfg) == BT_AV_NO_MODE);
    c = caps(3, 15, 0, 3, 3, 2, 53);
    CHECK(bt_avdtp_choose(&c, &cfg) == BT_AV_NO_BLOCKS);
    c = caps(3, 15, 15, 0, 3, 2, 53);
    CHECK(bt_avdtp_choose(&c, &cfg) == BT_AV_NO_BANDS);
    c = caps(3, 15, 15, 3, 0, 2, 53);
    CHECK(bt_avdtp_choose(&c, &cfg) == BT_AV_NO_ALLOC);
    c = caps(3, 15, 15, 3, 3, 2, 53);
    c.has_sbc = 0;
    CHECK(bt_avdtp_choose(&c, &cfg) == BT_AV_NO_SBC);

    /* ---- Set Configuration ---- */
    {
        unsigned char out[32];
        BtSbcConfig w = {1, 1, 1, 1, 1, 51, 2};
        unsigned n = bt_avdtp_set_config(out, sizeof out, 3, 1, &w);
        static const unsigned char want[] = {0x0C, 0x04,                                 /* the sink's endpoint 3, ours 1 (the number sits in the upper 6 bits) */
                                             0x01, 0x00,                                 /* Media Transport */
                                             0x07, 0x06, 0x00, 0x00, 0x11, 0x15, 0x02, 0x33}; /* Media Codec: audio, SBC, 48 kHz joint, 16 blocks, 8 subbands, loudness, bitpool 2 .. 51 */
        CHECK(n == sizeof want && memcmp(out, want, n) == 0);
        CHECK(bt_avdtp_set_config(out, 11, 3, 1, &w) == 0);          /* too small */
        CHECK(bt_avdtp_set_config(out, 12, 3, 1, &w) == 12);         /* just big enough */
        /* every field different, so that a mix-up of two of them shows: 48 kHz, stereo, 8 blocks, 4 subbands, loudness, bitpool 17 .. 40 */
        BtSbcConfig asym = {1, 2, 4, 2, 1, 40, 17};
        n = bt_avdtp_set_config(out, sizeof out, 5, 1, &asym);
        CHECK(n == 12 && out[0] == 0x14 && out[1] == 0x04 && out[8] == 0x12 && out[9] == 0x49 && out[10] == 17 && out[11] == 40);
        /* what we send reads back as the choice (the codec part is in the same form as the capabilities) */
        BtSbcCaps back;
        BtSbcConfig w2 = {2, 2, 2, 2, 2, 35, 10};
        n = bt_avdtp_set_config(out, sizeof out, 7, 1, &w2);
        CHECK(n == 12 && out[0] == (7 << 2));
        CHECK(bt_avdtp_parse_caps(out + 2, n - 2, &back) == 2 && back.has_sbc && back.freq == 2 && back.mode == 2 && back.blocks == 2 && back.bands == 2 &&
              back.alloc == 2 && back.min_bitpool == 10 && back.max_bitpool == 35);
        /* and the choice made from a headset's capabilities can be sent and read back */
        BtSbcCaps hc;
        bt_avdtp_parse_caps(CAPS, sizeof CAPS, &hc);
        BtSbcConfig ch;
        CHECK(bt_avdtp_choose(&hc, &ch) == 0);
        n = bt_avdtp_set_config(out, sizeof out, 1, 1, &ch);
        CHECK(bt_avdtp_parse_caps(out + 2, n - 2, &back) == 2 && back.freq == ch.freq && back.max_bitpool == ch.bitpool);
    }

    printf("bt_avdtp: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
