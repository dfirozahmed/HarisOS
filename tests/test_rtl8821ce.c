/* Host test for rtl8821ce.c (stages 1, 2, 3a and 3b). Against a pretend chip:
 *   - the register file is plain memory; every write the driver makes is logged (so the ORDER of the sequences is checked) and shown to
 *     the model, which reacts like the chip would: clears or sets status bits, runs the "beacon queue" DMA, the DDMA engine and the CPU boot;
 *   - the model reads the DMA rings and descriptors the driver wrote and checks every field against what Linux builds;
 *   - the pretend drive holds a synthetic firmware file, and at the end the chip's memory must hold exactly its three parts.
 * It proves the driver's own logic, not that the real chip behaves like the model. (On the real card the power-on part has been seen to
 * work; the power-off part and everything from the DMA on have not.) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/net/rtl8821ce.c"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static _Alignas(4096) unsigned char REGS[0x4000];
static unsigned char SNAP[sizeof REGS];
static u32 CFG_CMD;
static int BAR_KIND; /* 0 = 64 bit memory BAR at REGS, 1 = I/O BAR, 2 = empty */

static u32 reg32(unsigned off) { u32 v; memcpy(&v, REGS + off, 4); return v; }
static unsigned reg16(unsigned off) { unsigned v = REGS[off] | REGS[off + 1] << 8; return v; }
static void set32(unsigned off, u32 v) { memcpy(REGS + off, &v, 4); }
static void set16(unsigned off, u16 v) { memcpy(REGS + off, &v, 2); }
static u32 le32p(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static unsigned le16p(const unsigned char *p) { return p[0] | p[1] << 8; }

/* ---- the write log ---- */
typedef struct { u32 off; unsigned size; u32 value; } Wr;
/* the commands to the firmware (mailboxes 0 to 3): which mailbox, the two words */
typedef struct { u32 box_off; u32 w0, w1; } H2cCmd;
static H2cCmd H2CLOG[64];
static int H2CLOG_N;
static Wr LOG[4096];
static int NLOG;

/* ---- DMA memory: host memory with a made-up 32-bit address ---- */
static unsigned char *DMA_HOST;
static u32 DMA_FAKE = 0x20000000u;
static unsigned DMA_BYTES;
static int DMA_FAIL; /* 1 = no memory below 4 GiB */
void *rtl8821ce_host_dma_alloc(unsigned pages, u32 *dma)
{
    if (DMA_FAIL)
        return 0;
    DMA_BYTES = pages * 4096;
    unsigned char *raw = calloc(1, DMA_BYTES + 4096);
    DMA_HOST = (unsigned char *)(((size_t)raw + 4095) & ~(size_t)4095);
    *dma = DMA_FAKE;
    return DMA_HOST;
}
static unsigned char *dma2host(u32 dma)
{
    if (dma < DMA_FAKE || dma >= DMA_FAKE + DMA_BYTES)
        return 0;
    return DMA_HOST + (dma - DMA_FAKE);
}

/* ---- the efuse the pretend chip holds (physical map, 512 bytes) ---- */
static unsigned char EFUSE_MAP[512];
static int KNOB_EFUSE_STUCK;
static int KNOB_LLT_STUCK;
static int PH_BREAK_RF_READBACK; /* the RF window does not follow the SIPI writes */
static unsigned PH_FLIP_RF_BITS; /* the chip itself changes these bits of RF register 0x18 and 0x51 */
static int KNOB_H2C_STUCK; /* the firmware never takes a command from the mailbox */
static unsigned LTEM[0x400]; /* the coexistence block's registers behind the indirect interface */
static unsigned char EFUSE_RFE = 4, EFUSE_XTAL = 0x2A; /* what the board's efuse says at 0xCA and 0xB9 */

static void build_efuse(const unsigned char mac[6])
{
    memset(EFUSE_MAP, 0xFF, sizeof EFUSE_MAP);
    unsigned i = 0;
    /* block 1, 1-byte header: words 0 and 2 are written (word_en bits are 1 for words NOT written: 0b1010) */
    EFUSE_MAP[i++] = (1 << 4) | 0xA;
    EFUSE_MAP[i++] = 0xAA; EFUSE_MAP[i++] = 0xBB; EFUSE_MAP[i++] = 0xCC; EFUSE_MAP[i++] = 0xDD;
    /* block 26 (0xD0 / 8), which needs the 2-byte header: words 0, 1, 2 hold the MAC address */
    EFUSE_MAP[i++] = ((26 & 7) << 5) | 0x0F;
    EFUSE_MAP[i++] = ((26 >> 3) << 4) | 0x8;
    for (int k = 0; k < 6; k++)
        EFUSE_MAP[i++] = mac[k];
    /* block 23 (0xB8): word 0 = channel plan, crystal calibration (0xB9); block 25 (0xC8): word 1 = rf antenna option, rfe_option (0xCA is the byte after) */
    EFUSE_MAP[i++] = ((23 & 7) << 5) | 0x0F;
    EFUSE_MAP[i++] = ((23 >> 3) << 4) | 0xE;
    EFUSE_MAP[i++] = 0x00; EFUSE_MAP[i++] = EFUSE_XTAL;
    EFUSE_MAP[i++] = ((25 & 7) << 5) | 0x0F;
    EFUSE_MAP[i++] = ((25 >> 3) << 4) | 0xD;
    EFUSE_MAP[i++] = EFUSE_RFE; EFUSE_MAP[i++] = 0x00;
    /* blocks 2 .. 7 (0x10 .. 0x3F): the transmit power calibration, byte 0x10 + k = 5 + 3 * k */
    for (int blk = 2; blk <= 7; blk++)
    {
        EFUSE_MAP[i++] = (unsigned char)((blk << 4) | 0x0);
        for (int b = 0; b < 8; b++)
            EFUSE_MAP[i++] = (unsigned char)(5 + 3 * ((blk - 2) * 8 + b));
    }
}
/* ---- the pretend chip's insides ---- */
static unsigned char TXFIFO[16384];
static unsigned char CHIPMEM[0x400000];
static unsigned BCN_PAGE, PIECES;
static int KNOB_NO_BCN_VALID, KNOB_DDMA_STUCK, KNOB_CHKSUM_BAD, KNOB_NO_BOOT;
static int MODEL_ERRORS;
typedef struct { u32 dst, len; int cont; } DdmaOp;
static DdmaOp DDMA[64];
static int NDDMA;
static int BOOTED, IMAGE_OK;

#define MCHECK(c) do { if (!(c)) { printf("FAIL (model) line %d: %s\n", __LINE__, #c); MODEL_ERRORS++; fails++; } } while (0)

/* the synthetic firmware: header as in the real file, parts of other sizes (so that every part needs several pieces) */
#define SYN_DMEM 5000u
#define SYN_IMEM 9000u
#define SYN_EMEM 300u
static unsigned char IMG[64 + (SYN_DMEM + 8) + (SYN_IMEM + 8) + (SYN_EMEM + 8)];
static unsigned IMG_SIZE;
static unsigned EXPECT_DMEM_AT, EXPECT_IMEM_AT, EXPECT_EMEM_AT;

static void build_image(int with_emem)
{
    memset(IMG, 0, sizeof IMG);
    unsigned dmem = SYN_DMEM + 8, imem = SYN_IMEM + 8, emem = with_emem ? SYN_EMEM + 8 : 0;
    IMG_SIZE = 64 + dmem + imem + emem;
    IMG[0] = 0x21; IMG[1] = 0x88; IMG[4] = 24; IMG[6] = 11;
    IMG[0x10] = 1; IMG[0x11] = 10; IMG[0x14] = 0xE6; IMG[0x15] = 0x07;
    IMG[0x18] = with_emem ? 0x18 : 0x08;
    memcpy(IMG + 0x20, &(u32){0x80200000u}, 4);
    memcpy(IMG + 0x24, &(u32){SYN_DMEM}, 4);
    memcpy(IMG + 0x30, &(u32){SYN_IMEM}, 4);
    memcpy(IMG + 0x34, &(u32){with_emem ? SYN_EMEM : 0}, 4);
    memcpy(IMG + 0x38, &(u32){0x80100000u}, 4);
    memcpy(IMG + 0x3C, &(u32){0x80030000u}, 4);
    unsigned x = 12345;
    for (unsigned i = 64; i < IMG_SIZE; i++)
    {
        x = x * 1103515245u + 12345u;
        IMG[i] = (unsigned char)(x >> 16);
    }
    EXPECT_DMEM_AT = 0x00200000u;
    EXPECT_IMEM_AT = 0x00030000u;
    EXPECT_EMEM_AT = 0x00100000u;
}

static void run_bcn_dma(void)
{
    u32 bd_dma = reg32(0x308); /* RTK_PCI_TXBD_DESA_BCNQ */
    unsigned char *bd = dma2host(bd_dma);
    MCHECK(bd != 0);
    if (!bd)
        return;
    unsigned buf0 = le16p(bd), psb0 = le16p(bd + 2), buf1 = le16p(bd + 8);
    u32 dma0 = le32p(bd + 4), dma1 = le32p(bd + 12);
    MCHECK(buf0 == 48 && (psb0 & 0x8000) != 0);                      /* descriptor entry, owned by the chip */
    MCHECK((psb0 & 0x7FFF) == (48 + buf1 - 1) / 128 + 1);             /* total length in 128 byte units */
    MCHECK(dma1 == dma0 + 48 && le16p(bd + 10) == 0);
    unsigned char *desc = dma2host(dma0);
    unsigned char *data = dma2host(dma1);
    MCHECK(desc != 0 && data != 0);
    if (!desc || !data)
        return;
    u32 w0 = le32p(desc), w1 = le32p(desc + 4), w2 = le32p(desc + 8), w3 = le32p(desc + 12), w4 = le32p(desc + 16);
    MCHECK((w0 & 0xFFFF) == buf1 && ((w0 >> 16) & 0xFF) == 48);       /* packet size, offset = descriptor size */
    MCHECK(((w0 >> 24) & 1) == (unsigned)(buf1 > 4 ? (data[4] & 1) : 0)); /* "broadcast" = the multicast bit of the piece's 5th byte */
    MCHECK(((w0 >> 26) & 1) == 1 && ((w0 >> 31) & 1) == 1);           /* last segment, no queue/sequence select */
    MCHECK(((w1 >> 8) & 0x1F) == 16 && ((w1 >> 16) & 0x1F) == 7 && (w1 & 0xFF) == 0); /* beacon queue, rate id G, mac id 0 */
    MCHECK(w2 == 0 && w3 == (BIT(8) | BIT(10)) && (w4 & 0x7F) == 4);
    MCHECK((le32p(desc + 32) & BIT(15)) != 0 && le32p(desc + 20) == 0 && le32p(desc + 24) == 0 && le32p(desc + 28) == 0);
    for (int i = 40; i < 48; i++)
        MCHECK(desc[i] == 0);
    /* what Linux set up before sending: page 0, beacon function off, software beacon enabled, beacon queue download off */
    MCHECK(BCN_PAGE == 0);
    MCHECK((REGS[0x550] & 0x18) == 0x10);
    MCHECK((REGS[0x101] & 1) != 0);
    MCHECK((REGS[0x422] & 0x40) == 0);
    MCHECK(buf1 <= 4096 && BCN_PAGE * 128 + 48 + buf1 <= sizeof TXFIFO);
    if (KNOB_NO_BCN_VALID)
        return;
    memcpy(TXFIFO + BCN_PAGE * 128, desc, 48);
    memcpy(TXFIFO + BCN_PAGE * 128 + 48, data, buf1);
    REGS[0x205] |= 0x80; /* BIT_BCN_VALID_V1: the "beacon" is in the buffer */
    PIECES++;
}

static void run_ddma(u32 ctrl)
{
    u32 src = reg32(0x1200), dst = reg32(0x1204);
    unsigned len = ctrl & 0x3FFFF;
    MCHECK(src == 0x18780000u + 48);                                    /* the transmit buffer, behind the descriptor */
    MCHECK((ctrl & BIT(29)) != 0 && (ctrl & BIT(26)) == 0);            /* checksum on, normal mode */
    if (NDDMA < 64)
        DDMA[NDDMA++] = (DdmaOp){dst, len, (ctrl & BIT(24)) != 0};
    if (KNOB_DDMA_STUCK)
        return;
    MCHECK(dst + len <= sizeof CHIPMEM);
    if (dst + len <= sizeof CHIPMEM)
        memcpy(CHIPMEM + dst, TXFIFO + 48, len);
    REGS[0x120B] &= 0x7F; /* the engine is done: the OWN bit goes */
    if (KNOB_CHKSUM_BAD)
        REGS[0x120B] |= 0x08; /* BIT_DDMACH0_CHKSUM_STS */
}

static void maybe_boot(void)
{
    unsigned ctrl = reg16(0x80);
    if (!(REGS[0x03] & 4) || !(REGS[0x1D] & 1) || !(ctrl & BIT(14)) || (ctrl & 0x78) != 0x78 || (ctrl & BIT(0)))
        return;
    /* the CPU starts only if the three parts really are in its memory */
    IMAGE_OK = memcmp(CHIPMEM + EXPECT_DMEM_AT, IMG + 64, SYN_DMEM + 8) == 0 &&
               memcmp(CHIPMEM + EXPECT_IMEM_AT, IMG + 64 + SYN_DMEM + 8, SYN_IMEM + 8) == 0 &&
               (le32p(IMG + 0x34) == 0 ||
                memcmp(CHIPMEM + EXPECT_EMEM_AT, IMG + 64 + SYN_DMEM + 8 + SYN_IMEM + 8, SYN_EMEM + 8) == 0);
    if (IMAGE_OK && !KNOB_NO_BOOT)
    {
        REGS[0x81] |= 0x80; /* BIT_FW_INIT_RDY */
        BOOTED = 1;
    }
}

/* ---- capture of every chip access (writes and delays) as a hash, to compare with Linux's own table interpreter ---- */
static int CAPTURE;
static long CAP_N;
static unsigned long long CAP_H;
static void cap_ev(unsigned t, unsigned a, unsigned b, unsigned c)
{
    unsigned w[4] = {t, a, b, c};
    for (int i = 0; i < 16; i++)
    {
        CAP_H ^= ((unsigned char *)w)[i];
        CAP_H *= 1099511628211ULL;
    }
    CAP_N++;
}

/* the random numbers of the driver: a repeatable pattern */
static unsigned RND_COUNTER;
void rtl8821ce_host_random(unsigned char *out, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        out[i] = (unsigned char)(0x80 + RND_COUNTER * 7 + i * 3);
    RND_COUNTER++;
}

/* ---- the pretend chip's receiver: frames the test queued are put into the receive ring, one per delay call, once the receiver is on ---- */
typedef struct { unsigned char fc0; unsigned len; int c2h, crc_err; const unsigned char *data; unsigned w0_or; } RxFrame;
static RxFrame RXQ[128];
static int RXQ_N, RXQ_NEXT;
static void rx_deliver_one1(void);
static unsigned RX_HW; /* the chip's write index */
static void rxq_compact(void) { if (RXQ_NEXT >= RXQ_N) RXQ_N = RXQ_NEXT = 0; } /* everything queued was delivered: the table is free again */
static void rxq_add(unsigned char fc0, unsigned len, int c2h, int crc_err)
{
    rxq_compact();
    if (RXQ_N < 128)
        RXQ[RXQ_N++] = (RxFrame){fc0, len, c2h, crc_err, 0, 0};
}
static void rxq_add_frame(const unsigned char *data, unsigned len)
{
    rxq_compact();
    if (RXQ_N < 128)
        RXQ[RXQ_N++] = (RxFrame){data[0], len, 0, 0, data, 0};
}
/* a frame as the receive descriptor describes it after decryption: w0_or = the extra bits of descriptor word 0 */
static unsigned char RXPOOL[128][256];
static void rxq_add_frame_w0(const unsigned char *data, unsigned len, unsigned w0_or)
{
    rxq_compact();
    if (RXQ_N < 128 && len <= 256)
    {
        memcpy(RXPOOL[RXQ_N], data, len); /* a copy: the caller reuses its buffer */
        int at = RXQ_N++;
        RXQ[at] = (RxFrame){data[0], len, 0, 0, RXPOOL[at], w0_or};
    }
}
static int RX_BURST = 1; /* frames the chip delivers between two polls of the driver */
static void rx_deliver_one(void)
{
    for (int k = 0; k < RX_BURST; k++)
        rx_deliver_one1();
}
static void rx_deliver_one1(void)
{
    if (RXQ_NEXT >= RXQ_N || !D.ready || !(REGS[0x100] & 8) || reg32(0x608) != 0xF410400Eu) /* only once the driver has set the receive filter for listening */
        return;
    if ((RX_HW + 1) % 32 == (unsigned)((REGS[0x3B4] | REGS[0x3B5] << 8) & 0xFFF)) /* the ring is full: the chip waits for the host */
        return;
    unsigned char *buf = D.mem + Q_COUNT * PAGE + PAGE + RX_HW * RX_BUF_SLOT; /* the transmit rings, the receive ring, then the buffers */
    RxFrame *f = &RXQ[RXQ_NEXT++];
    memset(buf, 0, 24 + 32 + f->len);
    unsigned w0 = f->len | (f->crc_err ? 1u << 14 : 0) | (4u << 16) | f->w0_or;
    memcpy(buf, &w0, 4);
    unsigned w2 = f->c2h ? 1u << 28 : 0;
    memcpy(buf + 8, &w2, 4);
    buf[24 + 32] = f->fc0;
    if (f->data)
        memcpy(buf + 24 + 32, f->data, f->len);
    RX_HW = (RX_HW + 1) % 32;
    REGS[0x3B6] = (unsigned char)(RX_HW & 0xFF); /* bits 27..16 of 0x3B4 */
    REGS[0x3B7] = (unsigned char)((RX_HW >> 8) & 0x0F);
}

/* ---- the pretend chip's transmitter: what was kicked into the management ring is copied away, then the chip reports it has fetched it ---- */
typedef struct { unsigned len; unsigned char b[200]; } TxFrame;
static TxFrame TXLOG[32];
static int TXLOG_N;
typedef struct
{
    int count;
    unsigned char desc[48];
    unsigned char frame[256];
    unsigned len;
    unsigned char bd[16];
    unsigned idx_written;
    unsigned entry;
} TxCapture;
static TxCapture TXCAP;
static int KNOB_TX_STUCK; /* the chip never fetches anything */
static int KNOB_TX_ONLY32; /* the chip notices only a 32-bit write of the index */
static int KNOB_TX_SILENT; /* the frames go out and are answered, but the read index never moves */
static int POOL_OK = 1; /* the chip's transmit page pools are loaded: switching the command register off loses them until the next load (what the real card seemed to do) */
static int TX_ANSWER;     /* answer a broadcast probe request with this many probe responses */
static unsigned char TX_RESP[4][160];
static unsigned TX_RESP_LEN[4];
/* a pretend access point: answers authentication and association requests addressed to its BSSID */
static int AP_MODE; /* 0 answers everything, 1 silent, 2 refuses authentication (status 1), 3 refuses association (status 17) */
static const unsigned char AP_BSSID[6] = {2, 0, 0, 0, 0, 0xA1};
static unsigned char AP_RESP[8][64];
static int AP_RESP_N;
/* the pretend access point's side of the 4-way handshake: sends message 1 after the association, checks message 2, sends message 3, checks
 * message 4. It derives its keys from AP_PASS, so a station with another passphrase fails. */
static int AP_HS = 0;                         /* 0 no handshake (the access point sends no EAPOL), 1 the handshake runs */
static const char *AP_PASS = "secret-pass-phrase";
static const char *AP_SSID = "TestNet";
static int AP_BAD_MIC; /* message 3 goes out with a damaged signature */
static unsigned char AP_PTK[48];
static int AP_PTK_OK;
static int AP_GOT_MSG2, AP_GOT_MSG4, AP_MSG3_SENT;
static unsigned char AP_ANONCE[32];
static unsigned char HS_RESP[6][256];
static int HS_RESP_N;
static unsigned char AP_STA[6]; /* the station that associated */

static void ap_send_eapol(const unsigned char *eapol, unsigned len)
{
    unsigned char *r = HS_RESP[HS_RESP_N++ % 6];
    memset(r, 0, 256);
    r[0] = 0x08;
    r[1] = 0x02; /* data, from the distribution system */
    memcpy(r + 4, AP_STA, 6);
    memcpy(r + 10, AP_BSSID, 6);
    memcpy(r + 16, AP_BSSID, 6);
    static const unsigned char LLC[8] = {0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x88, 0x8E};
    memcpy(r + 24, LLC, 8);
    memcpy(r + 32, eapol, len);
    rxq_add_frame(r, 32 + len + 4); /* and the FCS */
}

static void ap_eapol_frame(unsigned char *f, unsigned info, unsigned replay, const unsigned char *kdata, unsigned kdlen)
{
    memset(f, 0, 99 + kdlen);
    f[0] = 2;
    f[1] = 3;
    f[2] = (unsigned char)((95 + kdlen) >> 8);
    f[3] = (unsigned char)(95 + kdlen);
    f[4] = 2;
    f[5] = (unsigned char)(info >> 8);
    f[6] = (unsigned char)info;
    f[8] = 16;
    f[16] = (unsigned char)replay;
    memcpy(f + 17, AP_ANONCE, 32);
    f[97] = (unsigned char)(kdlen >> 8);
    f[98] = (unsigned char)kdlen;
    memcpy(f + 99, kdata, kdlen);
    if (info & 0x100)
        wpa_eapol_mic(AP_PTK, f, 99 + kdlen, f + 81);
}

/* the access point's group key renewal (message 1 of the group handshake): the new group key (16 bytes from `keybase` on) with key id `kid`,
 * wrapped with the KEK of the PTK, signed with the KCK. Returns the length. */
static unsigned ap_group_msg1(unsigned char *m, unsigned replay, unsigned kid, unsigned keybase)
{
    unsigned char plain[24], wrapped[32];
    memset(plain, 0, sizeof plain);
    plain[0] = 0xDD;
    plain[1] = 22;
    plain[3] = 0x0F;
    plain[4] = 0xAC;
    plain[5] = 1;
    plain[6] = (unsigned char)kid;
    for (unsigned i = 0; i < 16; i++)
        plain[8 + i] = (unsigned char)(keybase + i);
    aes_wrap(AP_PTK + 16, plain, 3, wrapped);
    ap_eapol_frame(m, 0x1382, replay, wrapped, 32);
    return 99 + 32;
}

/* message 3 of the 4-way handshake again (as an access point sends it when it did not get message 4) with a higher replay counter */
static unsigned ap_msg3_again(unsigned char *m3, unsigned replay)
{
    static const unsigned char RSN[22] = {48, 20, 1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F, 0xAC, 2, 0, 0};
    unsigned char plain[48], wrapped[56];
    memset(plain, 0, sizeof plain);
    memcpy(plain, RSN, 22);
    plain[22] = 0xDD;
    plain[23] = 22;
    plain[25] = 0x0F;
    plain[26] = 0xAC;
    plain[27] = 1;
    plain[28] = 0x05;
    for (unsigned i = 0; i < 16; i++)
        plain[30 + i] = (unsigned char)(0x40 + i);
    plain[46] = 0xDD;
    aes_wrap(AP_PTK + 16, plain, 6, wrapped);
    ap_eapol_frame(m3, 0x13CA, replay, wrapped, 56);
    return 99 + 56;
}

static void ap_start_handshake(const unsigned char *sta)
{
    memcpy(AP_STA, sta, 6);
    for (unsigned i = 0; i < 32; i++)
        AP_ANONCE[i] = (unsigned char)(0x11 + i * 5);
    AP_PTK_OK = 0;
    AP_GOT_MSG2 = AP_GOT_MSG4 = AP_MSG3_SENT = 0;
    unsigned char m1[99];
    ap_eapol_frame(m1, 0x008A, 1, (const unsigned char *)"", 0);
    ap_send_eapol(m1, 99);
}

/* a data frame from the station: an EAPOL packet? */
static void ap_handshake_rx(const unsigned char *fr, unsigned len)
{
    static const unsigned char LLC[8] = {0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x88, 0x8E};
    if (!AP_HS || len < 32 + 99 || (fr[0] & 0x0C) != 0x08 || memcmp(fr + 4, AP_BSSID, 6) != 0 || memcmp(fr + 24, LLC, 8) != 0)
        return;
    const unsigned char *e = fr + 32;
    unsigned info = (unsigned)e[5] << 8 | e[6];
    if (info == 0x010A) /* message 2 */
    {
        unsigned char pmk[32];
        wpa_pmk(AP_PASS, (unsigned)strlen(AP_PASS), AP_SSID, (unsigned)strlen(AP_SSID), pmk);
        wpa_ptk(pmk, AP_BSSID, AP_STA, AP_ANONCE, e + 17, AP_PTK);
        unsigned char mic[16];
        wpa_eapol_mic(AP_PTK, e, 99 + (((unsigned)e[97] << 8) | e[98]), mic);
        if (memcmp(mic, e + 81, 16) != 0)
            return; /* the station has another passphrase: no message 3 (the real access point would repeat message 1) */
        AP_PTK_OK = 1;
        AP_GOT_MSG2++;
        /* message 3: the RSN element and the group key, wrapped */
        static const unsigned char RSN[22] = {48, 20, 1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F, 0xAC, 2, 0, 0};
        unsigned char plain[48], wrapped[56], m3[99 + 56];
        memset(plain, 0, sizeof plain);
        memcpy(plain, RSN, 22);
        plain[22] = 0xDD;
        plain[23] = 22;
        plain[25] = 0x0F;
        plain[26] = 0xAC;
        plain[27] = 1;
        plain[28] = 0x05;
        for (unsigned i = 0; i < 16; i++)
            plain[30 + i] = (unsigned char)(0x40 + i);
        plain[46] = 0xDD;
        aes_wrap(AP_PTK + 16, plain, 6, wrapped);
        ap_eapol_frame(m3, 0x13CA, 2, wrapped, 56);
        if (AP_BAD_MIC)
            m3[85] ^= 1;
        ap_send_eapol(m3, 99 + 56);
        AP_MSG3_SENT++;
    }
    else if (info == 0x030A && AP_PTK_OK) /* message 4 */
    {
        unsigned char mic[16];
        wpa_eapol_mic(AP_PTK, e, 99, mic);
        if (memcmp(mic, e + 81, 16) == 0)
        {
            AP_GOT_MSG4++;
            /* the port is open: the access point sends protected data frames (two the chip decrypts, one whose decryption fails) */
            for (int k = 0; k < 3; k++)
            {
                unsigned char *r = HS_RESP[HS_RESP_N++ % 6];
                memset(r, 0, 256);
                r[0] = 0x08;
                r[1] = 0x42; /* from the distribution system, protected */
                memset(r + 4, 0xFF, 6);
                memcpy(r + 10, AP_BSSID, 6);
                memcpy(r + 16, AP_BSSID, 6);
                rxq_add_frame_w0(r, 24 + 8 + 20 + 8 + 4, k < 2 ? (4u << 20) : (1u << 15));
            }
        }
    }
}

static void ap_answer(const unsigned char *fr, unsigned len)
{
    if (AP_MODE != 1)
        ap_handshake_rx(fr, len);
    if (AP_MODE == 1 || len < 24 || memcmp(fr + 4, AP_BSSID, 6) != 0)
        return;
    unsigned char *r = AP_RESP[AP_RESP_N++ % 8];
    memset(r, 0, 64);
    memcpy(r + 4, fr + 10, 6); /* to the sender */
    memcpy(r + 10, AP_BSSID, 6);
    memcpy(r + 16, AP_BSSID, 6);
    if (fr[0] == 0xB0)
    {
        r[0] = 0xB0;
        r[26] = 2; /* algorithm 0, sequence 2 */
        r[28] = AP_MODE == 2 ? 1 : 0;
        rxq_add_frame(r, 34); /* header, 6 bytes of body, 4 bytes FCS */
    }
    else if (fr[0] == 0x00)
    {
        r[0] = 0x10;
        r[24] = 0x11;
        r[25] = 0x04;
        r[26] = AP_MODE == 3 ? 17 : 0;
        r[28] = 5;
        r[29] = 0xC0; /* association id 5, with the two top bits set */
        rxq_add_frame(r, 34);
        if (AP_HS && AP_MODE == 0)
            ap_start_handshake(fr + 10);
    }
}

static void tx_kick(unsigned next)
{
    unsigned entry = (next + 127) % 128;
    TXCAP.count++;
    if (TXLOG_N < 32)
    {
        unsigned fl = (unsigned)D.mem[Q_MGMT * PAGE + ((next + 127) % 128) * 16 + 8] | (unsigned)D.mem[Q_MGMT * PAGE + ((next + 127) % 128) * 16 + 9] << 8;
        TXLOG[TXLOG_N].len = fl;
        memcpy(TXLOG[TXLOG_N].b, D.stage + 48, fl > 200 ? 200 : fl);
        TXLOG_N++;
    }
    TXCAP.idx_written = next;
    TXCAP.entry = entry;
    memcpy(TXCAP.bd, D.mem + Q_MGMT * PAGE + entry * 16, 16);
    memcpy(TXCAP.desc, D.stage, 48);
    unsigned len = (unsigned)TXCAP.bd[8] | (unsigned)TXCAP.bd[9] << 8;
    TXCAP.len = len;
    memcpy(TXCAP.frame, D.stage + 48, len > sizeof TXCAP.frame ? sizeof TXCAP.frame : len);
    if (KNOB_TX_STUCK || !POOL_OK)
        return;
    if (!KNOB_TX_SILENT)
    {
        REGS[0x3B2] = (unsigned char)(next & 0xFF); /* the chip's read index: it has fetched everything */
        REGS[0x3B3] = (unsigned char)((next >> 8) & 0x0F);
    }
    if ((TXCAP.frame[0] & 0xFC) == 0x40 && (TXCAP.frame[4] & 1)) /* a probe request to everybody */
        for (int i = 0; i < TX_ANSWER; i++)
            rxq_add_frame(TX_RESP[i], TX_RESP_LEN[i]);
    ap_answer(TXCAP.frame, TXCAP.len);
}

/* the pretend chip's best-effort queue (the data frames): what was kicked in is copied away, then the chip reports it has fetched it */
typedef struct
{
    int count;
    unsigned char desc[48];
    unsigned char frame[1700];
    unsigned len;
    unsigned char bd[16];
    unsigned idx_written;
} BeCapture;
static BeCapture BECAP;
static void tx_kick_be(unsigned next)
{
    unsigned entry = (next + 255) % 256;
    BECAP.count++;
    BECAP.idx_written = next;
    memcpy(BECAP.bd, D.mem + Q_BE * PAGE + entry * 16, 16);
    memcpy(BECAP.desc, D.stage, 48);
    BECAP.len = (unsigned)BECAP.bd[8] | (unsigned)BECAP.bd[9] << 8;
    memcpy(BECAP.frame, D.stage + 48, BECAP.len > sizeof BECAP.frame ? sizeof BECAP.frame : BECAP.len);
    REGS[0x3AA] = (unsigned char)(next & 0xFF);
    REGS[0x3AB] = (unsigned char)((next >> 8) & 0x0F);
}

void rtl8821ce_host_log(u32 off, unsigned size, u32 value)
{
    if (CAPTURE)
        cap_ev(1, off, size, value);
    if (NLOG < 4096)
        LOG[NLOG++] = (Wr){off, size, value};
    if (size == 1 && off == 0x100 && value == 0)
        POOL_OK = 0;
    else if (size == 4 && off == 0x22C && (value & BIT(31)))
        POOL_OK = 1;
    if (size == 2 && off == 0x204)
    {
        BCN_PAGE = value & 0xFFF;
        if (value & 0x8000)
            REGS[0x205] &= 0x7F; /* BIT_BCN_VALID_V1 is cleared by writing 1 */
    }
    else if (size == 1 && off == 0x383 && (value & 0x10))
    {
        run_bcn_dma();
        REGS[0x383] &= (unsigned char)~0x10;
    }
    else if (size == 4 && off == 0x30 && !(value & BIT(31)) && !KNOB_EFUSE_STUCK)
        set32(0x30, (value & ~0xFFu) | EFUSE_MAP[(value >> 8) & 0x3FF] | BIT(31)); /* the byte at that address, and "done" */
    else if (size == 4 && off == 0x1700)
    {
        unsigned c = (unsigned)reg32(0x1700), o = c & 0x3FF;
        if ((c & 0xF0000000u) == 0xC0000000u)
            LTEM[o] = reg32(0x1704);
        else if ((c & 0xF0000000u) == 0x80000000u)
            set32(0x1708, LTEM[o]);
        REGS[0x1703] |= 0x20; /* ready again */
    }
    else if (off == 0x3B0 && size == (KNOB_TX_ONLY32 ? 4u : 2u))
        tx_kick(value & 0xFFF);
    else if (off == 0x3A8 && size == 2)
        tx_kick_be(value & 0xFFF);
    else if (size == 4 && off >= 0x1D0 && off <= 0x1DC && (off & 3) == 0)
    {
        REGS[0x1CC] |= (unsigned char)(1u << ((off - 0x1D0) / 4)); /* HMETFR: the box holds a command until the firmware has read it */
        if (H2CLOG_N < 64)
            H2CLOG[H2CLOG_N++] = (H2cCmd){off, value, reg32(0x1F0 + (off - 0x1D0))};
    }
    else if (size == 4 && off == 0xC90 && !PH_BREAK_RF_READBACK)
        set32(0x2800 + ((value >> 20) & 0xFF) * 4, (value & 0xFFFFF) ^ (((value >> 20) & 0xFF) == 0x18 || ((value >> 20) & 0xFF) == 0x51 ? PH_FLIP_RF_BITS : 0)); /* the RF register answers in its direct-read window */
    else if (size == 1 && off == 0x208 && (value & 1) && !KNOB_LLT_STUCK)
        REGS[0x208] &= (unsigned char)~1; /* BIT_AUTO_INIT_LLT_V1: the chip has linked its pages and clears the bit */
    else if (size == 4 && off == 0x1208 && (value & BIT(31)))
        run_ddma(value);
    else if (size == 1 && (off == 0x03 || off == 0x1D))
        maybe_boot();
}

/* ---- the pretend chip: when the driver waits, the chip finishes what it was asked to do ---- */
/* the driver's clock: starts at one second, runs with the delays, and the test can push it forward to play minutes of silence */
static unsigned long long UDELAY_US, MS_JUMP;
unsigned long long rtl8821ce_host_ms(void) { return 1000 + UDELAY_US / 1000 + MS_JUMP; }

static int CHIP_COMPLETES = 1;
static unsigned UDELAYS;
void rtl8821ce_host_udelay(unsigned us)
{
    if (CAPTURE)
        cap_ev(2, us, 0, 0);
    UDELAYS++;
    UDELAY_US += us;
    rx_deliver_one();
    if (!KNOB_H2C_STUCK)
        REGS[0x1CC] = 0; /* the firmware takes the commands */
    if (!CHIP_COMPLETES)
        return;
    if (REGS[5] & 2)
        REGS[5] &= (unsigned char)~2; /* power-off: the state machine finishes (bit 1 clears itself) */
    if (REGS[5] & 1)
    {
        REGS[5] &= (unsigned char)~1; /* power-on: the state machine finishes (bit 0 clears itself) */
        REGS[R_CR] = 0x00;            /* ... and the MAC is on */
    }
}

u32 pci_cfg_read(unsigned bus, unsigned dev, unsigned fn, unsigned off)
{
    (void)bus; (void)dev; (void)fn;
    if (off == 4) return CFG_CMD;
    if (off == 0x18) return BAR_KIND == 1 ? 1u : BAR_KIND == 2 ? 0u : ((u32)(unsigned long long)(size_t)REGS | 4u);
    if (off == 0x1C) return BAR_KIND == 0 ? (u32)((unsigned long long)(size_t)REGS >> 32) : 0u;
    return 0;
}
void pci_cfg_write(unsigned bus, unsigned dev, unsigned fn, unsigned off, u32 v)
{
    (void)bus; (void)dev; (void)fn;
    if (off == 4) CFG_CMD = v;
}

/* ---- the pretend drive: one file, RTW8821C.BIN, or none ---- */
static int FILE_PRESENT;
static VfsNode FILE_NODE;
static int FILE_READ_FAIL;     /* 1 = every read fails */
static int READS_LEFT = -1;    /* >= 0: this many reads work, then they fail */
static const unsigned char *FILE_BYTES;
static unsigned FILE_BYTES_LEN;
static char LAST_NAME[32];

/* the settings file WIFI.CFG: a made-up text for the tests (never the user's own file) */
static const char *CFG_TEXT;
static VfsNode CFG_NODE;
int exec_find(const char *name)
{
    if (strcmp(name, "WIFI.CFG") == 0)
        return CFG_TEXT ? 8 : -1;
    strncpy(LAST_NAME, name, sizeof LAST_NAME - 1);
    return FILE_PRESENT && strcmp(name, "RTW8821C.BIN") == 0 ? 7 : -1;
}
const VfsNode *vfs_node(int id)
{
    if (id == 8 && CFG_TEXT)
    {
        memset(&CFG_NODE, 0, sizeof CFG_NODE);
        CFG_NODE.used = 1;
        CFG_NODE.size = (unsigned)strlen(CFG_TEXT);
        return &CFG_NODE;
    }
    return id == 7 && FILE_PRESENT ? &FILE_NODE : 0;
}
int vfs_read(int id, unsigned offset, void *buf, unsigned len)
{
    if (id == 8 && CFG_TEXT)
    {
        unsigned n = (unsigned)strlen(CFG_TEXT);
        if (offset > n)
            return -1;
        if (offset + len > n)
            len = n - offset;
        memcpy(buf, CFG_TEXT + offset, len);
        return (int)len;
    }
    if (READS_LEFT == 0)
        return -1;
    if (READS_LEFT > 0)
        READS_LEFT--;
    if (id != 7 || FILE_READ_FAIL || offset > FILE_BYTES_LEN)
        return -1;
    if (offset + len > FILE_BYTES_LEN)
        len = FILE_BYTES_LEN - offset;
    memcpy(buf, FILE_BYTES + offset, len);
    return (int)len;
}

/* a file with these contents; `size` is what the directory says (it may differ from the contents) */
static void file(const unsigned char *bytes, unsigned len, unsigned size)
{
    FILE_PRESENT = bytes != 0;
    FILE_BYTES = bytes;
    FILE_BYTES_LEN = len;
    memset(&FILE_NODE, 0, sizeof FILE_NODE);
    FILE_NODE.used = 1;
    FILE_NODE.size = size;
}

static const PciDevice WIFI = {1, 0, 0, 0x10EC, 0xC821, 0x02, 0x80, 0x00, 0x00, 0};

/* a chip as the user's PC showed it (cut B). `filler` is what every other register reads as. */
static void chip(unsigned char filler, unsigned char cr, u16 mcufw)
{
    memset(REGS, filler, sizeof REGS);
    set32(R_SYS_CFG1, 0x00491D3Du);
    set32(R_SYS_CFG2, 0x80000009u);
    set16(R_SYS_FUNC_EN, 0x51DD);
    set16(R_SYS_PW_CTRL, 0x0412);
    set16(R_MCUFW_CTRL, mcufw);
    REGS[R_CR] = cr;
    REGS[6] = (unsigned char)(REGS[6] | 0x02); /* "ready" for the power-on poll */
    NLOG = 0;
    UDELAYS = 0;
    PIECES = 0;
    NDDMA = 0;
    BOOTED = IMAGE_OK = 0;
    MODEL_ERRORS = 0;
    BCN_PAGE = 0xFFFF;
    memset(CHIPMEM, 0, sizeof CHIPMEM);
    memset(TXFIFO, 0, sizeof TXFIFO);
    KNOB_NO_BCN_VALID = KNOB_DDMA_STUCK = KNOB_CHKSUM_BAD = KNOB_NO_BOOT = KNOB_EFUSE_STUCK = KNOB_LLT_STUCK = KNOB_H2C_STUCK = 0;
    static const unsigned char TEST_MAC[6] = {0x00, 0xE0, 0x4C, 0x68, 0x12, 0x34};
    build_efuse(TEST_MAC);
    memset(LTEM, 0, sizeof LTEM);
    REGS[0x1703] |= 0x20; /* the indirect interface of the coexistence block is ready */
}

/* the same, for a download test: nothing set that the real chip does not have (filler 0: DDMA idle, no stale flags) */
static void chip_for_download(void)
{
    chip(0x00, 0xEA, 0x0000);
}

static void check_log(const Wr *expect, int n, const char *what)
{
    if (NLOG != n)
    {
        printf("FAIL %s: %d writes, want %d\n", what, NLOG, n);
        fails++;
    }
    for (int i = 0; i < n && i < NLOG; i++)
        if (LOG[i].off != expect[i].off || LOG[i].size != expect[i].size || LOG[i].value != expect[i].value)
        {
            printf("FAIL %s, write %d: got 0x%X size %u value 0x%X, want 0x%X size %u value 0x%X\n", what, i, LOG[i].off, LOG[i].size,
                   LOG[i].value, expect[i].off, expect[i].size, expect[i].value);
            fails++;
        }
}

/* the writes listed must all be in the log, in this order (other writes may lie between) */
static int in_order(const Wr *want, int n, const char *what)
{
    int at = 0;
    for (int i = 0; i < n; i++)
    {
        while (at < NLOG && !(LOG[at].off == want[i].off && LOG[at].size == want[i].size && LOG[at].value == want[i].value))
            at++;
        if (at >= NLOG)
        {
            printf("FAIL %s: write %d (0x%X size %u value 0x%X) not found in order\n", what, i, want[i].off, want[i].size, want[i].value);
            fails++;
            return 0;
        }
        at++;
    }
    return 1;
}

static int count_writes(u32 off, unsigned size)
{
    int n = 0;
    for (int i = 0; i < NLOG; i++)
        n += LOG[i].off == off && LOG[i].size == size;
    return n;
}

/* a beacon of the pretend access point "TestNet" on `chan`, WPA2-PSK/CCMP, with the capabilities of stage 8: legacy rates of the band, HT (MCS 0..7, short GI at 20 MHz) and VHT
 * (receive map `vht_map` for the first stream) when asked */
static unsigned build_beacon_caps(unsigned char *b, unsigned chan, int ht, int vht, unsigned vht_map)
{
    static const unsigned char RSN_OK2[20] = {1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F, 0xAC, 2, 0, 0};
    memset(b, 0, 200);
    b[0] = 0x80;
    memset(b + 4, 0xFF, 6);
    memcpy(b + 10, AP_BSSID, 6);
    memcpy(b + 16, AP_BSSID, 6);
    b[34] = 0x31;
    b[35] = 0x14;
    unsigned p = 36;
    b[p++] = 0;
    b[p++] = 7;
    memcpy(b + p, "TestNet", 7);
    p += 7;
    if (chan > 14)
    {
        static const unsigned char R5[8] = {0x8C, 0x12, 0x98, 0x24, 0xB0, 0x48, 0x60, 0x6C};
        b[p++] = 1;
        b[p++] = 8;
        memcpy(b + p, R5, 8);
        p += 8;
    }
    else
    {
        static const unsigned char R2[8] = {0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24};
        static const unsigned char X2[4] = {0x30, 0x48, 0x60, 0x6C};
        b[p++] = 1;
        b[p++] = 8;
        memcpy(b + p, R2, 8);
        p += 8;
        b[p++] = 50;
        b[p++] = 4;
        memcpy(b + p, X2, 4);
        p += 4;
    }
    b[p++] = 3;
    b[p++] = 1;
    b[p++] = (unsigned char)chan;
    b[p++] = 48;
    b[p++] = 20;
    memcpy(b + p, RSN_OK2, 20);
    p += 20;
    if (ht)
    {
        b[p++] = 45;
        b[p++] = 26;
        b[p + 0] = 0x2C; /* short GI at 20 MHz */
        b[p + 3] = 0xFF; /* MCS 0..7 */
        p += 26;
    }
    if (vht)
    {
        b[p++] = 191;
        b[p++] = 12;
        b[p + 4] = (unsigned char)vht_map;
        b[p + 5] = (unsigned char)(vht_map >> 8);
        p += 12;
    }
    p += 4; /* the FCS */
    return p;
}

/* the information element `tag` of a management frame body that starts at `from`; 0 if there is none */
static const unsigned char *find_ie(const unsigned char *f, unsigned len, unsigned from, unsigned tag)
{
    for (unsigned p = from; p + 2 <= len; p += 2 + f[p + 1])
        if (f[p] == tag)
            return f + p;
    return 0;
}

int main(void)
{
    TXP_ENABLE = 0; /* the probe-request test (stage 5a) has its own section below */
    ONLINE_ENABLE = 0; /* the data path has its own section below */
    /* ---- the claim ---- */
    PciDevice p = WIFI;
    CHECK(NIC_RTL8821CE.claims(&p));
    p.device_id = 0xB821;
    CHECK(NIC_RTL8821CE.claims(&p)); /* both ids of the 8821CE */
    p.device_id = 0x8168;
    CHECK(!NIC_RTL8821CE.claims(&p)); /* the Ethernet chip belongs to another driver */
    p = WIFI;
    p.vendor_id = 0x8086;
    CHECK(!NIC_RTL8821CE.claims(&p));

    char st[96];

    /* ---- the tables: sizes (the contents are checked against Linux' own tables by compiling them) ---- */
    CHECK(NPOWER_ON == 18 && NPOWER_OFF == 12);
    CHECK(DMA_PAGES == 107);

    /* ---- MAC off (CR = 0xEA), no firmware file: the power-on sequence, write by write ---- */
    file(0, 0, 0);
    chip(0xFF, 0xEA, 0x0078);
    CHECK(NIC_RTL8821CE.probe(&WIFI) == 0);          /* never a usable adapter yet */
    CHECK((CFG_CMD & 2) == 2);                       /* memory space enabled */
    static const Wr ON_ONLY[] = {
        /* rtw_mac_pre_system_cfg */
        {0x1C, 1, 0x00}, {0x74, 4, 0xFFFFFFFFu}, {0x64, 4, 0xFFFFFFFFu}, {0x4C, 4, 0xF9FFFFFFu}, {0x40, 4, 0xFFFFFFFFu},
        {0x02, 1, 0xDC}, {0x1F, 1, 0xF8}, {0xEC, 4, 0xF8FFFFFFu}, /* byte 2 is 0xDD in this chip: bits 0 and 1 cleared */
        /* trans_carddis_to_cardemu_8821c (PCIe steps) */
        {0x05, 1, 0x04}, {0x300, 1, 0x00}, {0x301, 1, 0x00}, /* byte 5 is 0x04 in this chip: bits 3, 4 and 7 are clear already */
        /* trans_cardemu_to_act_8821c (PCIe steps); the poll of byte 6 and the poll of byte 5 write nothing */
        {0x05, 1, 0x00}, {0x75, 1, 0xFF}, {0x75, 1, 0xFE}, {0x06, 1, 0xFF}, {0x05, 1, 0x00}, {0x05, 1, 0x00}, {0x05, 1, 0x01},
        {0x20, 1, 0xFF}, {0x74, 1, 0xFF}, {0x22, 1, 0xFD}, {0x62, 1, 0xFF}, {0x61, 1, 0x1F}, {0x7C, 1, 0xFD},
        /* __rtw_mac_init_system_cfg: the platform reset and DDMA bits, the blocks of the high byte of SYS_FUNC_EN, CR_EXT, no flash boot */
        {0x1080, 4, 0xFFFFFFFFu}, {0x03, 1, 0xD9}, {0x1103, 1, 0xFC},
        {0x80, 4, 0xFFEF0078u}, {0x40, 4, 0xFFF7FFFFu}, /* MCUFW_CTRL bit 20 (boot from flash) was set in this filler: cleared, and the flash pad bit */
    };
    check_log(ON_ONLY, (int)(sizeof ON_ONLY / sizeof ON_ONLY[0]) - 0, "power-on");
    CHECK(UDELAYS >= 1 && UDELAYS < 10);            /* the poll of byte 5 waited once: the chip had finished by then */
    NIC_RTL8821CE.status(st, sizeof st);
    CHECK(strstr(st, "RTL8821CE (no driver)") && strstr(st, "cut B") && strstr(st, "was off") && strstr(st, "powered on"));
    CHECK(strstr(st, "CR=00") && strstr(st, "02=") && strstr(st, "04=") && strstr(st, ", no RTW8821C.BIN") && strlen(st) < sizeof st);
    CHECK(!NIC_RTL8821CE.link_up() && NIC_RTL8821CE.send(st, 60) == -1 && NIC_RTL8821CE.recv(st, 60) == 0);
    NIC_RTL8821CE.status(st, 20); /* a short buffer is cut and terminated */
    CHECK(strlen(st) == 19);

    /* ---- MAC already on (what a warm reboot shows): off, then on, with the pre-configuration before each, like Linux ---- */
    chip(0x5A, 0x00, 0x0078);
    CHECK(NIC_RTL8821CE.probe(&WIFI) == 0);
    /* 8 (pre) + 11 writes of the off flow + 8 (pre) + 16 writes of the on flow + 5 of the system configuration; the polls write nothing */
    CHECK(NLOG == 8 + 11 + 8 + 16 + 5);
    static const unsigned OFF_ORDER[] = {0x93, 0x1F, 0x49, 0x06, 0x02, 0x05, 0x20, 0x67, 0x05, 0x81, 0x90};
    CHECK(LOG[0].off == 0x1C && LOG[8].off == 0x93);                        /* pre-configuration, then the off flow */
    for (unsigned i = 0; i < sizeof OFF_ORDER / sizeof OFF_ORDER[0]; i++)
        CHECK(LOG[8 + i].off == OFF_ORDER[i]);
    CHECK(LOG[19].off == 0x1C && LOG[27].off == 0x05 && LOG[27 + 15].off == 0x7C); /* again the pre-configuration, then the on flow */
    NIC_RTL8821CE.status(st, sizeof st);
    CHECK(strstr(st, "was on") && strstr(st, "powered on") && strstr(st, "CR=00") && strlen(st) < sizeof st);

    /* ---- MAC on and an older firmware resident (0xC078): Linux would first tell it to stop; that is not done, so nothing is touched ---- */
    chip(0x5A, 0x00, 0xC078);
    memcpy(SNAP, REGS, sizeof REGS);
    CHECK(NIC_RTL8821CE.probe(&WIFI) == 0);
    CHECK(memcmp(SNAP, REGS, sizeof REGS) == 0 && NLOG == 0);
    NIC_RTL8821CE.status(st, sizeof st);
    CHECK(strstr(st, "(firmware resident: left alone)") && strstr(st, "CR=00") && strstr(st, "80=c078") && strlen(st) < sizeof st);

    /* ---- failing power-on: the chip never says "ready" (bit 1 of byte 6 stays 0): the driver gives up at that step ---- */
    chip(0xFF, 0xEA, 0x0078);
    REGS[6] = 0x00;
    CHIP_COMPLETES = 0;
    CHECK(NIC_RTL8821CE.probe(&WIFI) == 0);
    CHECK(UDELAYS >= 2 * POLL_TRIES - 2 && UDELAYS <= 2 * POLL_TRIES);        /* a second, toggle, another second */
    NIC_RTL8821CE.status(st, sizeof st);
    CHECK(strstr(st, "power-on FAILED at step 6") && strlen(st) < sizeof st); /* the sixth command is that poll */
    int toggles = 0;
    for (int i = 0; i < NLOG; i++)
        toggles += LOG[i].off == 0x04 && LOG[i].size == 1;
    CHECK(toggles == 2);                                                      /* BIT_PFM_WOWL set, then cleared, once */
    CHECK(LOG[NLOG - 1].off == 0x04 && (LOG[NLOG - 1].value & BIT(3)) == 0);
    CHECK(NLOG < 20);                                                         /* and nothing after the failed step */

    /* ---- failing power-off (MAC on): that poll is the seventh step of the off flow, and the on flow is not started ---- */
    chip(0x5A, 0x00, 0x0078);
    CHIP_COMPLETES = 0;
    CHECK(NIC_RTL8821CE.probe(&WIFI) == 0);
    NIC_RTL8821CE.status(st, sizeof st);
    CHECK(strstr(st, "was on") && strstr(st, "power-off FAILED at step 7") && strlen(st) < sizeof st);
    for (int i = 0; i < NLOG; i++)
        CHECK(LOG[i].off != 0x300);                                           /* nothing of the on flow was written */
    CHIP_COMPLETES = 1;

    /* ---- the firmware file (stage 3a): found and checked. The real header, a size that matches it, contents that do not matter here
     * because the download is not what is looked at (it fails at the first read: the "file" has only a header). ---- */
    static const unsigned char REAL_HDR[FW_HDR_SIZE] = {
        0x21, 0x88, 0x00, 0x00, 0x18, 0x00, 0x0b, 0x00, 0x0f, 0x5c, 0xde, 0x0a, 0x07, 0x00, 0x00, 0x00,
        0x01, 0x0a, 0x0f, 0x10, 0xe6, 0x07, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x20, 0x80, 0xe8, 0x7c, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x08, 0xf7, 0x00, 0x00, 0x88, 0xac, 0x00, 0x00, 0x00, 0x00, 0x10, 0x80, 0x00, 0x00, 0x03, 0x80,
    };
    chip_for_download();
    file(REAL_HDR, FW_HDR_SIZE, 139472); /* the real file is 139472 bytes; here only its header can be read */
    NIC_RTL8821CE.probe(&WIFI);
    CHECK(strcmp(LAST_NAME, "RTW8821C.BIN") == 0);
    CHECK(FW.state == FW_OK && FW.version == 24 && FW.subversion == 11 && FW.year == 2022 && FW.month == 1 && FW.day == 10);
    CHECK(FW.dmem == 31976 && FW.imem == 63240 && FW.emem == 44168);
    CHECK(FW.dmem_addr == 0x80200000u && FW.imem_addr == 0x80030000u && FW.emem_addr == 0x80100000u);
    CHECK(DL.state == DL_FAILED && strcmp(DL.why, "file read") == 0); /* the download starts and cannot read past the header */
    NIC_RTL8821CE.status(st, sizeof st);
    CHECK(strstr(st, "load FAILED: file read in dmem+0") && strlen(st) < sizeof st);
    CHECK(DL.off_after == 1);                                           /* and it powered the chip off again */

    /* a wrong size, a short file, a wrong signature, absurd part sizes, a folder of that name */
    unsigned char hdr[FW_HDR_SIZE];
    chip_for_download();
    file(REAL_HDR, FW_HDR_SIZE, 139471);
    NIC_RTL8821CE.probe(&WIFI);
    NIC_RTL8821CE.status(st, sizeof st);
    CHECK(FW.state == FW_BAD && strstr(st, ", bad fw: file size") && strlen(st) < sizeof st && DL.state == DL_NOT_TRIED);
    chip_for_download();
    file(REAL_HDR, 10, 10);
    NIC_RTL8821CE.probe(&WIFI);
    NIC_RTL8821CE.status(st, sizeof st);
    CHECK(FW.state == FW_BAD && strstr(st, ", bad fw: too short"));
    memcpy(hdr, REAL_HDR, sizeof hdr);
    hdr[0] = 0x22;
    chip_for_download();
    file(hdr, FW_HDR_SIZE, 139472);
    NIC_RTL8821CE.probe(&WIFI);
    NIC_RTL8821CE.status(st, sizeof st);
    CHECK(FW.state == FW_BAD && strstr(st, ", bad fw: signature"));
    memcpy(hdr, REAL_HDR, sizeof hdr);
    hdr[0x27] = 0x7F; /* dmem size 0x7F007CE8 */
    chip_for_download();
    file(hdr, FW_HDR_SIZE, 139472);
    NIC_RTL8821CE.probe(&WIFI);
    NIC_RTL8821CE.status(st, sizeof st);
    CHECK(FW.state == FW_BAD && strstr(st, ", bad fw: part sizes"));
    chip_for_download();
    file(REAL_HDR, FW_HDR_SIZE, 0);
    FILE_NODE.is_dir = 1;
    NIC_RTL8821CE.probe(&WIFI);
    CHECK(FW.state == FW_MISSING);
    file(0, 0, 0);

    /* ---- stage 3b: the whole download against the model ---- */
    build_image(1);
    chip_for_download();
    file(IMG, IMG_SIZE, IMG_SIZE);
    CHECK(NIC_RTL8821CE.probe(&WIFI) == 0);
    CHECK((CFG_CMD & 6) == 6);                                       /* memory space and bus master */
    CHECK(MODEL_ERRORS == 0);
    CHECK(DL.state == DL_OK && DL.why == 0);
    CHECK(PIECES == 2 + 3 + 1);                                      /* 5008 = 4096 + 912, 9008 = 4096 + 4096 + 816, 308 */
    CHECK(NDDMA == 6);
    CHECK(DDMA[0].dst == 0x200000 && DDMA[0].len == 4096 && !DDMA[0].cont);   /* the first piece of a part starts a new checksum */
    CHECK(DDMA[1].dst == 0x200000 + 4096 && DDMA[1].len == 912 && DDMA[1].cont);
    CHECK(DDMA[2].dst == 0x30000 && DDMA[2].len == 4096 && !DDMA[2].cont);
    CHECK(DDMA[3].dst == 0x30000 + 4096 && DDMA[3].len == 4096 && DDMA[3].cont);
    CHECK(DDMA[4].dst == 0x30000 + 8192 && DDMA[4].len == 816 && DDMA[4].cont);
    CHECK(DDMA[5].dst == 0x100000 && DDMA[5].len == 308 && !DDMA[5].cont);
    CHECK(IMAGE_OK && BOOTED);                                       /* the chip's memory holds the three parts exactly */
    CHECK(memcmp(CHIPMEM + 0x200000, IMG + 64, SYN_DMEM + 8) == 0);
    CHECK(DL.off_after == 1);
    CHECK((DL.mcufw & 0xC078) == 0xC078);
    NIC_RTL8821CE.status(st, sizeof st);
    CHECK(strstr(st, "cut B, MAC 00:e0:4c:68:12:34, init ok, 0ap, no cfg, off") &&
          strlen(st) < sizeof st);
    /* the order of the register sequence, by landmarks */
    static const Wr LANDMARKS[] = {
        /* wlan_cpu_enable(false): the high byte of SYS_FUNC_EN keeps its bits (the CPU bit was clear), RSV_CTRL+1 */
        {0x03, 1, 0xD9}, {0x1D, 1, 0x00},
        /* download_firmware_reg_backup */
        {0x10D, 1, 0xC0}, {0x100, 1, 0x05}, {0x1330, 4, 0x80000000u}, {0x230, 2, 0x200}, {0x22C, 4, 0x80000000u}, {0x550, 1, 0x10},
        /* download_firmware_reset_platform: PLATFORM_RST and CPU_CLK_EN cleared, then set */
        {0x1082, 1, 0x00}, {0x09, 1, 0x00}, {0x1082, 1, 0x01}, {0x09, 1, 0x40},
        {0x80, 2, 0x0001},                              /* MCUFWDL_EN */
        /* download_firmware_reg_restore */
        {0x10D, 1, 0x00}, {0x100, 1, 0x00}, {0x1330, 4, 0x80000000u}, {0x230, 2, 0x0000}, {0x22C, 4, 0x80000000u}, {0x550, 1, 0x00},
        /* download_firmware_end_flow, then the CPU on */
        {0x210, 4, 0x4}, {0x80, 2, 0x4078}, {0x1D, 1, 0x01}, {0x03, 1, 0xDD},
    };
    in_order(LANDMARKS, (int)(sizeof LANDMARKS / sizeof LANDMARKS[0]), "download landmarks");
    CHECK(count_writes(0x383, 1) == 6);                              /* one kick per piece */
    CHECK(count_writes(0x1208, 4) == 6 + 3);                         /* the six commands and the checksum reset of each of the three parts */

    /* ---- failures of stage 3b: each says where, and the chip is powered off again ---- */
    build_image(1);
    chip_for_download();
    file(IMG, IMG_SIZE, IMG_SIZE);
    KNOB_NO_BCN_VALID = 1; /* the chip never says the piece arrived */
    NIC_RTL8821CE.probe(&WIFI);
    NIC_RTL8821CE.status(st, sizeof st);
    CHECK(DL.state == DL_FAILED && strstr(st, "load FAILED: send timeout in dmem+0") && DL.off_after == 1 && strlen(st) < sizeof st);
    CHECK(PIECES == 0 && NDDMA == 0);
    chip_for_download();
    file(IMG, IMG_SIZE, IMG_SIZE);
    KNOB_DDMA_STUCK = 1;
    NIC_RTL8821CE.probe(&WIFI);
    NIC_RTL8821CE.status(st, sizeof st);
    CHECK(DL.state == DL_FAILED && strstr(st, "load FAILED: ddma timeout in dmem+0") && strlen(st) < sizeof st);
    chip_for_download();
    file(IMG, IMG_SIZE, IMG_SIZE);
    KNOB_CHKSUM_BAD = 1;
    NIC_RTL8821CE.probe(&WIFI);
    NIC_RTL8821CE.status(st, sizeof st);
    CHECK(DL.state == DL_FAILED && strstr(st, "load FAILED: checksum in dmem+") && NDDMA == 2 && strlen(st) < sizeof st);
    chip_for_download();
    file(IMG, IMG_SIZE, IMG_SIZE);
    KNOB_NO_BOOT = 1;
    NIC_RTL8821CE.probe(&WIFI);
    NIC_RTL8821CE.status(st, sizeof st);
    CHECK(DL.state == DL_FAILED && strstr(st, "load FAILED: firmware not ready (80=") && !strstr(st, " in ") && strlen(st) < sizeof st);
    CHECK(IMAGE_OK && !BOOTED);                                      /* all of it was copied, the CPU just did not report */
    chip_for_download();
    file(IMG, IMG_SIZE - 100, IMG_SIZE);                             /* the file is shorter than its directory entry says */
    NIC_RTL8821CE.probe(&WIFI);
    NIC_RTL8821CE.status(st, sizeof st);
    CHECK(DL.state == DL_FAILED && strstr(st, "load FAILED: file read in emem+") && strlen(st) < sizeof st);
    chip_for_download();
    file(IMG, IMG_SIZE, IMG_SIZE);
    READS_LEFT = 1; /* the header is read for the check, the next read fails */
    NIC_RTL8821CE.probe(&WIFI);
    CHECK(DL.state == DL_FAILED && strcmp(DL.why, "file read") == 0);
    READS_LEFT = -1;
    DMA_FAIL = 1;                                                    /* no DMA memory below 4 GiB: the power-on still happens */
    D.ready = 0;
    chip_for_download();
    file(IMG, IMG_SIZE, IMG_SIZE);
    NIC_RTL8821CE.probe(&WIFI);
    NIC_RTL8821CE.status(st, sizeof st);
    CHECK(DL.state == DL_SKIPPED && FW.state == FW_OK && W.pwr_state == PWR_OK);
    CHECK(strstr(st, "not loaded: no DMA memory below 4 GiB") && strstr(st, "fw 24.11 ok") && strlen(st) < sizeof st);
    DMA_FAIL = 0;
    D.ready = 0;

    /* ---- stage 4a: the efuse map, pure decoding first ---- */
    {
        static const unsigned char MAC[6] = {0x00, 0xE0, 0x4C, 0x68, 0x12, 0x34};
        unsigned char lg[EFUSE_LOGICAL];
        memset(lg, 0xFF, sizeof lg);
        build_efuse(MAC);
        CHECK(efuse_logical(EFUSE_MAP, lg) == 0);
        CHECK(lg[8] == 0xAA && lg[9] == 0xBB && lg[12] == 0xCC && lg[13] == 0xDD); /* 1-byte header 0x1A: offset 1, words 0 and 2 */
        CHECK(lg[10] == 0xFF && lg[11] == 0xFF);                                    /* disabled words stay blank */
        CHECK(memcmp(lg + EFUSE_MAC_AT, MAC, 6) == 0);                              /* 2-byte header: MAC at 0xD0 */
        unsigned char bad[512];
        memset(bad, 0xFF, sizeof bad);
        bad[0] = 0xEF; /* 2-byte header, block 0x7F: its words would land at logical 1016, past the 512-byte map */
        bad[1] = 0xF0;
        CHECK(efuse_logical(bad, lg) == -1);
    }

    /* the full read through the chip's register: MAC in the status, then failures say why */
    build_image(1);
    chip_for_download();
    file(IMG, IMG_SIZE, IMG_SIZE);
    NIC_RTL8821CE.probe(&WIFI);
    CHECK(EF.state == EF_OK && memcmp(EF.mac, "\x00\xE0\x4C\x68\x12\x34", 6) == 0 && MODEL_ERRORS == 0);
    chip_for_download();
    file(IMG, IMG_SIZE, IMG_SIZE);
    KNOB_EFUSE_STUCK = 1;
    NIC_RTL8821CE.probe(&WIFI);
    NIC_RTL8821CE.status(st, sizeof st);
    CHECK(DL.state == DL_OK && EF.state == EF_FAILED && strstr(st, ", efuse: read timeout") && strstr(st, ", off") && strlen(st) < sizeof st);
    {
        static const unsigned char MCAST[6] = {0x01, 0xE0, 0x4C, 0x68, 0x12, 0x34};
        static const unsigned char ZERO[6] = {0, 0, 0, 0, 0, 0};
        chip_for_download();
        build_efuse(MCAST);
        file(IMG, IMG_SIZE, IMG_SIZE);
        NIC_RTL8821CE.probe(&WIFI);
        NIC_RTL8821CE.status(st, sizeof st);
        CHECK(EF.state == EF_FAILED && strstr(st, ", efuse: no valid MAC") && strlen(st) < sizeof st);
        chip_for_download();
        build_efuse(ZERO);
        file(IMG, IMG_SIZE, IMG_SIZE);
        NIC_RTL8821CE.probe(&WIFI);
        CHECK(EF.state == EF_FAILED && strcmp(EF.why, "no valid MAC") == 0);
    }

    /* ---- stage 4b-1: rtw_mac_init against Linux's own code. The writes below were produced by running Linux' rtw_init_trx_cfg,
     * rtw8821c_mac_init and rtw_drv_info_cfg (rtw88 sources) with a plain register file behind them (reads return what was written; BIT_AUTO_INIT_LLT_V1 clears itself); mine must write the same, in the
     * same order. (The harness is not part of the repo.) ---- */
    static const Wr MAC_INIT_LINUX[] = {
    {0x10C, 2, 0xC5A0U}, {0x100, 1, 0x0U}, {0x100, 1, 0xFFU}, {0x1330, 4, 0x80000000U},
    {0x230, 2, 0x10U}, {0x234, 2, 0x10U}, {0x238, 2, 0x10U}, {0x23C, 2, 0xEU},
    {0x240, 2, 0x18DU}, {0x22C, 4, 0x80000000U}, {0x204, 2, 0x1CCU}, {0x422, 1, 0x10U},
    {0x424, 2, 0x1CCU}, {0x206, 2, 0x1CCU}, {0x456, 2, 0x1CCU}, {0x11C, 4, 0x3EFFU},
    {0x208, 1, 0x1U}, {0x103, 1, 0x0U}, {0x244, 4, 0xFA00U}, {0x24C, 4, 0xFA00U},
    {0x248, 4, 0xFE00U}, {0x254, 1, 0x1U}, {0x254, 1, 0x5U}, {0x20D, 1, 0x80U},
    {0x455, 1, 0x70U}, {0x45E, 1, 0x4U}, {0x4E5, 1, 0xE4U}, {0x4E6, 1, 0x9U},
    {0x4C8, 4, 0x202008FFU}, {0x4CE, 2, 0x801U}, {0x1448, 1, 0x6U}, {0x144A, 1, 0x6U},
    {0x144C, 1, 0x6U}, {0x144E, 1, 0x6U}, {0x480, 1, 0x20U}, {0x5B4, 1, 0x0U},
    {0x522, 2, 0x0U}, {0x51B, 1, 0x9U}, {0x512, 1, 0x19U}, {0x514, 4, 0x10100E0AU},
    {0x502, 2, 0x186U}, {0x506, 2, 0x3BCU}, {0x544, 4, 0x1B0005U}, {0x55E, 2, 0x3030U},
    {0x550, 1, 0x8U}, {0x540, 4, 0x6404U}, {0x558, 1, 0x4U}, {0x559, 1, 0x2U},
    {0x521, 1, 0x0U}, {0x6A0, 2, 0xFFFFU}, {0x6A2, 2, 0xFFFU}, {0x6A4, 2, 0xFFFFU},
    {0x608, 4, 0xE400220EU}, {0x60C, 1, 0x18U}, {0x606, 1, 0x30U}, {0x605, 1, 0x30U},
    {0x639, 1, 0x40U}, {0x66C, 1, 0x2U}, {0x718, 1, 0x40U}, {0x7D8, 4, 0xB0810041U},
    {0x7D4, 1, 0x98U}, {0x60F, 1, 0x4U}, {0x115, 1, 0xFU}, {0x608, 4, 0xF400220EU},
    {0x7D4, 4, 0x98U},    };
    chip_for_download();
    memset(REGS, 0, sizeof REGS);
    NLOG = 0;
    MI.state = MI_NOT_TRIED;
    mac_init();
    CHECK(MI.state == MI_OK && MI.why == 0);
    /* exactly Linux's writes: the DMA engines are left on (see the final CR write in probe()) */
    check_log(MAC_INIT_LINUX, (int)(sizeof MAC_INIT_LINUX / sizeof MAC_INIT_LINUX[0]), "mac init vs Linux");

    /* the auto-LLT never finishes: the init stops there, and says so */
    chip_for_download();
    memset(REGS, 0, sizeof REGS);
    KNOB_LLT_STUCK = 1;
    MI.state = MI_NOT_TRIED;
    mac_init();
    CHECK(MI.state == MI_FAILED && strcmp(MI.why, "llt") == 0);
    CHECK(count_writes(0x608, 4) == 0);                                         /* never got as far as the receive filter */
    KNOB_LLT_STUCK = 0;

    /* a command ring that does not start empty */
    chip_for_download();
    memset(REGS, 0, sizeof REGS);
    set32(0x10D4, 0x100); /* the write pointer is ahead of the read pointer */
    MI.state = MI_NOT_TRIED;
    mac_init();
    CHECK(MI.state == MI_FAILED && strcmp(MI.why, "h2c") == 0);

    /* ---- stage 4b-2: the baseband / AGC / RF tables against Linux's own interpreter. The expected event counts and hashes (every
     * register write and every delay, in order) come from running Linux' rtw_phy_load_tables / rtw_parse_tbl_phy_cond / rtw_phy_cfg_*
     * / rtw_phy_write_rf_reg_sipi and the first half of rtw8821c_phy_set_param on the real rtw8821c_table.c, cut B, PCIe, package 0,
     * with a plain register file behind them. (The harness is not part of the repo.) ---- */
    static const struct { unsigned rfe, xtal, pkg; long events; unsigned long long hash; } PHY_LINUX[] = {
        {4, 0x2A, 0, 4076, 0xa448cd59c4217c59ULL}, /* rfe 2 and 4 load the extra BTG AGC table, too */
        {2, 0x7F, 0, 4076, 0x7bff35ce73685275ULL}, /* 0x7F & 0x3F = 0x3F reaches the crystal registers */
        {0, 0x2A, 0, 3948, 0x1573d11c9c443c9cULL},
        {6, 0x2A, 0, 3948, 0x1573d11c9c443c9cULL},
        {0, 0x15, 0, 3948, 0x0376394a56f3effcULL},
        {4, 0x2A, 1, 4076, 0x0827f572d5a8e151ULL}, /* package type 1 (efuse 0xCA bit 5) selects other blocks of the tables */
    };
    for (unsigned k = 0; k < sizeof PHY_LINUX / sizeof PHY_LINUX[0]; k++)
    {
        chip_for_download();
        memset(REGS, 0, sizeof REGS);
        EF.map_ok = 1;
        EF.rfe = (unsigned char)PHY_LINUX[k].rfe;
        EF.pkg = (unsigned char)PHY_LINUX[k].pkg;
        EF.xtal = (unsigned char)PHY_LINUX[k].xtal;
        CAP_N = 0;
        CAP_H = 1469598103934665603ULL;
        CAPTURE = 1;
        phy_init(1); /* chip cut B */
        CAPTURE = 0;
        if (CAP_N != PHY_LINUX[k].events || CAP_H != PHY_LINUX[k].hash)
        {
            printf("FAIL phy tables rfe %u xtal 0x%X: %ld events hash %016llx, Linux %ld events hash %016llx\n", PHY_LINUX[k].rfe, PHY_LINUX[k].xtal,
                   CAP_N, CAP_H, PHY_LINUX[k].events, PHY_LINUX[k].hash);
            fails++;
        }
        CHECK(PH.state == PH_OK && PH.rf_diff == 0 && PH.rf_written > 100);
    }
    /* what cannot be done: no efuse map, or a board type that Linux has no tables for */
    chip_for_download();
    memset(REGS, 0, sizeof REGS);
    EF.map_ok = 0;
    NLOG = 0;
    phy_init(1);
    CHECK(PH.state == PH_SKIPPED && strcmp(PH.why, "efuse") == 0 && NLOG == 0);
    EF.map_ok = 1;
    EF.rfe = 5;
    phy_init(1);
    CHECK(PH.state == PH_SKIPPED && strcmp(PH.why, "rfe") == 0 && NLOG == 0);
    /* a few RF registers whose bits the chip changes itself: counted, but the stage is fine */
    chip_for_download();
    memset(REGS, 0, sizeof REGS);
    EF.map_ok = 1;
    EF.rfe = 0;
    EF.xtal = 0x2A;
    PH_FLIP_RF_BITS = 0x3000;
    phy_init(1);
    PH_FLIP_RF_BITS = 0;
    CHECK(PH.state == PH_OK && PH.rf_diff == 2 && PH.diff_addr[0] == 0x18 && PH.diff_xor[0] == 0x3000 && !phy_rf_suspect());
    {
        char s3[96];
        DL.state = DL_OK;
        DL.off_after = 1;
        NIC_RTL8821CE.status(s3, sizeof s3);
        CHECK(strstr(s3, ", phy ok") && !strstr(s3, "phy rf") && strlen(s3) < sizeof s3);
    }
    /* an RF register that does not hold what was written is counted */
    chip_for_download();
    memset(REGS, 0, sizeof REGS);
    EF.map_ok = 1;
    EF.rfe = 0;
    EF.xtal = 0x2A;
    PH_BREAK_RF_READBACK = 1;
    phy_init(1);
    PH_BREAK_RF_READBACK = 0;
    CHECK(PH.state == PH_OK && PH.rf_diff > 0 && phy_rf_suspect());
    {
        char s2[96];
        DL.state = DL_OK;
        DL.off_after = 1;
        NIC_RTL8821CE.status(s2, sizeof s2);
        CHECK(strstr(s2, ", phy rf") && strchr(s2, ':') && strstr(s2, ", off") && strlen(s2) < sizeof s2);
        CHECK(PH.diff_addr[0] == 0x00); /* RF register 0 is the first one written, and the first that failed to read back here */
    }

    /* ---- stage 4b-3: the mailbox command. Sequence as in rtw_fw_send_h2c_command: wait for the box to be free, high word first,
     * then the low word with the command id; then the firmware must clear the box bit. ---- */
    chip_for_download();
    memset(REGS, 0, sizeof REGS);
    NLOG = 0;
    h2c_probe();
    CHECK(H2.state == H2_OK && H2.why == 0);
    CHECK(NLOG == 2 && LOG[0].off == 0x1F0 && LOG[0].size == 4 && LOG[0].value == 0 && LOG[1].off == 0x1D0 && LOG[1].size == 4 &&
          LOG[1].value == 0x63);
    CHECK((REGS[0x1CC] & 1) == 0);
    chip_for_download();
    memset(REGS, 0, sizeof REGS);
    KNOB_H2C_STUCK = 1; /* the firmware does not answer */
    NLOG = 0;
    h2c_probe();
    CHECK(H2.state == H2_FAILED && strcmp(H2.why, "no ack") == 0 && NLOG == 2);
    {
        char s4[96];
        PH.state = PH_OK;
        PH.rf_diff = 0;
        MI.state = MI_OK;
        EF.state = EF_OK;
        DL.state = DL_OK;
        DL.off_after = 1;
        CHN.state = CH_NOT_TRIED;
        RX.state = RXS_NOT_TRIED;
        SC.state = 0;
        NIC_RTL8821CE.status(s4, sizeof s4);
        CHECK(strstr(s4, ", h2c no ack, off") && strlen(s4) < sizeof s4);
    }
    chip_for_download();
    memset(REGS, 0, sizeof REGS);
    REGS[0x1CC] = 1; /* the box is still occupied by an earlier command */
    KNOB_H2C_STUCK = 1;
    NLOG = 0;
    h2c_probe();
    CHECK(H2.state == H2_FAILED && strcmp(H2.why, "busy") == 0 && NLOG == 0); /* nothing is written into a busy box */
    KNOB_H2C_STUCK = 0;

    /* ---- stage 4c-1: the antenna path and the channel against Linux's own functions (rtw_coex_set_ant_path for the Wi-Fi-only
     * phase, rtw8821c_set_channel), run after the table load, with the same kind of register file behind them (including the
     * indirect interface of the coexistence block). Event count and hash as for the tables. ---- */
    static const struct
    {
        unsigned rfe, channel, swing2g;
        long coex_events;
        unsigned long long coex_hash;
        long ch_events;
        unsigned long long ch_hash;
    } CH_LINUX[] = {
        {4, 6, 0xFF, 19, 0x44792c9b1eb830c0ULL, 37, 0x8451c084ab98df15ULL},
        {4, 1, 0xFF, 19, 0x44792c9b1eb830c0ULL, 37, 0x0e09fe3ab8371a7eULL},
        {4, 14, 0xFF, 19, 0x44792c9b1eb830c0ULL, 37, 0xf7cb7fb48955e3f1ULL}, /* channel 14 has its own CCK filter */
        {4, 36, 0xFF, 19, 0x44792c9b1eb830c0ULL, 30, 0x40fdb8d19ff2a1e9ULL}, /* 5 GHz: the WLA input */
        {4, 149, 0xFF, 19, 0x44792c9b1eb830c0ULL, 30, 0xd5665caffaa4962bULL},
        {0, 6, 0xFF, 19, 0x44792c9b1eb830c0ULL, 37, 0x85e3c5e0344f82adULL}, /* not a BTG board: the WLG input */
        {2, 11, 0xFF, 19, 0x44792c9b1eb830c0ULL, 37, 0xb751c9f5e1c3d5c8ULL},
        {6, 40, 0xFF, 13, 0x53efffcdf6038b21ULL, 30, 0xb01e580961fb5c5dULL}, /* no antenna switch on this board type */
        {4, 6, 4, 19, 0x44792c9b1eb830c0ULL, 37, 0x51177f14eae9573eULL},     /* another bb swing setting from the efuse */
    };
    for (unsigned k = 0; k < sizeof CH_LINUX / sizeof CH_LINUX[0]; k++)
    {
        chip_for_download();
        memset(REGS, 0, sizeof REGS);
        memset(LTEM, 0, sizeof LTEM);
        REGS[0x1703] |= 0x20; /* the indirect interface is ready */
        EF.map_ok = 1;
        EF.rfe = (unsigned char)CH_LINUX[k].rfe;
        EF.pkg = 0;
        EF.xtal = 0x2A;
        EF.swing2g = (unsigned char)CH_LINUX[k].swing2g;
        EF.swing5g = 0xFF;
        phy_init(1);
        CAP_N = 0;
        CAP_H = 1469598103934665603ULL;
        CAPTURE = 1;
        coex_wifi_only(EF.rfe);
        CAPTURE = 0;
        if (CAP_N != CH_LINUX[k].coex_events || CAP_H != CH_LINUX[k].coex_hash)
        {
            printf("FAIL coex wifi-only rfe %u: %ld events hash %016llx, Linux %ld events hash %016llx\n", CH_LINUX[k].rfe, CAP_N, CAP_H,
                   CH_LINUX[k].coex_events, CH_LINUX[k].coex_hash);
            fails++;
        }
        CAP_N = 0;
        CAP_H = 1469598103934665603ULL;
        CAPTURE = 1;
        set_channel_20(CH_LINUX[k].channel, PH.ch_param, EF.swing2g, EF.swing5g);
        CAPTURE = 0;
        if (CAP_N != CH_LINUX[k].ch_events || CAP_H != CH_LINUX[k].ch_hash)
        {
            printf("FAIL channel %u rfe %u swing %u: %ld events hash %016llx, Linux %ld events hash %016llx\n", CH_LINUX[k].channel,
                   CH_LINUX[k].rfe, CH_LINUX[k].swing2g, CAP_N, CAP_H, CH_LINUX[k].ch_events, CH_LINUX[k].ch_hash);
            fails++;
        }
    }
    /* the full flow tunes channel 6 and checks it by reading RF register 0x18 back */
    build_image(1);
    chip_for_download();
    file(IMG, IMG_SIZE, IMG_SIZE);
    NIC_RTL8821CE.probe(&WIFI);
    CHECK(CHN.state == CH_OK && CHN.channel == 6 && (CHN.rf18 & 0xFF) == 6);
    CHECK(CX.ant_switch_exist && CX.wlg_at_btg && CX.polarity == 1 && CX.rfe_btg); /* rfe 4 */
    {
        char s5[96];
        NIC_RTL8821CE.status(s5, sizeof s5);
        CHECK(strstr(s5, ", init ok, 0ap, no cfg, off") && strlen(s5) < sizeof s5);
    }

    /* ---- stage 4c-2: receive. The pretend chip delivers the frames the test queued; the driver must classify them, hand the
     * buffers back and report how far it has read. ---- */
    build_image(1);
    chip_for_download();
    file(IMG, IMG_SIZE, IMG_SIZE);
    RXQ_N = RXQ_NEXT = 0;
    RX_HW = 0;
    rxq_add(0x80, 80, 0, 0);  /* beacon */
    rxq_add(0x80, 90, 0, 0);
    rxq_add(0x50, 100, 0, 0); /* probe response */
    rxq_add(0x80, 70, 0, 0);
    rxq_add(0x08, 120, 0, 0); /* data */
    rxq_add(0xD4, 14, 0, 0);  /* acknowledgement */
    rxq_add(0x40, 60, 0, 0);  /* probe request: some other management frame */
    rxq_add(0x00, 40, 1, 0);  /* a message from the firmware */
    rxq_add(0x80, 80, 0, 1);  /* checksum error */
    NLOG = 0;
    NIC_RTL8821CE.probe(&WIFI);
    CHECK(RX.state == RXS_OK && RX.frames == 7 && RX.beacons == 3 && RX.probe_resp == 1 && RX.data == 1 && RX.ctrl == 1 && RX.mgmt_other == 1);
    CHECK(RX.c2h == 1 && RX.bad == 1 && RX.rp == 9);
    {
        /* RCR as Linux' default filter, and the receiver is off again at the end, before the power-off */
        int rcr_seen = 0, cr_on = 0, cr_off_after = 0;
        for (int i = 0; i < NLOG; i++)
        {
            if (LOG[i].off == 0x608 && LOG[i].size == 4 && LOG[i].value == 0xF410400Eu)
                rcr_seen = 1;
            if (LOG[i].off == 0x100 && LOG[i].size == 1 && LOG[i].value == 0xFF)
                cr_on = 1;
            if (cr_on && LOG[i].off == 0x100 && LOG[i].size == 1 && LOG[i].value == 0)
                cr_off_after = 1;
        }
        CHECK(rcr_seen && cr_on && cr_off_after);
    }
    {
        char s6[96];
        NIC_RTL8821CE.status(s6, sizeof s6);
        CHECK(strstr(s6, ", init ok, 1ap, no cfg, off") && strlen(s6) < sizeof s6);
    }
    /* more frames than the ring has buffers: it must wrap around and nothing is lost */
    build_image(1);
    chip_for_download();
    file(IMG, IMG_SIZE, IMG_SIZE);
    RXQ_N = RXQ_NEXT = 0;
    RX_HW = 0;
    RX_BURST = 5; /* five frames arrive between two looks of the driver, so the index wraps around the ring in the middle of a batch */
    for (int i = 0; i < 100; i++)
        rxq_add(i % 2 ? 0x80 : 0x08, 60 + i, 0, 0);
    NIC_RTL8821CE.probe(&WIFI);
    CHECK(RX.frames == 100 && RX.beacons == 50 && RX.data == 50 && RX.bad == 0 && RX.rp == 100 % 32);
    RX_BURST = 1;
    /* nonsense in the index register is ignored */
    RX.rp = 0;
    RX.frames = 0;
    memset(REGS, 0, sizeof REGS);
    REGS[0x3B6] = 0xFF;
    REGS[0x3B7] = 0x0F; /* index 4095, beyond the 32 buffers */
    rx_poll();
    CHECK(RX.frames == 0 && RX.rp == 0);
    RXQ_N = RXQ_NEXT = 0;

    /* ---- stage 4c-3: scan. Beacons built byte by byte, as an access point sends them. ---- */
    {
        static unsigned char FR[10][256];
        unsigned FL[10];
        int nf = 0;
        /* header, fixed fields, then SSID, rates, (DS channel | HT operation), optional security elements, FCS */
#define MKBEACON(fc0v, bssid_last, cap, ssid_s, ssid_n, ds_chan, ht_chan, rsn, wpa)                          \
    do                                                                                                       \
    {                                                                                                        \
        unsigned char *f = FR[nf];                                                                           \
        memset(f, 0, 256);                                                                                   \
        f[0] = (fc0v);                                                                                       \
        memset(f + 4, 0xFF, 6);                                                                              \
        f[10] = 2; f[15] = (bssid_last);                                                                     \
        f[16] = 2; f[21] = (bssid_last);                                                                     \
        f[32] = 0x64;                                                                                        \
        f[34] = (cap) & 0xFF; f[35] = (cap) >> 8;                                                            \
        unsigned p = 36;                                                                                     \
        f[p++] = 0; f[p++] = (ssid_n); memcpy(f + p, (ssid_s), (ssid_n)); p += (ssid_n);                     \
        f[p++] = 1; f[p++] = 2; f[p++] = 0x82; f[p++] = 0x84;                                                \
        if (ds_chan) { f[p++] = 3; f[p++] = 1; f[p++] = (ds_chan); }                                         \
        if (ht_chan) { f[p++] = 61; f[p++] = 2; f[p++] = (ht_chan); f[p++] = 0; }                            \
        if (rsn) { f[p++] = 48; f[p++] = 2; f[p++] = 1; f[p++] = 0; }                                        \
        if (wpa) { f[p++] = 221; f[p++] = 4; f[p++] = 0x00; f[p++] = 0x50; f[p++] = 0xF2; f[p++] = 1; }      \
        p += 4; /* FCS */                                                                                    \
        FL[nf] = p;                                                                                          \
        nf++;                                                                                                \
    } while (0)
        MKBEACON(0x80, 1, 0x1431, "HomeNet", 7, 6, 0, 1, 0);       /* WPA2, channel 6 */
        MKBEACON(0x80, 2, 0x0421, "Cafe", 4, 11, 0, 0, 0);         /* open */
        MKBEACON(0x80, 3, 0x0411, "OldWep", 6, 1, 0, 0, 0);        /* privacy bit, no RSN: WEP */
        MKBEACON(0x80, 4, 0x1431, "WpaNet", 6, 6, 0, 0, 1);        /* WPA */
        MKBEACON(0x80, 5, 0x0421, "", 0, 6, 0, 0, 0);              /* hidden: empty name */
        MKBEACON(0x80, 6, 0x0421, "\0\0\0\0\0\0\0\0", 8, 6, 0, 0, 0); /* hidden: zeros */
        MKBEACON(0x80, 7, 0x1431, "Fast5", 5, 0, 36, 1, 0);        /* 5 GHz: no DS element, HT operation says 36 */
        MKBEACON(0x50, 8, 0x0421, "Resp", 4, 11, 0, 0, 0);         /* probe response */
        MKBEACON(0x80, 1, 0x1431, "HomeNet", 7, 6, 0, 1, 0);       /* HomeNet again */
        MKBEACON(0x80, 1, 0x1431, "HomeNet", 7, 6, 0, 1, 0);
#undef MKBEACON
        build_image(1);
        chip_for_download();
        file(IMG, IMG_SIZE, IMG_SIZE);
        RXQ_N = RXQ_NEXT = 0;
        RX_HW = 0;
        for (int i = 0; i < nf; i++)
            rxq_add_frame(FR[i], FL[i]);
        NIC_RTL8821CE.probe(&WIFI);
        CHECK(SC.state == 1 && SC.channels_ok == SCAN_NCH && SC.n == 8 && SC.dropped == 0);
        CHECK(strcmp(SC.ap[0].ssid, "HomeNet") == 0 && SC.ap[0].sec == 3 && SC.ap[0].channel == 6 && SC.ap[0].beacons == 3);
        CHECK(SC.ap[0].bssid[0] == 2 && SC.ap[0].bssid[5] == 1);
        CHECK(strcmp(SC.ap[1].ssid, "Cafe") == 0 && SC.ap[1].sec == 0 && SC.ap[1].channel == 11);
        CHECK(strcmp(SC.ap[2].ssid, "OldWep") == 0 && SC.ap[2].sec == 1);
        CHECK(strcmp(SC.ap[3].ssid, "WpaNet") == 0 && SC.ap[3].sec == 2);
        CHECK(SC.ap[4].ssid[0] == 0 && SC.ap[5].ssid[0] == 0); /* hidden networks have no name */
        CHECK(strcmp(SC.ap[6].ssid, "Fast5") == 0 && SC.ap[6].channel == 36 && SC.ap[6].sec == 3);
        CHECK(strcmp(SC.ap[7].ssid, "Resp") == 0);
        CHECK(RX.frames == 10 && RX.beacons == 9 && RX.probe_resp == 1);
        CHECK(SC.per_channel[0] == 10); /* everything arrived while the receiver listened on the first channel */
        char s7[96];
        NIC_RTL8821CE.status(s7, sizeof s7);
        CHECK(strstr(s7, ", init ok, 8ap, no cfg, off") && strlen(s7) < sizeof s7);
        /* the target access point: the WPA2 network with the name of the settings file; the passphrase stays out of the status line */
        static const char *const TARGETS[] = {"ssid=HomeNet\npsk=secret-pass-phrase\n", "ssid=OldWep\npsk=secret-pass-phrase\n", "ssid=Nope\npsk=secret-pass-phrase\n",
                                                  "ssid=Home\npsk=secret-pass-phrase\n"}; /* the last: a prefix of a name is not the name */
        for (int t = 0; t < 4; t++)
        {
            CFG_TEXT = TARGETS[t];
            chip_for_download();
            file(IMG, IMG_SIZE, IMG_SIZE);
            RXQ_N = RXQ_NEXT = 0;
            RX_HW = 0;
            for (int i = 0; i < nf; i++)
                rxq_add_frame(FR[i], FL[i]);
            NIC_RTL8821CE.probe(&WIFI);
            char s9[96];
            NIC_RTL8821CE.status(s9, sizeof s9);
            CHECK(!strstr(s9, "secret") && !strstr(s9, "phrase") && strlen(s9) < sizeof s9);
            if (t == 0)
                CHECK(TARGET.found && SC.ap[TARGET.index].channel == 6 && SC.ap[TARGET.index].beacons == 3 && strstr(s9, ", net rsn, off")) /* (the test beacons carry only a version number in the RSN element: not a security the driver can join) */;
            else
                CHECK(!TARGET.found && strstr(s9, ", net absent, off")); /* a WEP network is not a target: the plan is WPA2 only */
        }
        CFG_TEXT = 0;
        /* the same result as text lines (SYS_WIFISCAN reads these) */
        char ln[256];
        CHECK(rtl8821ce_scan_line(0, ln, sizeof ln) > 0 && strcmp(ln, "8 networks, 11/11 channels tuned, frames 2.4G 10 5G 0, beacons 9") == 0);
        CHECK(rtl8821ce_scan_line(1, ln, sizeof ln) > 0 && strcmp(ln, "ch   6  WPA2  02:00:00:00:00:01  HomeNet  x3") == 0);
        CHECK(rtl8821ce_scan_line(2, ln, sizeof ln) > 0 && strcmp(ln, "ch  11  open  02:00:00:00:00:02  Cafe  x1") == 0);
        CHECK(rtl8821ce_scan_line(3, ln, sizeof ln) > 0 && strcmp(ln, "ch   1  WEP   02:00:00:00:00:03  OldWep  x1") == 0);
        CHECK(rtl8821ce_scan_line(5, ln, sizeof ln) > 0 && strcmp(ln, "ch   6  open  02:00:00:00:00:05  (hidden)  x1") == 0);
        CHECK(rtl8821ce_scan_line(7, ln, sizeof ln) > 0 && strcmp(ln, "ch  36  WPA2  02:00:00:00:00:07  Fast5  x1") == 0);
        CHECK(rtl8821ce_scan_line(9, ln, sizeof ln) > 0 && strncmp(ln, "frames per channel: 1:10 6:0 11:0 36:0", 38) == 0);
        CHECK(rtl8821ce_scan_line(10, ln, sizeof ln) == -1);            /* past the end */
        CHECK(rtl8821ce_scan_line(1, ln, 8) == 7 && strcmp(ln, "ch   6 ") == 0); /* a short buffer gets a cut line, always NUL-terminated */
        CHECK(rtl8821ce_scan_line(0, ln, 1) == -1);
        SC.state = 0;
        CHECK(rtl8821ce_scan_line(0, ln, sizeof ln) == -1); /* no scan, no lines */
        SC.state = 1;
        /* a frame whose element runs past its end, and one that is too short: nothing is read beyond the frame */
        SC.n = 0;
        static unsigned char SHORT[40] = {0x80};
        scan_note(SHORT, 20);
        CHECK(SC.n == 0);
        scan_note(FR[0], 41); /* cut in the middle of the SSID element: it runs past the end of the frame */
        CHECK(SC.n == 0);
        scan_note(FR[0], FL[0] - 4);
        CHECK(SC.n == 1 && strcmp(SC.ap[0].ssid, "HomeNet") == 0);
    }

    /* ---- stage 5a-1: transmit power against Linux's whole pipeline (phy.c: parse the power-by-rate table and the limit table, the
     * by-rate and limit configuration, rtw_phy_set_tx_power_level_by_path, rtw8821c_set_tx_power_index) run on the real 8821c tables with
     * the world-wide limits. The efuse is made up from the seed: byte i = (seed * 131 + i * 29 + (i >> 1) * 7) & 0x3F. ---- */
    static const struct
    {
        unsigned seed, channel;
        unsigned char idx[12]; /* the power index of rates 0 .. 11 (CCK 1M .. 11M, OFDM 6M .. 54M) */
        unsigned w0, w4, w8;   /* the registers 0x1D00, 0x1D04, 0x1D08 */
    } PWR_LINUX[] = {
    {1, 1, {63,63,63,63,13,13,13,13,13,13,13,11}, 0x3F3F3F3F, 0x0D0D0D0D, 0x0B0D0D0D},
    {1, 6, {0,0,0,0,14,14,14,14,14,14,14,12}, 0x00000000, 0x0E0E0E0E, 0x0C0E0E0E},
    {1, 11, {29,29,29,29,35,35,35,35,35,35,35,35}, 0x1D1D1D1D, 0x23232323, 0x23232323},
    {1, 13, {63,63,63,63,63,63,63,63,63,63,63,63}, 0x3F3F3F3F, 0x3F3F3F3F, 0x3F3F3F3F},
    {1, 14, {36,36,36,34,21,21,21,21,19,17,15,13}, 0x22242424, 0x15151515, 0x0D0F1113},
    {1, 36, {0,0,0,0,16,16,16,16,16,16,16,15}, 0x00000000, 0x10101010, 0x0F101010},
    {1, 40, {0,0,0,0,16,16,16,16,16,16,16,15}, 0x00000000, 0x10101010, 0x0F101010},
    {1, 44, {0,0,0,0,45,45,45,45,45,45,45,44}, 0x00000000, 0x2D2D2D2D, 0x2C2D2D2D},
    {1, 48, {0,0,0,0,45,45,45,45,45,45,45,44}, 0x00000000, 0x2D2D2D2D, 0x2C2D2D2D},
    {1, 52, {0,0,0,0,6,6,6,6,6,6,6,6}, 0x00000000, 0x06060606, 0x06060606},
    {1, 56, {0,0,0,0,17,17,17,17,17,17,17,16}, 0x00000000, 0x11111111, 0x10111111},
    {1, 60, {0,0,0,0,46,46,46,46,46,46,46,45}, 0x00000000, 0x2E2E2E2E, 0x2D2E2E2E},
    {1, 64, {0,0,0,0,46,46,46,46,46,46,46,45}, 0x00000000, 0x2E2E2E2E, 0x2D2E2E2E},
    {1, 100, {0,0,0,0,18,18,18,18,18,18,18,17}, 0x00000000, 0x12121212, 0x11121212},
    {1, 108, {0,0,0,0,47,47,47,47,47,47,47,46}, 0x00000000, 0x2F2F2F2F, 0x2E2F2F2F},
    {1, 116, {0,0,0,0,19,19,19,19,19,19,19,18}, 0x00000000, 0x13131313, 0x12131313},
    {1, 124, {0,0,0,0,48,48,48,48,48,48,48,47}, 0x00000000, 0x30303030, 0x2F303030},
    {1, 132, {0,0,0,0,20,20,20,20,20,20,20,19}, 0x00000000, 0x14141414, 0x13141414},
    {1, 140, {0,0,0,0,49,49,49,49,49,49,49,48}, 0x00000000, 0x31313131, 0x30313131},
    {1, 149, {0,0,0,0,8,8,8,8,8,8,8,8}, 0x00000000, 0x08080808, 0x08080808},
    {1, 153, {0,0,0,0,8,8,8,8,8,8,8,8}, 0x00000000, 0x08080808, 0x08080808},
    {1, 157, {0,0,0,0,37,37,37,37,37,37,37,37}, 0x00000000, 0x25252525, 0x25252525},
    {1, 161, {0,0,0,0,37,37,37,37,37,37,37,37}, 0x00000000, 0x25252525, 0x25252525},
    {1, 165, {0,0,0,0,9,9,9,9,9,9,9,9}, 0x00000000, 0x09090909, 0x09090909},
    {2, 1, {2,2,2,2,3,3,3,3,3,3,3,1}, 0x02020202, 0x03030303, 0x01030303},
    {2, 6, {3,3,3,3,4,4,4,4,4,4,4,2}, 0x03030303, 0x04040404, 0x02040404},
    {2, 11, {32,32,32,32,25,25,25,25,25,25,25,25}, 0x20202020, 0x19191919, 0x19191919},
    {2, 13, {63,63,63,63,63,63,63,63,63,63,63,63}, 0x3F3F3F3F, 0x3F3F3F3F, 0x3F3F3F3F},
    {2, 14, {39,39,39,37,11,11,11,11,9,7,5,3}, 0x25272727, 0x0B0B0B0B, 0x03050709},
    {2, 36, {0,0,0,0,22,22,22,22,22,22,22,21}, 0x00000000, 0x16161616, 0x15161616},
    {2, 40, {0,0,0,0,22,22,22,22,22,22,22,21}, 0x00000000, 0x16161616, 0x15161616},
    {2, 44, {0,0,0,0,51,51,51,51,51,51,51,50}, 0x00000000, 0x33333333, 0x32333333},
    {2, 48, {0,0,0,0,51,51,51,51,51,51,51,50}, 0x00000000, 0x33333333, 0x32333333},
    {2, 52, {0,0,0,0,12,12,12,12,12,12,12,12}, 0x00000000, 0x0C0C0C0C, 0x0C0C0C0C},
    {2, 56, {0,0,0,0,23,23,23,23,23,23,23,22}, 0x00000000, 0x17171717, 0x16171717},
    {2, 60, {0,0,0,0,52,52,52,52,52,52,52,51}, 0x00000000, 0x34343434, 0x33343434},
    {2, 64, {0,0,0,0,52,52,52,52,52,52,52,51}, 0x00000000, 0x34343434, 0x33343434},
    {2, 100, {0,0,0,0,24,24,24,24,24,24,24,23}, 0x00000000, 0x18181818, 0x17181818},
    {2, 108, {0,0,0,0,53,53,53,53,53,53,53,52}, 0x00000000, 0x35353535, 0x34353535},
    {2, 116, {0,0,0,0,25,25,25,25,25,25,25,24}, 0x00000000, 0x19191919, 0x18191919},
    {2, 124, {0,0,0,0,54,54,54,54,54,54,54,53}, 0x00000000, 0x36363636, 0x35363636},
    {2, 132, {0,0,0,0,26,26,26,26,26,26,26,25}, 0x00000000, 0x1A1A1A1A, 0x191A1A1A},
    {2, 140, {0,0,0,0,55,55,55,55,55,55,55,54}, 0x00000000, 0x37373737, 0x36373737},
    {2, 149, {0,0,0,0,14,14,14,14,14,14,14,14}, 0x00000000, 0x0E0E0E0E, 0x0E0E0E0E},
    {2, 153, {0,0,0,0,14,14,14,14,14,14,14,14}, 0x00000000, 0x0E0E0E0E, 0x0E0E0E0E},
    {2, 157, {0,0,0,0,43,43,43,43,43,43,43,43}, 0x00000000, 0x2B2B2B2B, 0x2B2B2B2B},
    {2, 161, {0,0,0,0,43,43,43,43,43,43,43,43}, 0x00000000, 0x2B2B2B2B, 0x2B2B2B2B},
    {2, 165, {0,0,0,0,15,15,15,15,15,15,15,15}, 0x00000000, 0x0F0F0F0F, 0x0F0F0F0F},
    {3, 1, {5,5,5,5,9,9,9,9,9,9,9,7}, 0x05050505, 0x09090909, 0x07090909},
    {3, 6, {6,6,6,6,10,10,10,10,10,10,10,8}, 0x06060606, 0x0A0A0A0A, 0x080A0A0A},
    {3, 11, {35,35,35,35,31,31,31,31,31,31,31,31}, 0x23232323, 0x1F1F1F1F, 0x1F1F1F1F},
    {3, 13, {63,63,63,63,63,63,63,63,63,63,63,63}, 0x3F3F3F3F, 0x3F3F3F3F, 0x3F3F3F3F},
    {3, 14, {42,42,42,40,17,17,17,17,15,13,11,9}, 0x282A2A2A, 0x11111111, 0x090B0D0F},
    {3, 36, {0,0,0,0,12,12,12,12,12,12,12,11}, 0x00000000, 0x0C0C0C0C, 0x0B0C0C0C},
    {3, 40, {0,0,0,0,12,12,12,12,12,12,12,11}, 0x00000000, 0x0C0C0C0C, 0x0B0C0C0C},
    {3, 44, {0,0,0,0,41,41,41,41,41,41,41,40}, 0x00000000, 0x29292929, 0x28292929},
    {3, 48, {0,0,0,0,41,41,41,41,41,41,41,40}, 0x00000000, 0x29292929, 0x28292929},
    {3, 52, {0,0,0,0,2,2,2,2,2,2,2,2}, 0x00000000, 0x02020202, 0x02020202},
    {3, 56, {0,0,0,0,13,13,13,13,13,13,13,12}, 0x00000000, 0x0D0D0D0D, 0x0C0D0D0D},
    {3, 60, {0,0,0,0,42,42,42,42,42,42,42,41}, 0x00000000, 0x2A2A2A2A, 0x292A2A2A},
    {3, 64, {0,0,0,0,42,42,42,42,42,42,42,41}, 0x00000000, 0x2A2A2A2A, 0x292A2A2A},
    {3, 100, {0,0,0,0,14,14,14,14,14,14,14,13}, 0x00000000, 0x0E0E0E0E, 0x0D0E0E0E},
    {3, 108, {0,0,0,0,43,43,43,43,43,43,43,42}, 0x00000000, 0x2B2B2B2B, 0x2A2B2B2B},
    {3, 116, {0,0,0,0,15,15,15,15,15,15,15,14}, 0x00000000, 0x0F0F0F0F, 0x0E0F0F0F},
    {3, 124, {0,0,0,0,44,44,44,44,44,44,44,43}, 0x00000000, 0x2C2C2C2C, 0x2B2C2C2C},
    {3, 132, {0,0,0,0,16,16,16,16,16,16,16,15}, 0x00000000, 0x10101010, 0x0F101010},
    {3, 140, {0,0,0,0,45,45,45,45,45,45,45,44}, 0x00000000, 0x2D2D2D2D, 0x2C2D2D2D},
    {3, 149, {0,0,0,0,4,4,4,4,4,4,4,4}, 0x00000000, 0x04040404, 0x04040404},
    {3, 153, {0,0,0,0,4,4,4,4,4,4,4,4}, 0x00000000, 0x04040404, 0x04040404},
    {3, 157, {0,0,0,0,33,33,33,33,33,33,33,33}, 0x00000000, 0x21212121, 0x21212121},
    {3, 161, {0,0,0,0,33,33,33,33,33,33,33,33}, 0x00000000, 0x21212121, 0x21212121},
    {3, 165, {0,0,0,0,5,5,5,5,5,5,5,5}, 0x00000000, 0x05050505, 0x05050505},
    {4, 1, {8,8,8,8,15,15,15,15,15,15,15,13}, 0x08080808, 0x0F0F0F0F, 0x0D0F0F0F},
    {4, 6, {9,9,9,9,16,16,16,16,16,16,16,14}, 0x09090909, 0x10101010, 0x0E101010},
    {4, 11, {38,38,38,38,37,37,37,37,37,37,37,37}, 0x26262626, 0x25252525, 0x25252525},
    {4, 13, {63,63,63,63,63,63,63,63,63,63,63,63}, 0x3F3F3F3F, 0x3F3F3F3F, 0x3F3F3F3F},
    {4, 14, {45,45,45,43,23,23,23,23,21,19,17,15}, 0x2B2D2D2D, 0x17171717, 0x0F111315},
    {4, 36, {0,0,0,0,18,18,18,18,18,18,18,17}, 0x00000000, 0x12121212, 0x11121212},
    {4, 40, {0,0,0,0,18,18,18,18,18,18,18,17}, 0x00000000, 0x12121212, 0x11121212},
    {4, 44, {0,0,0,0,47,47,47,47,47,47,47,46}, 0x00000000, 0x2F2F2F2F, 0x2E2F2F2F},
    {4, 48, {0,0,0,0,47,47,47,47,47,47,47,46}, 0x00000000, 0x2F2F2F2F, 0x2E2F2F2F},
    {4, 52, {0,0,0,0,8,8,8,8,8,8,8,8}, 0x00000000, 0x08080808, 0x08080808},
    {4, 56, {0,0,0,0,19,19,19,19,19,19,19,18}, 0x00000000, 0x13131313, 0x12131313},
    {4, 60, {0,0,0,0,48,48,48,48,48,48,48,47}, 0x00000000, 0x30303030, 0x2F303030},
    {4, 64, {0,0,0,0,48,48,48,48,48,48,48,47}, 0x00000000, 0x30303030, 0x2F303030},
    {4, 100, {0,0,0,0,20,20,20,20,20,20,20,19}, 0x00000000, 0x14141414, 0x13141414},
    {4, 108, {0,0,0,0,49,49,49,49,49,49,49,48}, 0x00000000, 0x31313131, 0x30313131},
    {4, 116, {0,0,0,0,21,21,21,21,21,21,21,20}, 0x00000000, 0x15151515, 0x14151515},
    {4, 124, {0,0,0,0,50,50,50,50,50,50,50,49}, 0x00000000, 0x32323232, 0x31323232},
    {4, 132, {0,0,0,0,22,22,22,22,22,22,22,21}, 0x00000000, 0x16161616, 0x15161616},
    {4, 140, {0,0,0,0,51,51,51,51,51,51,51,50}, 0x00000000, 0x33333333, 0x32333333},
    {4, 149, {0,0,0,0,10,10,10,10,10,10,10,10}, 0x00000000, 0x0A0A0A0A, 0x0A0A0A0A},
    {4, 153, {0,0,0,0,10,10,10,10,10,10,10,10}, 0x00000000, 0x0A0A0A0A, 0x0A0A0A0A},
    {4, 157, {0,0,0,0,39,39,39,39,39,39,39,39}, 0x00000000, 0x27272727, 0x27272727},
    {4, 161, {0,0,0,0,39,39,39,39,39,39,39,39}, 0x00000000, 0x27272727, 0x27272727},
    {4, 165, {0,0,0,0,11,11,11,11,11,11,11,11}, 0x00000000, 0x0B0B0B0B, 0x0B0B0B0B},
    {5, 1, {11,11,11,11,21,21,21,21,21,21,21,19}, 0x0B0B0B0B, 0x15151515, 0x13151515},
    {5, 6, {12,12,12,12,22,22,22,22,22,22,22,20}, 0x0C0C0C0C, 0x16161616, 0x14161616},
    {5, 11, {41,41,41,41,43,43,43,43,43,43,43,43}, 0x29292929, 0x2B2B2B2B, 0x2B2B2B2B},
    {5, 13, {1,1,1,1,63,63,63,63,63,63,63,63}, 0x01010101, 0x3F3F3F3F, 0x3F3F3F3F},
    {5, 14, {48,48,48,46,29,29,29,29,27,25,23,21}, 0x2E303030, 0x1D1D1D1D, 0x1517191B},
    {5, 36, {0,0,0,0,24,24,24,24,24,24,24,23}, 0x00000000, 0x18181818, 0x17181818},
    {5, 40, {0,0,0,0,24,24,24,24,24,24,24,23}, 0x00000000, 0x18181818, 0x17181818},
    {5, 44, {0,0,0,0,53,53,53,53,53,53,53,52}, 0x00000000, 0x35353535, 0x34353535},
    {5, 48, {0,0,0,0,53,53,53,53,53,53,53,52}, 0x00000000, 0x35353535, 0x34353535},
    {5, 52, {0,0,0,0,14,14,14,14,14,14,14,14}, 0x00000000, 0x0E0E0E0E, 0x0E0E0E0E},
    {5, 56, {0,0,0,0,25,25,25,25,25,25,25,24}, 0x00000000, 0x19191919, 0x18191919},
    {5, 60, {0,0,0,0,54,54,54,54,54,54,54,53}, 0x00000000, 0x36363636, 0x35363636},
    {5, 64, {0,0,0,0,54,54,54,54,54,54,54,53}, 0x00000000, 0x36363636, 0x35363636},
    {5, 100, {0,0,0,0,26,26,26,26,26,26,26,25}, 0x00000000, 0x1A1A1A1A, 0x191A1A1A},
    {5, 108, {0,0,0,0,55,55,55,55,55,55,55,54}, 0x00000000, 0x37373737, 0x36373737},
    {5, 116, {0,0,0,0,27,27,27,27,27,27,27,26}, 0x00000000, 0x1B1B1B1B, 0x1A1B1B1B},
    {5, 124, {0,0,0,0,56,56,56,56,56,56,56,55}, 0x00000000, 0x38383838, 0x37383838},
    {5, 132, {0,0,0,0,28,28,28,28,28,28,28,27}, 0x00000000, 0x1C1C1C1C, 0x1B1C1C1C},
    {5, 140, {0,0,0,0,57,57,57,57,57,57,57,56}, 0x00000000, 0x39393939, 0x38393939},
    {5, 149, {0,0,0,0,16,16,16,16,16,16,16,16}, 0x00000000, 0x10101010, 0x10101010},
    {5, 153, {0,0,0,0,16,16,16,16,16,16,16,16}, 0x00000000, 0x10101010, 0x10101010},
    {5, 157, {0,0,0,0,45,45,45,45,45,45,45,45}, 0x00000000, 0x2D2D2D2D, 0x2D2D2D2D},
    {5, 161, {0,0,0,0,45,45,45,45,45,45,45,45}, 0x00000000, 0x2D2D2D2D, 0x2D2D2D2D},
    {5, 165, {0,0,0,0,17,17,17,17,17,17,17,17}, 0x00000000, 0x11111111, 0x11111111},
    {6, 1, {14,14,14,14,27,27,27,27,27,27,27,25}, 0x0E0E0E0E, 0x1B1B1B1B, 0x191B1B1B},
    {6, 6, {15,15,15,15,28,28,28,28,28,28,28,26}, 0x0F0F0F0F, 0x1C1C1C1C, 0x1A1C1C1C},
    {6, 11, {44,44,44,44,49,49,49,49,49,49,49,49}, 0x2C2C2C2C, 0x31313131, 0x31313131},
    {6, 13, {4,4,4,4,1,1,1,1,1,1,1,1}, 0x04040404, 0x01010101, 0x01010101},
    {6, 14, {51,51,51,49,35,35,35,35,33,31,29,27}, 0x31333333, 0x23232323, 0x1B1D1F21},
    {6, 36, {0,0,0,0,30,30,30,30,30,30,30,29}, 0x00000000, 0x1E1E1E1E, 0x1D1E1E1E},
    {6, 40, {0,0,0,0,30,30,30,30,30,30,30,29}, 0x00000000, 0x1E1E1E1E, 0x1D1E1E1E},
    {6, 44, {0,0,0,0,59,59,59,59,59,59,59,58}, 0x00000000, 0x3B3B3B3B, 0x3A3B3B3B},
    {6, 48, {0,0,0,0,59,59,59,59,59,59,59,58}, 0x00000000, 0x3B3B3B3B, 0x3A3B3B3B},
    {6, 52, {0,0,0,0,20,20,20,20,20,20,20,20}, 0x00000000, 0x14141414, 0x14141414},
    {6, 56, {0,0,0,0,31,31,31,31,31,31,31,30}, 0x00000000, 0x1F1F1F1F, 0x1E1F1F1F},
    {6, 60, {0,0,0,0,60,60,60,60,60,60,60,59}, 0x00000000, 0x3C3C3C3C, 0x3B3C3C3C},
    {6, 64, {0,0,0,0,60,60,60,60,60,60,60,59}, 0x00000000, 0x3C3C3C3C, 0x3B3C3C3C},
    {6, 100, {0,0,0,0,32,32,32,32,32,32,32,31}, 0x00000000, 0x20202020, 0x1F202020},
    {6, 108, {0,0,0,0,61,61,61,61,61,61,61,60}, 0x00000000, 0x3D3D3D3D, 0x3C3D3D3D},
    {6, 116, {0,0,0,0,33,33,33,33,33,33,33,32}, 0x00000000, 0x21212121, 0x20212121},
    {6, 124, {0,0,0,0,62,62,62,62,62,62,62,61}, 0x00000000, 0x3E3E3E3E, 0x3D3E3E3E},
    {6, 132, {0,0,0,0,34,34,34,34,34,34,34,33}, 0x00000000, 0x22222222, 0x21222222},
    {6, 140, {0,0,0,0,63,63,63,63,63,63,63,62}, 0x00000000, 0x3F3F3F3F, 0x3E3F3F3F},
    {6, 149, {0,0,0,0,22,22,22,22,22,22,22,22}, 0x00000000, 0x16161616, 0x16161616},
    {6, 153, {0,0,0,0,22,22,22,22,22,22,22,22}, 0x00000000, 0x16161616, 0x16161616},
    {6, 157, {0,0,0,0,51,51,51,51,51,51,51,51}, 0x00000000, 0x33333333, 0x33333333},
    {6, 161, {0,0,0,0,51,51,51,51,51,51,51,51}, 0x00000000, 0x33333333, 0x33333333},
    {6, 165, {0,0,0,0,23,23,23,23,23,23,23,23}, 0x00000000, 0x17171717, 0x17171717},
    {7, 1, {17,17,17,17,33,33,33,33,33,33,33,31}, 0x11111111, 0x21212121, 0x1F212121},
    {7, 6, {18,18,18,18,34,34,34,34,34,34,34,32}, 0x12121212, 0x22222222, 0x20222222},
    {7, 11, {47,47,47,47,55,55,55,55,55,55,55,55}, 0x2F2F2F2F, 0x37373737, 0x37373737},
    {7, 13, {7,7,7,7,7,7,7,7,7,7,7,7}, 0x07070707, 0x07070707, 0x07070707},
    {7, 14, {54,54,54,52,41,41,41,41,39,37,35,33}, 0x34363636, 0x29292929, 0x21232527},
    {7, 36, {0,0,0,0,36,36,36,36,36,36,36,35}, 0x00000000, 0x24242424, 0x23242424},
    {7, 40, {0,0,0,0,36,36,36,36,36,36,36,35}, 0x00000000, 0x24242424, 0x23242424},
    {7, 44, {0,0,0,0,63,63,63,63,63,63,63,63}, 0x00000000, 0x3F3F3F3F, 0x3F3F3F3F},
    {7, 48, {0,0,0,0,63,63,63,63,63,63,63,63}, 0x00000000, 0x3F3F3F3F, 0x3F3F3F3F},
    {7, 52, {0,0,0,0,26,26,26,26,26,26,26,26}, 0x00000000, 0x1A1A1A1A, 0x1A1A1A1A},
    {7, 56, {0,0,0,0,37,37,37,37,37,37,37,36}, 0x00000000, 0x25252525, 0x24252525},
    {7, 60, {0,0,0,0,63,63,63,63,63,63,63,63}, 0x00000000, 0x3F3F3F3F, 0x3F3F3F3F},
    {7, 64, {0,0,0,0,63,63,63,63,63,63,63,63}, 0x00000000, 0x3F3F3F3F, 0x3F3F3F3F},
    {7, 100, {0,0,0,0,38,38,38,38,38,38,38,37}, 0x00000000, 0x26262626, 0x25262626},
    {7, 108, {0,0,0,0,63,63,63,63,63,63,63,63}, 0x00000000, 0x3F3F3F3F, 0x3F3F3F3F},
    {7, 116, {0,0,0,0,39,39,39,39,39,39,39,38}, 0x00000000, 0x27272727, 0x26272727},
    {7, 124, {0,0,0,0,63,63,63,63,63,63,63,63}, 0x00000000, 0x3F3F3F3F, 0x3F3F3F3F},
    {7, 132, {0,0,0,0,40,40,40,40,40,40,40,39}, 0x00000000, 0x28282828, 0x27282828},
    {7, 140, {0,0,0,0,63,63,63,63,63,63,63,63}, 0x00000000, 0x3F3F3F3F, 0x3F3F3F3F},
    {7, 149, {0,0,0,0,28,28,28,28,28,28,28,28}, 0x00000000, 0x1C1C1C1C, 0x1C1C1C1C},
    {7, 153, {0,0,0,0,28,28,28,28,28,28,28,28}, 0x00000000, 0x1C1C1C1C, 0x1C1C1C1C},
    {7, 157, {0,0,0,0,63,63,63,63,63,63,63,63}, 0x00000000, 0x3F3F3F3F, 0x3F3F3F3F},
    {7, 161, {0,0,0,0,63,63,63,63,63,63,63,63}, 0x00000000, 0x3F3F3F3F, 0x3F3F3F3F},
    {7, 165, {0,0,0,0,29,29,29,29,29,29,29,29}, 0x00000000, 0x1D1D1D1D, 0x1D1D1D1D},
    };
    for (unsigned k = 0; k < sizeof PWR_LINUX / sizeof PWR_LINUX[0]; k++)
    {
        unsigned char tp[42];
        for (unsigned i = 0; i < 42; i++)
            tp[i] = (unsigned char)((PWR_LINUX[k].seed * 131u + i * 29u + (i >> 1) * 7u) & 0x3F);
        unsigned ch = PWR_LINUX[k].channel;
        for (unsigned r = ch <= 14 ? 0 : 4; r < 12; r++)
            if (tx_power_index(tp, ch, r) != PWR_LINUX[k].idx[r])
            {
                printf("FAIL tx power seed %u channel %u rate %u: %u, Linux %u\n", PWR_LINUX[k].seed, ch, r, tx_power_index(tp, ch, r),
                       PWR_LINUX[k].idx[r]);
                fails++;
            }
        chip_for_download();
        NLOG = 0;
        tx_power_apply(tp, ch);
        int n_expected = ch <= 14 ? 3 : 2;
        CHECK(NLOG == n_expected);
        for (int i = 0; i < NLOG; i++)
        {
            unsigned want = LOG[i].off == 0x1D00 ? PWR_LINUX[k].w0 : LOG[i].off == 0x1D04 ? PWR_LINUX[k].w4 : PWR_LINUX[k].w8;
            if (LOG[i].size != 4 || (LOG[i].off != 0x1D00 && LOG[i].off != 0x1D04 && LOG[i].off != 0x1D08) || LOG[i].value != want)
            {
                printf("FAIL tx power register seed %u channel %u: 0x%X = 0x%08X, Linux 0x%08X\n", PWR_LINUX[k].seed, ch, LOG[i].off, LOG[i].value, want);
                fails++;
            }
        }
    }

    /* ---- stage 5a-2: the first frames on the air. The pretend chip answers a broadcast probe request with two probe responses. ---- */
    {
        TXP_ENABLE = 1;
        static const unsigned char MAC6[6] = {0x00, 0xE0, 0x4C, 0x68, 0x12, 0x34};
        /* two access points' probe responses: a header of 24 bytes, 12 fixed bytes, SSID and DS elements, 4 bytes FCS */
        for (int a = 0; a < 2; a++)
        {
            unsigned char *f = TX_RESP[a];
            memset(f, 0, 160);
            f[0] = 0x50;
            memcpy(f + 4, MAC6, 6);
            f[10] = 2;
            f[15] = (unsigned char)(0x70 + a);
            f[16] = 2;
            f[21] = (unsigned char)(0x70 + a);
            f[34] = 0x31;
            f[35] = 0x14;
            unsigned p = 36;
            f[p++] = 0;
            f[p++] = 5;
            memcpy(f + p, a ? "Other" : "Reply", 5);
            p += 5;
            f[p++] = 3;
            f[p++] = 1;
            f[p++] = 149;
            f[p++] = 48; /* RSN */
            f[p++] = 2;
            f[p++] = 1;
            f[p++] = 0;
            p += 4;
            TX_RESP_LEN[a] = p;
        }
        TX_ANSWER = 2;
        build_image(1);
        chip_for_download();
        file(IMG, IMG_SIZE, IMG_SIZE);
        RXQ_N = RXQ_NEXT = 0;
        RX_HW = 0;
        memset(&TXCAP, 0, sizeof TXCAP);
        NIC_RTL8821CE.probe(&WIFI);
        CHECK(EF.state == EF_OK && EF.txpwr[0] == 5 && EF.txpwr[41] == 5 + 3 * 41); /* the calibration comes from the efuse */
        CHECK(TXP.state == 1 && TXP.sent == 3 && TXP.failed == 0 && TXP.channel == 149);
        CHECK(TXCAP.count == 3 && TXCAP.idx_written == 3 && TXCAP.entry == 2);
        for (int i = 0; i < 6; i++)
            CHECK(REGS[0x610 + i] == MAC6[i]); /* the chip answers to our address */
        /* the transmit descriptor of the third frame */
        unsigned w[10];
        memcpy(w, TXCAP.desc, 40);
        CHECK((w[0] & 0xFFFF) == TXCAP.len && ((w[0] >> 16) & 0xFF) == 48 && (w[0] & BIT(24)) && (w[0] & BIT(26)) && (w[0] & BIT(31)));
        CHECK((w[1] & 0xFF) == 0 && ((w[1] >> 8) & 0x1F) == 18 && ((w[1] >> 16) & 0x1F) == 7 && ((w[1] >> 22) & 3) == 0); /* MAC id 0, management queue, rate id G */
        CHECK(w[2] == 0 && (w[3] & BIT(8)) && (w[3] & BIT(10)) && (w[3] & 0xC0) == 0 && (w[4] & 0x7F) == 4 && w[5] == 0);   /* use rate 6M, no fallback */
        CHECK((w[8] & BIT(15)) && (w[9] & 0xFFF000) == 0);                                                                   /* hardware sequence number */
        /* the buffer descriptor: the descriptor, then the frame */
        unsigned bd0 = TXCAP.bd[0] | TXCAP.bd[1] << 8, psb = TXCAP.bd[2] | TXCAP.bd[3] << 8, bd1 = TXCAP.bd[8] | TXCAP.bd[9] << 8;
        unsigned d0 = TXCAP.bd[4] | TXCAP.bd[5] << 8 | TXCAP.bd[6] << 16 | (unsigned)TXCAP.bd[7] << 24;
        unsigned d1 = TXCAP.bd[12] | TXCAP.bd[13] << 8 | TXCAP.bd[14] << 16 | (unsigned)TXCAP.bd[15] << 24;
        CHECK(bd0 == 48 && bd1 == TXCAP.len && psb == (48 + TXCAP.len - 1) / 128 + 1 && !(psb & BIT(15)) && d0 == D.stage_dma && d1 == D.stage_dma + 48);
        /* the frame: a probe request from our address to everybody with an empty name and the 5 GHz rates */
        static const unsigned char WANT[] = {0x40, 0x00, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0xE0, 0x4C, 0x68, 0x12, 0x34, 0xFF, 0xFF, 0xFF,
                                             0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 1, 8, 0x8C, 0x12, 0x98, 0x24, 0xB0, 0x48, 0x60, 0x6C};
        CHECK(TXCAP.len == sizeof WANT && memcmp(TXCAP.frame, WANT, sizeof WANT) == 0);
        /* the answers were heard, and the networks that answered are in the list */
        CHECK(TXP.replies == 6 && SC.n >= 2); /* two responses to each of the three requests */
        char s8[96];
        NIC_RTL8821CE.status(s8, sizeof s8);
        CHECK(strstr(s8, ", init ok, ") && strstr(s8, ", tx3 pr6, no cfg, off") && strlen(s8) < sizeof s8);
        /* a power on 5 GHz channel 149 was written: the rates 6M .. 54M */
        CHECK(count_writes(0x1D04, 4) >= 1 && count_writes(0x1D08, 4) >= 1 && count_writes(0x1D00, 4) == 0);

        /* the chip fetches nothing: every frame fails and says so */
        chip_for_download();
        file(IMG, IMG_SIZE, IMG_SIZE);
        RXQ_N = RXQ_NEXT = 0;
        RX_HW = 0;
        KNOB_TX_STUCK = 1;
        NIC_RTL8821CE.probe(&WIFI);
        CHECK(TXP.sent == 0 && TXP.failed == 3 && TXP.replies == 0);
        NIC_RTL8821CE.status(s8, sizeof s8);
        CHECK(TXP.variant == -1 && strstr(s8, ", init ok, tx0/3 00000000 00000000, off") && strlen(s8) < sizeof s8); /* no way of starting the DMA worked: the DMA error status and the pause register are shown */
        KNOB_TX_STUCK = 0;

        /* a chip that takes the index only as a 32-bit write: the fifth way of starting works, and the line says so */
        chip_for_download();
        file(IMG, IMG_SIZE, IMG_SIZE);
        RXQ_N = RXQ_NEXT = 0;
        RX_HW = 0;
        KNOB_TX_ONLY32 = 1;
        TX_ANSWER = 2;
        NIC_RTL8821CE.probe(&WIFI);
        CHECK(TXP.variant == 4 && TXP.sent == 3 && TXP.failed == 0 && TXP.replies == 6);
        NIC_RTL8821CE.status(s8, sizeof s8);
        CHECK(strstr(s8, ", tx3 v4 pr6, no cfg, off") && strlen(s8) < sizeof s8);
        KNOB_TX_ONLY32 = 0;
        TXP.kick32 = 0;

        /* the index never moves but the access points answer: that counts as working, and the line says how it went */
        chip_for_download();
        file(IMG, IMG_SIZE, IMG_SIZE);
        RXQ_N = RXQ_NEXT = 0;
        RX_HW = 0;
        KNOB_TX_SILENT = 1;
        NIC_RTL8821CE.probe(&WIFI);
        CHECK(TXP.variant == 0 && TXP.sent == 1 && TXP.failed == 2 && TXP.replies == 6);
        NIC_RTL8821CE.status(s8, sizeof s8);
        CHECK(strstr(s8, ", tx1/3 pr6, no cfg, off") && strlen(s8) < sizeof s8);
        KNOB_TX_SILENT = 0;

        /* the ring wraps after 128 entries, and a full ring is not written */
        chip_for_download();
        memset(REGS, 0, sizeof REGS);
        POOL_OK = 1;
        TXP.wp = 0;
        unsigned char fr[40] = {0x40};
        memset(fr + 4, 0xFF, 6);
        int okc = 0;
        for (int i = 0; i < 130; i++)
            okc += tx_mgmt(fr, sizeof fr, 6) == 0;
        CHECK(okc == 130 && TXP.wp == 2);
        REGS[0x3B2] = 3; /* the chip's read index is 3: entry 2 is the last one that may be filled */
        REGS[0x3B3] = 0;
        CHECK(tx_mgmt(fr, sizeof fr, 6) == -1 && TXP.wp == 2);
        CHECK(tx_mgmt(fr, 10, 6) == -1 && tx_mgmt(fr, 2000, 6) == -1); /* a frame that is too short or too long */
        TXP_ENABLE = 0;
        TX_ANSWER = 0;
    }

    /* ---- stage 5b-0: the settings file (made-up texts; the user's own file is never read by anything but the driver on their PC) ---- */
    {
        CFG_TEXT = 0;
        cfg_load();
        CHECK(CFG.state == CFG_NONE);
        const char *ok1 = "ssid=TestNet\npsk=hunter2hunter2\n";
        CFG_TEXT = ok1;
        cfg_load();
        CHECK(CFG.state == CFG_OK && CFG.ssid_len == 7 && strcmp(CFG.ssid, "TestNet") == 0 && CFG.psk_len == 14 && strcmp(CFG.psk, "hunter2hunter2") == 0);
        const char *ok2 = "# a comment\r\nother=1\r\nssid=My Net  \r\n\r\npsk=pass word 12 \r\n"; /* CRLF, trailing blanks, other lines, spaces inside, no final newline needed */
        CFG_TEXT = ok2;
        cfg_load();
        CHECK(CFG.state == CFG_OK && strcmp(CFG.ssid, "My Net") == 0 && strcmp(CFG.psk, "pass word 12") == 0);
        const char *ok3 = "psk=abcdefgh\nssid=X"; /* the other order, the last line without a newline */
        CFG_TEXT = ok3;
        cfg_load();
        CHECK(CFG.state == CFG_OK && strcmp(CFG.ssid, "X") == 0 && strcmp(CFG.psk, "abcdefgh") == 0);
        static const struct { const char *text; const char *why; } BAD[] = {
            {"ssid=\npsk=12345678\n", "ssid"},
            {"ssid=012345678901234567890123456789012\npsk=12345678\n", "ssid"}, /* 33 characters */
            {"psk=12345678\n", "ssid"},                                        /* no ssid line */
            {"ssid=A\npsk=1234567\n", "psk"},                                  /* 7 characters */
            {"ssid=A\npsk=0123456789012345678901234567890123456789012345678901234567890123\n", "psk"}, /* 64 characters */
            {"ssid=A\n", "psk"},
        };
        for (unsigned i = 0; i < sizeof BAD / sizeof BAD[0]; i++)
        {
            CFG_TEXT = BAD[i].text;
            cfg_load();
            if (CFG.state != CFG_BAD || strcmp(CFG.why, BAD[i].why) != 0)
            {
                printf("FAIL settings text %u: state %d why %s, want bad %s\n", i, CFG.state, CFG.why ? CFG.why : "-", BAD[i].why);
                fails++;
            }
        }
        const char *lim = "ssid=01234567890123456789012345678901\npsk=01234567890123456789012345678901234567890123456789012345678901\n"; /* 32 and 62 */
        CFG_TEXT = lim;
        cfg_load();
        CHECK(CFG.state == CFG_OK && CFG.ssid_len == 32 && CFG.psk_len == 62);
    }

    /* ---- stage 5b-1: authentication and association against a pretend access point ---- */
    {
        /* the RSN element of a WPA2-Personal network, as the AP sends it: version, group CCMP, pairwise CCMP, key management PSK, no capabilities */
        static const unsigned char RSN_OK_[20] = {1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F, 0xAC, 2, 0, 0};
        static unsigned char RSN_TKIP[20], RSN_SAE[20], RSN_PMF[20], RSN_GTKIP[20];
        memcpy(RSN_TKIP, RSN_OK_, 20);
        RSN_TKIP[11] = 2; /* pairwise TKIP only */
        memcpy(RSN_SAE, RSN_OK_, 20);
        RSN_SAE[17] = 8; /* key management SAE only */
        memcpy(RSN_PMF, RSN_OK_, 20);
        RSN_PMF[18] = 0x40; /* management frame protection required */
        memcpy(RSN_GTKIP, RSN_OK_, 20);
        RSN_GTKIP[5] = 2; /* group cipher TKIP */
        CHECK(rsn_check(RSN_OK_, 20) == RSN_OK && rsn_check(RSN_TKIP, 20) == RSN_NO_CCMP && rsn_check(RSN_SAE, 20) == RSN_NO_PSK);
        CHECK(rsn_check(RSN_PMF, 20) == RSN_PMF_REQUIRED && rsn_check(RSN_GTKIP, 20) == RSN_GROUP);
        CHECK(rsn_check(RSN_OK_, 2) == RSN_BAD_VERSION && rsn_check(RSN_OK_, 12) == RSN_BAD_VERSION && rsn_check(RSN_OK_, 7) == RSN_BAD_VERSION);
        unsigned char two[20];
        memcpy(two, RSN_OK_, 20);
        two[0] = 2; /* version 2 */
        CHECK(rsn_check(two, 20) == RSN_BAD_VERSION);
        /* an AP that offers both PSK and SAE (WPA2/WPA3 mixed): fine, PMF is only capable, not required */
        unsigned char mixed[24] = {1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F, 0xAC, 4, 2, 0, 0x00, 0x0F, 0xAC, 8, 0x00, 0x0F, 0xAC, 2, 0x80, 0};
        CHECK(rsn_check(mixed, 24) == RSN_OK);

        static const struct { const unsigned char *rsn; int mode; const char *line; int frames; int hs; const char *pass; int bad_mic; } CASES[] = {
            {RSN_OK_, 0, ", net ch149, assoc ok, 4way none, off", 6, 0, 0, 0},   /* 3 probe requests, authentication, association, deauthentication */
            {RSN_OK_, 1, ", net ch149, auth none, off", 6, 0, 0, 0},           /* the AP stays silent: three authentication attempts, nothing more */
            {RSN_OK_, 2, ", net ch149, auth s1, off", 4, 0, 0, 0},             /* authentication refused: no association */
            {RSN_OK_, 3, ", net ch149, auth ok, assoc s17, off", 6, 0, 0, 0},  /* association refused: still says goodbye */
            {RSN_TKIP, 0, ", net ccmp, off", 3, 0, 0, 0},                      /* a security this driver cannot join: no frame is sent to it */
            {RSN_SAE, 0, ", net akm, off", 3, 0, 0, 0},
            {RSN_PMF, 0, ", net pmf, off", 3, 0, 0, 0},
            {RSN_GTKIP, 0, ", net grp, off", 3, 0, 0, 0},
            /* the 4-way handshake: 3 probe requests, authentication, association, message 2, message 4, deauthentication */
            {RSN_OK_, 0, ", net ch149, assoc ok, 4way ok, dec 2, icv 1, off", 8, 1, "secret-pass-phrase", 0},
            {RSN_OK_, 0, ", net ch149, assoc ok, 4way none, off", 7, 1, "another passphrase", 0}, /* the access point has another key: it never sends message 3 */
            {RSN_OK_, 0, ", net ch149, assoc ok, 4way mic, off", 7, 1, "secret-pass-phrase", 1},  /* message 3 with a damaged signature */
        };
        for (unsigned c = 0; c < sizeof CASES / sizeof CASES[0]; c++)
        {
            static unsigned char beacon[128];
            memset(beacon, 0, sizeof beacon);
            beacon[0] = 0x80;
            memset(beacon + 4, 0xFF, 6);
            memcpy(beacon + 10, AP_BSSID, 6);
            memcpy(beacon + 16, AP_BSSID, 6);
            beacon[32] = 0x64;
            beacon[34] = 0x31;
            beacon[35] = 0x14;
            unsigned p = 36;
            beacon[p++] = 0;
            beacon[p++] = 7;
            memcpy(beacon + p, "TestNet", 7);
            p += 7;
            beacon[p++] = 3;
            beacon[p++] = 1;
            beacon[p++] = 149;
            beacon[p++] = 48;
            beacon[p++] = 20;
            memcpy(beacon + p, CASES[c].rsn, 20);
            p += 20;
            p += 4; /* FCS */
            CFG_TEXT = "ssid=TestNet\npsk=secret-pass-phrase\n";
            TXP_ENABLE = 1;
            TX_ANSWER = 0;
            AP_MODE = CASES[c].mode;
            AP_HS = CASES[c].hs;
            AP_PASS = CASES[c].pass ? CASES[c].pass : "secret-pass-phrase";
            AP_BAD_MIC = CASES[c].bad_mic;
            build_image(1);
            chip_for_download();
            file(IMG, IMG_SIZE, IMG_SIZE);
            RXQ_N = RXQ_NEXT = 0;
            RX_HW = 0;
            TXLOG_N = 0;
            AP_RESP_N = 0;
            rxq_add_frame(beacon, p);
            NIC_RTL8821CE.probe(&WIFI);
            char sc[96];
            NIC_RTL8821CE.status(sc, sizeof sc);
            if (!strstr(sc, CASES[c].line) || strlen(sc) >= sizeof sc || TXLOG_N != CASES[c].frames || strstr(sc, "secret"))
            {
                printf("FAIL connect case %u: [%s] with %d frames, wanted %s and %d frames\n", c, sc, TXLOG_N, CASES[c].line, CASES[c].frames);
                fails++;
            }
            if (c == 0)
            {
                static const unsigned char PROBE_TYPES[3] = {0x40, 0x40, 0x40};
                for (int i = 0; i < 3; i++)
                    CHECK(TXLOG[i].b[0] == PROBE_TYPES[i]);
                /* authentication: open system, sequence 1, status 0, to the BSSID, from our MAC */
                static const unsigned char MAC6[6] = {0x00, 0xE0, 0x4C, 0x68, 0x12, 0x34};
                unsigned char want_auth[30] = {0xB0, 0, 0, 0};
                memcpy(want_auth + 4, AP_BSSID, 6);
                memcpy(want_auth + 10, MAC6, 6);
                memcpy(want_auth + 16, AP_BSSID, 6);
                want_auth[26] = 1;
                CHECK(TXLOG[3].len == 30 && memcmp(TXLOG[3].b, want_auth, 30) == 0);
                /* association: capability ESS + privacy, listen interval 10, the name, the 5 GHz rates, the RSN element */
                unsigned char want_assoc[69] = {0x00, 0, 0, 0};
                memcpy(want_assoc + 4, AP_BSSID, 6);
                memcpy(want_assoc + 10, MAC6, 6);
                memcpy(want_assoc + 16, AP_BSSID, 6);
                unsigned q = 24; /* the body follows the 24 byte header */
                want_assoc[q++] = 0x11;
                want_assoc[q++] = 0x00;
                want_assoc[q++] = 10;
                want_assoc[q++] = 0;
                want_assoc[q++] = 0;
                want_assoc[q++] = 7;
                memcpy(want_assoc + q, "TestNet", 7);
                q += 7;
                static const unsigned char TAIL[] = {1, 8, 0x8C, 0x12, 0x98, 0x24, 0xB0, 0x48, 0x60, 0x6C, 48, 20, 1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F, 0xAC, 2, 0, 0};
                memcpy(want_assoc + q, TAIL, sizeof TAIL);
                q += sizeof TAIL;
                CHECK(q == 69 && TXLOG[4].len == 69 && memcmp(TXLOG[4].b, want_assoc, 69) == 0);
                /* deauthentication: reason 3 */
                unsigned char want_deauth[26] = {0xC0, 0, 0, 0};
                memcpy(want_deauth + 4, AP_BSSID, 6);
                memcpy(want_deauth + 10, MAC6, 6);
                memcpy(want_deauth + 16, AP_BSSID, 6);
                want_deauth[24] = 3;
                CHECK(TXLOG[5].len == 26 && memcmp(TXLOG[5].b, want_deauth, 26) == 0);
                CHECK(CONN.aid == 5 && CONN.auth_seen && CONN.assoc_seen && CONN.auth_status == 0 && CONN.assoc_status == 0);
                CHECK(CONN.auth_tries == 1 && CONN.assoc_tries == 1);
                CHECK(!strstr(sc, "MAC")); /* no room for the address when a connection was tried */
            }
            if (c == 1)
                CHECK(CONN.auth_tries == 3 && !CONN.auth_seen && !CONN.assoc_seen);
            if (c == 8) /* the handshake that works: the frames, and what the access point saw */
            {
                CHECK(AP_GOT_MSG2 == 1 && AP_GOT_MSG4 == 1 && HS.result == HS_OK && HS.rx_msgs == 2 && HS.tx_msgs == 2);
                CHECK(TXLOG[5].len == 32 + 99 + 22 && TXLOG[5].b[0] == 0x08 && TXLOG[5].b[1] == 0x01);                 /* message 2: a data frame to the access point */
                CHECK(memcmp(TXLOG[5].b + 4, AP_BSSID, 6) == 0 && memcmp(TXLOG[5].b + 16, AP_BSSID, 6) == 0);
                CHECK(TXLOG[5].b[24] == 0xAA && TXLOG[5].b[30] == 0x88 && TXLOG[5].b[31] == 0x8E && TXLOG[5].b[32 + 5] == 0x01 && TXLOG[5].b[32 + 6] == 0x0A);
                CHECK(TXLOG[6].len == 32 + 99 && TXLOG[6].b[32 + 5] == 0x03 && TXLOG[6].b[32 + 6] == 0x0A);              /* message 4 */
                CHECK(TXLOG[7].b[0] == 0xC0);                                                                           /* then the deauthentication */
                CHECK(W4.state == 2 && W4.gtk_len == 16 && W4.gtk_id == 1 && W4.gtk[0] == 0x40 && W4.gtk[15] == 0x4F);
                for (unsigned i = 0; i < sizeof HS.pmk; i++)
                    CHECK(HS.pmk[i] == 0); /* the master key is wiped after use */
            }
        }
        CFG_TEXT = 0;
        AP_MODE = 0;
        TXP_ENABLE = 0;
    }

    /* ---- an EAPOL packet is taken only from our access point, to our address, while the handshake runs ---- */
    {
        static const unsigned char MACX[6] = {0x00, 0xE0, 0x4C, 0x68, 0x12, 0x34};
        memcpy(EF.mac, MACX, 6);
        memcpy(CONN.bssid, AP_BSSID, 6);
        unsigned char fr[32 + 99 + 4];
        memset(fr, 0, sizeof fr);
        fr[0] = 0x08;
        fr[1] = 0x02;
        memcpy(fr + 4, MACX, 6);
        memcpy(fr + 10, AP_BSSID, 6);
        memcpy(fr + 16, AP_BSSID, 6);
        static const unsigned char LLC[8] = {0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x88, 0x8E};
        memcpy(fr + 24, LLC, 8);
        fr[32] = 2;
        fr[33] = 3;
        HS_LISTEN = 0;
        EAPQ.pending = 0;
        eapol_note(fr, 32 + 99);
        CHECK(!EAPQ.pending); /* not while no handshake runs */
        HS_LISTEN = 1;
        fr[10] ^= 1; /* from another station */
        eapol_note(fr, 32 + 99);
        CHECK(!EAPQ.pending);
        fr[10] ^= 1;
        fr[4] ^= 1; /* to another address */
        eapol_note(fr, 32 + 99);
        CHECK(!EAPQ.pending);
        fr[4] ^= 1;
        fr[1] = 0x01; /* a frame to the access point, not from it */
        eapol_note(fr, 32 + 99);
        CHECK(!EAPQ.pending);
        fr[1] = 0x02;
        fr[30] = 0x08; /* another ethertype */
        eapol_note(fr, 32 + 99);
        CHECK(!EAPQ.pending);
        fr[30] = 0x88;
        eapol_note(fr, 32 + 99);
        CHECK(EAPQ.pending && EAPQ.len == 99 && EAPQ.b[0] == 2 && EAPQ.b[1] == 3);
        eapol_note(fr, 32 + 99); /* one is waiting: the next one is counted as dropped */
        CHECK(EAPQ.dropped == 1);
        EAPQ.pending = 0;
        /* a QoS data frame has two more bytes of header */
        unsigned char q[2 + 32 + 99 + 4];
        memset(q, 0, sizeof q);
        memcpy(q, fr, 24);
        q[0] = 0x88;
        memcpy(q + 26, fr + 24, 8 + 99);
        eapol_note(q, 2 + 32 + 99);
        CHECK(EAPQ.pending && EAPQ.len == 99 && EAPQ.b[1] == 3);
        EAPQ.pending = 0;
        HS_LISTEN = 0;
    }

    /* ---- stage 6c-2: the security engine and the key table against Linux' own rtw_sec_enable_sec_engine and rtw_sec_write_cam (the register
     * writes, in order: the same event hash as for the tables) ---- */
    {
        unsigned char pk[16], gk[16];
        for (unsigned i = 0; i < 16; i++)
        {
            pk[i] = (unsigned char)(0xA0 + i);
            gk[i] = (unsigned char)(0x40 + i);
        }
        static const unsigned char AP6[6] = {0x3c, 0x78, 0x95, 0x07, 0x75, 0x87}, BC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        chip_for_download();
        memset(REGS, 0, sizeof REGS);
        REGS[0x100] = 0xFF;
        CAP_N = 0;
        CAP_H = 1469598103934665603ULL;
        CAPTURE = 1;
        sec_enable();
        cam_write(4, pk, 0, 0, AP6);
        cam_write(1, gk, 1, 1, BC);
        CAPTURE = 0;
        CHECK(CAP_N == 34 && CAP_H == 0x65cd1cffa28d0113ULL);
    }

    /* ---- stage 7: the data path. The pretend access point does the handshake; then Ethernet frames go out as encrypted data frames (the pretend chip
     * only records them) and protected frames the pretend chip "decrypted" come in as Ethernet frames. ---- */
    {
        static const unsigned char MAC6[6] = {0x00, 0xE0, 0x4C, 0x68, 0x12, 0x34};
        static const unsigned char GW[6] = {0x02, 0x00, 0x00, 0x00, 0xB0, 0x01}; /* a host behind the access point */
        static unsigned char beacon[128];
        static const unsigned char RSN_OK2[20] = {1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F, 0xAC, 2, 0, 0};
        memset(beacon, 0, sizeof beacon);
        beacon[0] = 0x80;
        memset(beacon + 4, 0xFF, 6);
        memcpy(beacon + 10, AP_BSSID, 6);
        memcpy(beacon + 16, AP_BSSID, 6);
        beacon[34] = 0x31;
        beacon[35] = 0x14;
        unsigned p = 36;
        beacon[p++] = 0;
        beacon[p++] = 7;
        memcpy(beacon + p, "TestNet", 7);
        p += 7;
        beacon[p++] = 3;
        beacon[p++] = 1;
        beacon[p++] = 149;
        beacon[p++] = 48;
        beacon[p++] = 20;
        memcpy(beacon + p, RSN_OK2, 20);
        p += 20;
        p += 4;
        CFG_TEXT = "ssid=TestNet\npsk=secret-pass-phrase\n";
        ONLINE_ENABLE = 1;
        TXP_ENABLE = 1;
        TX_ANSWER = 0;
        AP_MODE = 0;
        AP_HS = 1;
        AP_PASS = "secret-pass-phrase";
        AP_BAD_MIC = 0;
        build_image(1);
        chip_for_download();
        file(IMG, IMG_SIZE, IMG_SIZE);
        RXQ_N = RXQ_NEXT = 0;
        RX_HW = 0;
        TXLOG_N = 0;
        AP_RESP_N = 0;
        memset(&BECAP, 0, sizeof BECAP);
        rxq_add_frame(beacon, p);
        CHECK(NIC_RTL8821CE.probe(&WIFI) == 1); /* connected: a working adapter, the card stays on */
        CHECK(LINK.up && HS.result == HS_OK && HS.keys);
        CHECK(NIC_RTL8821CE.link_up() == 1 && memcmp(NIC_RTL8821CE.mac(), MAC6, 6) == 0);
        {
            char sl[96];
            NIC_RTL8821CE.status(sl, sizeof sl);
            CHECK(strcmp(sl, "RTL8821CE Wi-Fi: TestNet ch149, up, tx 0 rx 0") == 0);
        }
        /* no deauthentication was sent: the link stays */
        for (int i = 0; i < TXLOG_N; i++)
            CHECK(TXLOG[i].b[0] != 0xC0);

        /* send an ARP request (broadcast) and an ICMP-sized frame */
        unsigned char arp[42];
        memset(arp, 0, sizeof arp);
        memset(arp, 0xFF, 6);
        memcpy(arp + 6, MAC6, 6);
        arp[12] = 0x08;
        arp[13] = 0x06;
        for (unsigned i = 14; i < 42; i++)
            arp[i] = (unsigned char)(i * 3);
        CHECK(NIC_RTL8821CE.send(arp, 42) == 0);
        CHECK(BECAP.count == 1 && BECAP.idx_written == 1);
        {
            const unsigned char *fr = BECAP.frame;
            CHECK(BECAP.len == 24 + 8 + 8 + 28);
            CHECK(fr[0] == 0x08 && fr[1] == 0x41 && fr[2] == 0 && fr[3] == 0);                                  /* data, to the AP, protected */
            CHECK(memcmp(fr + 4, AP_BSSID, 6) == 0 && memcmp(fr + 10, MAC6, 6) == 0 && memcmp(fr + 16, arp, 6) == 0);
            CHECK(fr[22] == 0x00 && fr[23] == 0x00);                                                              /* sequence 0 */
            static const unsigned char CCMP1[8] = {1, 0, 0, 0x20, 0, 0, 0, 0};                                    /* packet number 1, key id 0, extended IV */
            CHECK(memcmp(fr + 24, CCMP1, 8) == 0);
            static const unsigned char LLC[8] = {0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x08, 0x06};
            CHECK(memcmp(fr + 32, LLC, 8) == 0 && memcmp(fr + 40, arp + 14, 28) == 0);
            unsigned w[10];
            memcpy(w, BECAP.desc, 40);
            CHECK((w[0] & 0xFFFF) == BECAP.len && ((w[0] >> 16) & 0xFF) == 48 && !(w[0] & BIT(24)) && (w[0] & BIT(26)) && !(w[0] & BIT(31)));
            CHECK((w[1] & 0xFF) == 0 && ((w[1] >> 8) & 0x1F) == 0 && ((w[1] >> 22) & 3) == 3);                   /* queue 0 (best effort), security type AES */
            CHECK((w[3] & BIT(8)) && (w[3] & BIT(10)) && (w[4] & 0x7F) == 8 && (w[8] & BIT(15)) == 0 && (w[9] >> 12) == 0);
            unsigned bd0 = BECAP.bd[0] | BECAP.bd[1] << 8, bd1 = BECAP.bd[8] | BECAP.bd[9] << 8;
            CHECK(bd0 == 48 && bd1 == BECAP.len);
        }
        CHECK(NIC_RTL8821CE.send(arp, 42) == 0);
        CHECK(BECAP.count == 2 && BECAP.frame[24] == 2 && BECAP.frame[22] == 0x10 && BECAP.frame[23] == 0x00); /* the next packet number and sequence number */
        CHECK(NIC_RTL8821CE.send(arp, 13) == -1 && NIC_RTL8821CE.send(0, 42) == -1);                            /* shorter than an Ethernet header */

        /* receive: a helper that builds what the chip hands over after decrypting */
#define AP_DATA(buf, da, sa, qos, ethertype, payload, plen, pn, protect)                                          \
    do                                                                                                            \
    {                                                                                                             \
        unsigned _h = (qos) ? 26 : 24, _n;                                                                        \
        memset(buf, 0, sizeof buf);                                                                               \
        buf[0] = (qos) ? 0x88 : 0x08;                                                                             \
        buf[1] = (protect) ? 0x42 : 0x02;                                                                         \
        memcpy(buf + 4, da, 6);                                                                                   \
        memcpy(buf + 10, AP_BSSID, 6);                                                                            \
        memcpy(buf + 16, sa, 6);                                                                                  \
        _n = _h;                                                                                                  \
        buf[_n++] = (unsigned char)(pn);                                                                          \
        buf[_n++] = (unsigned char)((pn) >> 8);                                                                   \
        buf[_n++] = 0;                                                                                            \
        buf[_n++] = 0x20;                                                                                                                                 \
        buf[_n++] = 0;                                                                                            \
        buf[_n++] = 0;                                                                                            \
        buf[_n++] = 0;                                                                                            \
        buf[_n++] = 0;                                                                                            \
        memcpy(buf + _n, "\xAA\xAA\x03\x00\x00\x00", 6);                                                          \
        _n += 6;                                                                                                  \
        buf[_n++] = (unsigned char)((ethertype) >> 8);                                                            \
        buf[_n++] = (unsigned char)(ethertype);                                                                   \
        memcpy(buf + _n, payload, plen);                                                                          \
        _n += (plen);                                                                                             \
        _n += 8 + 4; /* the MIC the chip leaves, and the FCS */                                                   \
        rxlen = _n;                                                                                               \
    } while (0)
        unsigned char rb[200];
        unsigned rxlen = 0;
        unsigned char arp_reply[28];
        for (unsigned i = 0; i < 28; i++)
            arp_reply[i] = (unsigned char)(0x90 + i);
        unsigned char got[ETH_MAX];
        /* 1. an ARP reply to us: unicast, protected, decrypted; the MIC after the 28 bytes is cut off */
        AP_DATA(rb, MAC6, GW, 0, 0x0806, arp_reply, 28, 1, 1);
        rxq_add_frame_w0(rb, rxlen, 4u << 20);
        for (int i = 0; i < 4; i++)
            rtl8821ce_host_udelay(1);
        int n = NIC_RTL8821CE.recv(got, sizeof got);
        CHECK(n == 14 + 28 && memcmp(got, MAC6, 6) == 0 && memcmp(got + 6, GW, 6) == 0 && got[12] == 0x08 && got[13] == 0x06 && memcmp(got + 14, arp_reply, 28) == 0);
        CHECK(NIC_RTL8821CE.recv(got, sizeof got) == 0);
        /* 2. an IPv4 packet in a QoS data frame, broadcast: the IP header says how long it is (28 bytes), the rest is the MIC and padding */
        unsigned char ip[40];
        memset(ip, 0, sizeof ip);
        ip[0] = 0x45;
        ip[2] = 0;
        ip[3] = 28;
        for (unsigned i = 4; i < 28; i++)
            ip[i] = (unsigned char)(i + 100);
        unsigned char bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        AP_DATA(rb, bcast, GW, 1, 0x0800, ip, 40, 2, 1);
        rxq_add_frame_w0(rb, rxlen, 4u << 20);
        for (int i = 0; i < 4; i++)
            rtl8821ce_host_udelay(1);
        n = NIC_RTL8821CE.recv(got, sizeof got);
        CHECK(n == 14 + 28 && memcmp(got, bcast, 6) == 0 && got[12] == 0x08 && got[13] == 0x00 && memcmp(got + 14, ip, 28) == 0);
        /* 3. frames that must not get through: replayed number, unprotected, decryption error, not decrypted by the chip, to another station,
         * from another access point, our own broadcast sent back, a null frame */
        unsigned char other[6] = {2, 0, 0, 0, 0, 9};
        AP_DATA(rb, MAC6, GW, 0, 0x0806, arp_reply, 28, 1, 1);
        rxq_add_frame_w0(rb, rxlen, 4u << 20); /* replay: the unicast counter is at 1 */
        AP_DATA(rb, MAC6, GW, 0, 0x0806, arp_reply, 28, 9, 0);
        rxq_add_frame_w0(rb, rxlen, 0); /* unprotected */
        AP_DATA(rb, MAC6, GW, 0, 0x0806, arp_reply, 28, 9, 0);
        rxq_add_frame_w0(rb, rxlen, 4u << 20); /* the protected bit is clear although the descriptor claims decryption: still not taken */
        AP_DATA(rb, MAC6, GW, 0, 0x0806, arp_reply, 28, 9, 1);
        rxq_add_frame_w0(rb, rxlen, 1u << 15); /* ICV error */
        AP_DATA(rb, MAC6, GW, 0, 0x0806, arp_reply, 28, 9, 1);
        rxq_add_frame_w0(rb, rxlen, (4u << 20) | (1u << 27)); /* software decrypt: the chip did not */
        AP_DATA(rb, MAC6, GW, 0, 0x0806, arp_reply, 28, 9, 1);
        rxq_add_frame_w0(rb, rxlen, 0); /* protected bit set but the chip reports no encryption */
        AP_DATA(rb, other, GW, 0, 0x0806, arp_reply, 28, 9, 1);
        rxq_add_frame_w0(rb, rxlen, 4u << 20); /* to another station */
        AP_DATA(rb, MAC6, GW, 0, 0x0806, arp_reply, 28, 9, 1);
        rb[10] ^= 1;
        rxq_add_frame_w0(rb, rxlen, 4u << 20); /* from another access point */
        AP_DATA(rb, bcast, MAC6, 0, 0x0806, arp_reply, 28, 9, 1);
        rxq_add_frame_w0(rb, rxlen, 4u << 20); /* our own broadcast, sent back */
        AP_DATA(rb, MAC6, GW, 0, 0x0806, arp_reply, 28, 9, 1);
        rb[0] = 0x48; /* a null data frame */
        rxq_add_frame_w0(rb, rxlen, 4u << 20);
        for (int i = 0; i < 20; i++)
            rtl8821ce_host_udelay(1);
        CHECK(NIC_RTL8821CE.recv(got, sizeof got) == 0 && LINK.rx_frames == 2);
        CHECK(LINK.rx_replay == 1);
        /* 4. a broadcast with its own counter (the group key's), a higher unicast number works */
        AP_DATA(rb, bcast, GW, 0, 0x0806, arp_reply, 28, 1, 1); /* group counter starts after 2: number 1 is a replay */
        rxq_add_frame_w0(rb, rxlen, 4u << 20);
        AP_DATA(rb, bcast, GW, 0, 0x0806, arp_reply, 28, 3, 1);
        rxq_add_frame_w0(rb, rxlen, 4u << 20);
        AP_DATA(rb, MAC6, GW, 0, 0x0806, arp_reply, 28, 5, 1);
        rxq_add_frame_w0(rb, rxlen, 4u << 20);
        for (int i = 0; i < 10; i++)
            rtl8821ce_host_udelay(1);
        CHECK(NIC_RTL8821CE.recv(got, sizeof got) == 42 && NIC_RTL8821CE.recv(got, sizeof got) == 42 && NIC_RTL8821CE.recv(got, sizeof got) == 0);
        /* 5. the queue of received frames is bounded: what the stack does not take is dropped, nothing is overwritten */
        for (int k = 0; k < 12; k++)
        {
            AP_DATA(rb, MAC6, GW, 0, 0x0806, arp_reply, 28, 10 + k, 1);
            rxq_add_frame_w0(rb, rxlen, 4u << 20);
        }
        for (int i = 0; i < 30; i++)
            rtl8821ce_host_udelay(1);
        int taken = 0;
        while (NIC_RTL8821CE.recv(got, sizeof got) > 0)
            taken++;
        CHECK(taken == 7 && LINK.rx_dropped >= 5); /* the 8 slots hold 7 frames */
        /* 5b. the group key renewal: answered (encrypted, signed), installed in the chip, never shown to the network stack */
#define PUMP(n)                                    \
    do                                             \
    {                                              \
        for (int _i = 0; _i < (n); _i++)           \
            rtl8821ce_host_udelay(1);              \
    } while (0)
#define GK_SEND(msg, mlen, pn)                                          \
    do                                                                  \
    {                                                                   \
        AP_DATA(rb2, MAC6, AP_BSSID, 0, 0x888E, msg, mlen, pn, 1);       \
        rxq_add_frame_w0(rb2, rxlen, 4u << 20);                         \
        PUMP(10);                                                       \
        NIC_RTL8821CE.recv(got, sizeof got);                            \
    } while (0)
        {
            unsigned char rb2[260];
            unsigned char m[99 + 56];
            unsigned ml = ap_group_msg1(m, 3, 2, 0x70); /* replay counter 3 (the handshake used 1 and 2), key id 2, key 70 71 72 ... */
            int be0 = BECAP.count;
            NLOG = 0;
            GK_SEND(m, ml, 1000);
            CHECK(LINK.rekeys == 1 && LINK.eapol_bad == 0 && BECAP.count == be0 + 1);
            CHECK(NIC_RTL8821CE.recv(got, sizeof got) == 0); /* nothing for the network stack */
            const unsigned char *fr = BECAP.frame;
            CHECK(fr[1] == 0x41 && memcmp(fr + 4, AP_BSSID, 6) == 0 && fr[38] == 0x88 && fr[39] == 0x8E); /* protected data frame to the access point, EAPOL */
            const unsigned char *e = fr + 40;
            CHECK(BECAP.len == 40 + 99 && e[1] == 3 && ((e[5] << 8) | e[6]) == 0x0302 && memcmp(e + 9, m + 9, 8) == 0 && e[97] == 0 && e[98] == 0);
            unsigned char mic[16];
            wpa_eapol_mic(AP_PTK, e, 99, mic);
            CHECK(memcmp(mic, e + 81, 16) == 0);
            int wrote_key = 0, wrote_cmd = 0;
            for (int i = 0; i < NLOG; i++)
            {
                if (LOG[i].off == 0x674 && LOG[i].value == 0x73727170u) /* the first four key bytes, as one word */
                    wrote_key = 1;
                if (LOG[i].off == 0x670 && LOG[i].value == (BIT(16) | BIT(31) | 16u)) /* the command for the first word of entry 2 */
                    wrote_cmd = 1;
            }
            CHECK(wrote_key && wrote_cmd);

            /* traffic under the new key id has its own packet counter (starting again), the old key's counter goes on */
            unsigned rep0 = LINK.rx_replay;
            AP_DATA(rb, bcast, GW, 0, 0x0806, arp_reply, 28, 1, 1);
            rb[24 + 3] = 0x20 | (2 << 6);
            rxq_add_frame_w0(rb, rxlen, 4u << 20);
            AP_DATA(rb, bcast, GW, 0, 0x0806, arp_reply, 28, 1, 1); /* the same number again under key id 2: a replay */
            rb[24 + 3] = 0x20 | (2 << 6);
            rxq_add_frame_w0(rb, rxlen, 4u << 20);
            AP_DATA(rb, bcast, GW, 0, 0x0806, arp_reply, 28, 4, 1); /* the old key (id 0): its counter was at 3 */
            rxq_add_frame_w0(rb, rxlen, 4u << 20);
            AP_DATA(rb, bcast, GW, 0, 0x0806, arp_reply, 28, 2, 1); /* ... and 2 is old news */
            rxq_add_frame_w0(rb, rxlen, 4u << 20);
            PUMP(10);
            CHECK(NIC_RTL8821CE.recv(got, sizeof got) == 42 && NIC_RTL8821CE.recv(got, sizeof got) == 42 && NIC_RTL8821CE.recv(got, sizeof got) == 0);
            CHECK(LINK.rx_replay == rep0 + 2);

            /* refused: the counter of the last one again, a damaged signature, a pairwise message 1 (a new PTK is not handled) */
            unsigned rk0 = LINK.rekeys, bad0 = LINK.eapol_bad;
            be0 = BECAP.count;
            NLOG = 0;
            ml = ap_group_msg1(m, 3, 3, 0x20);
            GK_SEND(m, ml, 1001);
            ml = ap_group_msg1(m, 4, 3, 0x20);
            m[85] ^= 1;
            GK_SEND(m, ml, 1002);
            ap_eapol_frame(m, 0x008A, 10, (const unsigned char *)"", 0);
            GK_SEND(m, 99, 1003);
            int cam_cmds = 0;
            for (int i = 0; i < NLOG; i++)
                if (LOG[i].off == 0x670)
                    cam_cmds++;
            CHECK(LINK.rekeys == rk0 && LINK.eapol_bad == bad0 + 2 && BECAP.count == be0 && cam_cmds == 0);
            /* a damaged frame did not move the replay counter: the right message with counter 4 works, key id 3 */
            ml = ap_group_msg1(m, 4, 3, 0x20);
            GK_SEND(m, ml, 1004);
            CHECK(LINK.rekeys == rk0 + 1 && BECAP.count == be0 + 1);

            /* message 3 of the 4-way handshake again (our message 4 got lost): answered with message 4, the keys are not installed again */
            be0 = BECAP.count;
            rk0 = LINK.rekeys;
            NLOG = 0;
            ml = ap_msg3_again(m, 9);
            GK_SEND(m, ml, 1005);
            CHECK(BECAP.count == be0 + 1 && LINK.rekeys == rk0);
            e = BECAP.frame + 40;
            CHECK(BECAP.len == 40 + 99 && ((e[5] << 8) | e[6]) == 0x030A && memcmp(e + 9, m + 9, 8) == 0);
            wpa_eapol_mic(AP_PTK, e, 99, mic);
            CHECK(memcmp(mic, e + 81, 16) == 0);
            cam_cmds = 0;
            for (int i = 0; i < NLOG; i++)
                if (LOG[i].off == 0x670)
                    cam_cmds++;
            CHECK(cam_cmds == 0);
        }
#undef GK_SEND
        /* a deauthentication from the access point, which then stays silent: the link is down, and the card tries again by itself, with growing pauses */
        CHECK(NIC_RTL8821CE.link_up() == 1);
        unsigned char dr[40];
#define DEAUTH(dst)                          \
    do                                       \
    {                                        \
        memset(dr, 0, sizeof dr);            \
        dr[0] = 0xC0;                        \
        memcpy(dr + 4, dst, 6);              \
        memcpy(dr + 10, AP_BSSID, 6);        \
        memcpy(dr + 16, AP_BSSID, 6);        \
        dr[24] = 7;                          \
        rxq_add_frame_w0(dr, 24 + 2 + 4, 0); \
        PUMP(6);                             \
    } while (0)
        {
            char sl[96];
            AP_MODE = 1;
            DEAUTH(MAC6);
            NIC_RTL8821CE.recv(got, sizeof got); /* the first attempt is at once: nobody answers */
            CHECK(NIC_RTL8821CE.link_up() == 0 && NIC_RTL8821CE.send(arp, 42) == -1);
            CHECK(LINK.fails == 1 && LINK.reconnects == 0);
            NIC_RTL8821CE.status(sl, sizeof sl);
            CHECK(strstr(sl, "reconnecting") != 0);
            int tx0 = TXCAP.count;
            NIC_RTL8821CE.recv(got, sizeof got); /* too early for the next one */
            CHECK(TXCAP.count == tx0 && LINK.fails == 1);
            MS_JUMP += 600; /* the pause was 500 ms */
            NIC_RTL8821CE.recv(got, sizeof got);
            CHECK(TXCAP.count > tx0 && LINK.fails == 2 && LINK.next_try - rtl8821ce_host_ms() > 900 && LINK.next_try - rtl8821ce_host_ms() <= 1000); /* the pause doubled */
            /* the third attempt scans first (the access point may have moved) and finds it again; it answers now */
            AP_MODE = 0;
            rxq_add_frame(beacon, p);
            MS_JUMP += 1100;
            NIC_RTL8821CE.recv(got, sizeof got);
            CHECK(NIC_RTL8821CE.link_up() == 1 && LINK.reconnects == 1 && LINK.fails == 0 && AP_GOT_MSG4 == 1 && SC.n >= 1 && TARGET.found);
            NIC_RTL8821CE.status(sl, sizeof sl);
            CHECK(strstr(sl, ", up") != 0 && strstr(sl, ", rc 1") != 0 && strstr(sl, ", gk 2") != 0);
            /* the new keys count from the start; the transmit ring goes on where it was */
            CHECK(NIC_RTL8821CE.send(arp, 42) == 0 && BECAP.frame[24] == 1 && BECAP.idx_written == LINK.be_wp);
            /* a frame under the new pairwise key arrives */
            unsigned char hello[28];
            memset(hello, 0x5A, sizeof hello);
            AP_DATA(rb, MAC6, GW, 0, 0x0806, hello, 28, 1, 1);
            rxq_add_frame_w0(rb, rxlen, 4u << 20);
            PUMP(6);
            CHECK(NIC_RTL8821CE.recv(got, sizeof got) == 42 && memcmp(got + 14, hello, 28) == 0);
        }
        /* an access point that says goodbye to everybody (a deauthentication to the broadcast address) */
        {
            unsigned r0 = LINK.reconnects;
            DEAUTH(bcast);
            NIC_RTL8821CE.recv(got, sizeof got);
            CHECK(NIC_RTL8821CE.link_up() == 1 && LINK.reconnects == r0 + 1);
        }
        /* silence: heard now and then, nothing at all for 3 s: it is asked (a directed probe request, once a second), after 9 s it is given up */
        {
            unsigned r0 = LINK.reconnects;
            int tx0 = TXCAP.count;
            MS_JUMP += 2500;
            NIC_RTL8821CE.recv(got, sizeof got);
            CHECK(TXCAP.count == tx0 && NIC_RTL8821CE.link_up() == 1);
            rxq_add_frame(beacon, p); /* a beacon: it is there */
            PUMP(4);
            NIC_RTL8821CE.recv(got, sizeof got);
            MS_JUMP += 2500;
            NIC_RTL8821CE.recv(got, sizeof got);
            CHECK(TXCAP.count == tx0); /* 2.5 s since the beacon: no question yet */
            MS_JUMP += 1000;
            NIC_RTL8821CE.recv(got, sizeof got);
            CHECK(TXCAP.count == tx0 + 1);
            CHECK(TXCAP.frame[0] == 0x40 && memcmp(TXCAP.frame + 4, AP_BSSID, 6) == 0 && memcmp(TXCAP.frame + 10, MAC6, 6) == 0 && memcmp(TXCAP.frame + 16, AP_BSSID, 6) == 0);
            NIC_RTL8821CE.recv(got, sizeof got);
            CHECK(TXCAP.count == tx0 + 1); /* not twice in a second */
            MS_JUMP += 1100;
            NIC_RTL8821CE.recv(got, sizeof got);
            CHECK(TXCAP.count == tx0 + 2 && NIC_RTL8821CE.link_up() == 1 && LINK.reconnects == r0);
            MS_JUMP += 6000; /* 10 s without a word */
            NIC_RTL8821CE.recv(got, sizeof got);
            CHECK(NIC_RTL8821CE.link_up() == 1 && LINK.reconnects == r0 + 1);
        }
#undef DEAUTH
#undef PUMP
#undef AP_DATA
        ONLINE_ENABLE = 0;
        TXP_ENABLE = 0;
        AP_HS = 0;
        CFG_TEXT = 0;
    }

    /* ---- stage 8: speed. An access point with HT / VHT: the association request offers them, the firmware is told about the access point (media
     * status + RA info, with the exact words Linux would build), and data goes out as QoS data frames with a rate table instead of a fixed rate. ---- */
    {
        static const unsigned char MAC6[6] = {0x00, 0xE0, 0x4C, 0x68, 0x12, 0x34};
        static const unsigned char GW[6] = {0x02, 0x00, 0x00, 0x00, 0xB0, 0x01};
        static unsigned char bc[200];
        unsigned bl;
        unsigned char arp[42];
        memset(arp, 0, sizeof arp);
        memset(arp, 0xFF, 6);
        memcpy(arp + 6, MAC6, 6);
        arp[12] = 0x08;
        arp[13] = 0x06;
#define CONNECT8(cfg)                    \
    do                                   \
    {                                    \
        CFG_TEXT = cfg;                  \
        ONLINE_ENABLE = 1;               \
        TXP_ENABLE = 1;                  \
        TX_ANSWER = 0;                   \
        AP_MODE = 0;                     \
        AP_HS = 1;                       \
        AP_PASS = "secret-pass-phrase";  \
        AP_BAD_MIC = 0;                  \
        build_image(1);                  \
        chip_for_download();             \
        file(IMG, IMG_SIZE, IMG_SIZE);   \
        RXQ_N = RXQ_NEXT = 0;            \
        RX_HW = 0;                       \
        TXLOG_N = 0;                     \
        AP_RESP_N = 0;                   \
        H2CLOG_N = 0;                    \
        memset(&BECAP, 0, sizeof BECAP); \
        rxq_add_frame(bc, bl);           \
    } while (0)
#define FIND_ASSOC(var)                                    \
    const TxFrame *var = 0;                                \
    for (int _i = 0; _i < TXLOG_N; _i++)                   \
        if (TXLOG[_i].b[0] == 0x00 && TXLOG[_i].len >= 40) \
            var = &TXLOG[_i];
        unsigned w[10];
        unsigned char got[ETH_MAX];

        /* A: 5 GHz, HT and VHT (the first stream up to MCS 9) */
        bl = build_beacon_caps(bc, 149, 1, 1, 0xFFFE);
        CONNECT8("ssid=TestNet\npsk=secret-pass-phrase\n");
        CHECK(NIC_RTL8821CE.probe(&WIFI) == 1 && LINK.up && RA.on && RA.vht && RA.ht);
        CHECK(TARGET.found && SC.ap[TARGET.index].rates == 0x0FF0 && SC.ap[TARGET.index].ht && SC.ap[TARGET.index].ht_mcs0 == 0xFF &&
              SC.ap[TARGET.index].ht_cap == 0x2C && SC.ap[TARGET.index].vht && SC.ap[TARGET.index].vht_map == 0xFFFE);
        {
            FIND_ASSOC(as);
            CHECK(as != 0);
            if (as)
            {
                CHECK(((as->b[24] | as->b[25] << 8) & 0x0200) != 0); /* the QoS capability bit */
                const unsigned char *ie = find_ie(as->b, as->len, 28, 45);
                CHECK(ie && ie[1] == 26 && ie[2] == 0x2C && ie[3] == 0 && ie[5] == 0xFF && ie[15] == 72 && ie[16] == 0 && ie[17] == 0);
                ie = find_ie(as->b, as->len, 28, 191);
                CHECK(ie && ie[1] == 12 && ie[2] == 0 && ie[6] == 0xFD && ie[7] == 0xFF && ie[8] == 87 && ie[10] == 0xFD && ie[11] == 0xFF && ie[12] == 87);
                ie = find_ie(as->b, as->len, 28, 199);
                CHECK(ie && ie[1] == 1 && ie[2] == 0x00); /* "20 MHz, one stream" */
                ie = find_ie(as->b, as->len, 28, 221);
                CHECK(ie && ie[1] == 7 && memcmp(ie + 2, "\x00\x50\xF2\x02\x00\x01\x00", 7) == 0);
                CHECK(find_ie(as->b, as->len, 28, 48) != 0); /* the security element is still there */
            }
        }
        /* the commands: "ignore WLAN actions" from the stage before, then RA info (rate table 10, level 1, short GI, VHT, mask: VHT MCS 0-9 and 6 Mb/s) and "station 0 is connected" */
        CHECK(H2CLOG_N == 3);
        CHECK(H2CLOG[0].box_off == 0x1D0 && H2CLOG[0].w0 == 0x63);
        CHECK(H2CLOG[1].box_off == 0x1D4 && H2CLOG[1].w0 == 0x50AA0040u && H2CLOG[1].w1 == 0x003FF010u);
        CHECK(H2CLOG[2].box_off == 0x1D8 && H2CLOG[2].w0 == 0x00000101u && H2CLOG[2].w1 == 0);
        {
            char sl[96];
            NIC_RTL8821CE.status(sl, sizeof sl);
            CHECK(strcmp(sl, "RTL8821CE Wi-Fi: TestNet ch149, up, vht, tx 0 rx 0") == 0);
        }
        CHECK(NIC_RTL8821CE.send(arp, 42) == 0 && BECAP.len == 26 + 8 + 8 + 28);
        {
            const unsigned char *fr = BECAP.frame;
            CHECK(fr[0] == 0x88 && fr[1] == 0x41 && fr[24] == 0 && fr[25] == 0); /* QoS data, QoS control: traffic class 0 */
            CHECK(fr[26] == 1 && fr[29] == 0x20);                                /* the CCMP header after the longer header */
            static const unsigned char LLC[8] = {0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x08, 0x06};
            CHECK(memcmp(fr + 34, LLC, 8) == 0 && memcmp(fr + 42, arp + 14, 28) == 0);
            memcpy(w, BECAP.desc, 40);
            CHECK(((w[1] >> 16) & 0x1F) == 10 && ((w[1] >> 22) & 3) == 3); /* rate table 10, AES */
            CHECK((w[3] & (BIT(8) | BIT(10))) == 0 && (w[4] & 0x7F) == 0x2C + 9 && (w[5] & 0x60) == 0); /* no fixed rate, fallback allowed; 20 MHz */
        }
        /* an A-MSDU is not understood: dropped; a plain QoS data frame is taken */
        {
            unsigned char q[120];
            memset(q, 0, sizeof q);
            q[0] = 0x88;
            q[1] = 0x42;
            memcpy(q + 4, MAC6, 6);
            memcpy(q + 10, AP_BSSID, 6);
            memcpy(q + 16, GW, 6);
            q[26] = 1;
            q[29] = 0x20;
            memcpy(q + 34, "\xAA\xAA\x03\x00\x00\x00\x08\x06", 8);
            memset(q + 42, 0x77, 28);
            rxq_add_frame_w0(q, 26 + 8 + 8 + 28 + 12, 4u << 20);
            q[26] = 2;
            q[24] = 0x80;
            rxq_add_frame_w0(q, 26 + 8 + 8 + 28 + 12, 4u << 20);
            for (int i = 0; i < 10; i++)
                rtl8821ce_host_udelay(1);
            unsigned d0 = LINK.rx_dropped;
            CHECK(NIC_RTL8821CE.recv(got, sizeof got) == 42 && got[14] == 0x77 && NIC_RTL8821CE.recv(got, sizeof got) == 0 && LINK.rx_dropped == d0 + 1);
        }
        /* the link is lost and made again: the firmware hears "station 0 left", then the same two commands as the first time */
        {
            unsigned char dr[40];
            memset(dr, 0, sizeof dr);
            dr[0] = 0xC0;
            memcpy(dr + 4, MAC6, 6);
            memcpy(dr + 10, AP_BSSID, 6);
            memcpy(dr + 16, AP_BSSID, 6);
            dr[24] = 7;
            rxq_add_frame_w0(dr, 24 + 2 + 4, 0);
            for (int i = 0; i < 6; i++)
                rtl8821ce_host_udelay(1);
            NIC_RTL8821CE.recv(got, sizeof got);
            CHECK(NIC_RTL8821CE.link_up() == 1 && LINK.reconnects == 1 && RA.on);
            CHECK(H2CLOG_N == 6);
            CHECK(H2CLOG[3].box_off == 0x1DC && H2CLOG[3].w0 == 0x00000001u);
            CHECK(H2CLOG[4].box_off == 0x1D0 && H2CLOG[4].w0 == 0x50AA0040u && H2CLOG[4].w1 == 0x003FF010u);
            CHECK(H2CLOG[5].box_off == 0x1D4 && H2CLOG[5].w0 == 0x00000101u);
        }

        /* B: 5 GHz, HT only (no VHT element): rate table 5, the HT rates and 6 / 9 Mb/s; no VHT and no operating mode element in the request */
        bl = build_beacon_caps(bc, 149, 1, 0, 0);
        CONNECT8("ssid=TestNet\npsk=secret-pass-phrase\n");
        CHECK(NIC_RTL8821CE.probe(&WIFI) == 1 && RA.on && !RA.vht && RA.ht);
        {
            FIND_ASSOC(as);
            CHECK(as && find_ie(as->b, as->len, 28, 45) && !find_ie(as->b, as->len, 28, 191) && !find_ie(as->b, as->len, 28, 199) && find_ie(as->b, as->len, 28, 221));
        }
        CHECK(H2CLOG_N == 3 && H2CLOG[1].w0 == 0x40A50040u && H2CLOG[1].w1 == 0x000FF030u && H2CLOG[2].w0 == 0x101);
        CHECK(NIC_RTL8821CE.send(arp, 42) == 0);
        memcpy(w, BECAP.desc, 40);
        CHECK(((w[1] >> 16) & 0x1F) == 5 && (w[3] & (BIT(8) | BIT(10))) == 0 && (w[4] & 0x7F) == 0x13);
        {
            char sl[96];
            NIC_RTL8821CE.status(sl, sizeof sl);
            CHECK(strstr(sl, ", up, ht, tx") != 0);
        }

        /* C: "rate=legacy" in WIFI.CFG: as before stage 8, whatever the access point can do */
        bl = build_beacon_caps(bc, 149, 1, 1, 0xFFFE);
        CONNECT8("ssid=TestNet\npsk=secret-pass-phrase\nrate=legacy\n");
        CHECK(NIC_RTL8821CE.probe(&WIFI) == 1 && !RA.on && CFG.rate == CFG_RATE_LEGACY);
        {
            FIND_ASSOC(as);
            CHECK(as && !find_ie(as->b, as->len, 28, 45) && !find_ie(as->b, as->len, 28, 191) && !find_ie(as->b, as->len, 28, 221) &&
                  ((as->b[24] | as->b[25] << 8) & 0x0200) == 0);
        }
        CHECK(H2CLOG_N == 1);
        CHECK(NIC_RTL8821CE.send(arp, 42) == 0 && BECAP.len == 24 + 8 + 8 + 28 && BECAP.frame[0] == 0x08);
        memcpy(w, BECAP.desc, 40);
        CHECK(((w[1] >> 16) & 0x1F) == 7 && (w[3] & (BIT(8) | BIT(10))) == (BIT(8) | BIT(10)) && (w[4] & 0x7F) == 8);
        /* "rate=ht": HT but no VHT */
        bl = build_beacon_caps(bc, 149, 1, 1, 0xFFFE);
        CONNECT8("ssid=TestNet\npsk=secret-pass-phrase\nrate=ht\n");
        CHECK(NIC_RTL8821CE.probe(&WIFI) == 1 && RA.on && !RA.vht && CFG.rate == CFG_RATE_HT);
        {
            FIND_ASSOC(as);
            CHECK(as && find_ie(as->b, as->len, 28, 45) && !find_ie(as->b, as->len, 28, 191));
        }
        /* an unknown value is "auto" */
        bl = build_beacon_caps(bc, 149, 1, 1, 0xFFFE);
        CONNECT8("ssid=TestNet\npsk=secret-pass-phrase\nrate=turbo\n");
        CHECK(NIC_RTL8821CE.probe(&WIFI) == 1 && RA.vht && CFG.rate == CFG_RATE_AUTO);

        /* D: 2.4 GHz, HT (a VHT element there is not answered): rate table 3, HT MCS 0-7 beside CCK 1 / 5.5 and OFDM 6 Mb/s */
        bl = build_beacon_caps(bc, 6, 1, 1, 0xFFFE);
        CONNECT8("ssid=TestNet\npsk=secret-pass-phrase\n");
        CHECK(NIC_RTL8821CE.probe(&WIFI) == 1 && RA.on && !RA.vht && CONN.channel == 6);
        {
            FIND_ASSOC(as);
            CHECK(as && find_ie(as->b, as->len, 28, 45) && !find_ie(as->b, as->len, 28, 191));
        }
        CHECK(H2CLOG_N == 3 && H2CLOG[1].w0 == 0x40A30040u && H2CLOG[1].w1 == 0x000FF015u);

        /* an access point without HT: the fixed rate, no QoS */
        bl = build_beacon_caps(bc, 149, 0, 0, 0);
        CONNECT8("ssid=TestNet\npsk=secret-pass-phrase\n");
        CHECK(NIC_RTL8821CE.probe(&WIFI) == 1 && !RA.on && H2CLOG_N == 1);
        CHECK(NIC_RTL8821CE.send(arp, 42) == 0 && BECAP.frame[0] == 0x08);
#undef CONNECT8
#undef FIND_ASSOC
        ONLINE_ENABLE = 0;
        TXP_ENABLE = 0;
        AP_HS = 0;
        CFG_TEXT = 0;
    }

    /* a firmware without extended memory: two parts only */
    build_image(0);
    chip_for_download();
    file(IMG, IMG_SIZE, IMG_SIZE);
    NIC_RTL8821CE.probe(&WIFI);
    CHECK(DL.state == DL_OK && NDDMA == 5 && PIECES == 5 && BOOTED);

    /* a powered-on chip with no firmware resident is cycled first, and then the firmware goes in */
    build_image(1);
    chip(0x00, 0x00, 0x0000);
    REGS[6] = 0x02;
    file(IMG, IMG_SIZE, IMG_SIZE);
    NIC_RTL8821CE.probe(&WIFI);
    CHECK(DL.state == DL_OK && W.was_on && BOOTED);
    file(0, 0, 0);

    /* ---- failures of stage 1 say why ---- */
    chip(0xFF, 0xFF, 0xFFFF);
    memset(REGS, 0xFF, sizeof REGS);
    CHECK(NIC_RTL8821CE.probe(&WIFI) == 0 && NLOG == 0); /* all ones: not powered / not there: nothing is written */
    NIC_RTL8821CE.status(st, sizeof st);
    CHECK(strstr(st, "all ones"));
    BAR_KIND = 1;
    NIC_RTL8821CE.probe(&WIFI);
    NIC_RTL8821CE.status(st, sizeof st);
    CHECK(strstr(st, "BAR 2 is not memory"));
    BAR_KIND = 2;
    NIC_RTL8821CE.probe(&WIFI);
    NIC_RTL8821CE.status(st, sizeof st);
    CHECK(strstr(st, "BAR 2 is empty"));
    BAR_KIND = 0;

    /* a cut number that has no letter */
    chip(0x5A, 0x00, 0xC078);
    set32(R_SYS_CFG1, 0x0049F000u);
    NIC_RTL8821CE.probe(&WIFI);
    NIC_RTL8821CE.status(st, sizeof st);
    CHECK(strstr(st, "cut ?"));

    if (fails == 0)
        printf("rtl8821ce tests passed\n");
    return fails ? 1 : 0;
}
