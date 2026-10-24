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

static _Alignas(4096) unsigned char REGS[0x2000];
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

void rtl8821ce_host_log(u32 off, unsigned size, u32 value)
{
    if (NLOG < 4096)
        LOG[NLOG++] = (Wr){off, size, value};
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
    else if (size == 4 && off == 0x1208 && (value & BIT(31)))
        run_ddma(value);
    else if (size == 1 && (off == 0x03 || off == 0x1D))
        maybe_boot();
}

/* ---- the pretend chip: when the driver waits, the chip finishes what it was asked to do ---- */
static int CHIP_COMPLETES = 1;
static unsigned UDELAYS;
void rtl8821ce_host_udelay(unsigned us)
{
    (void)us;
    UDELAYS++;
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

int exec_find(const char *name)
{
    strncpy(LAST_NAME, name, sizeof LAST_NAME - 1);
    return FILE_PRESENT && strcmp(name, "RTW8821C.BIN") == 0 ? 7 : -1;
}
const VfsNode *vfs_node(int id) { return id == 7 && FILE_PRESENT ? &FILE_NODE : 0; }
int vfs_read(int id, unsigned offset, void *buf, unsigned len)
{
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
    KNOB_NO_BCN_VALID = KNOB_DDMA_STUCK = KNOB_CHKSUM_BAD = KNOB_NO_BOOT = 0;
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

int main(void)
{
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
    CHECK(strstr(st, "powered on") && strstr(st, "fw 24.11 loaded and ready (80=") && strstr(st, "c078") && strstr(st, ", off again") &&
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
