/* Host test for usb_msc.c: a fake Bulk-Only device (RAM disk behind CBW / data / CSW) drives the real SCSI code. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "blk.h"
#include "usb_msc.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

#define SS 512
#define NSEC 4096
static unsigned char DISK[NSEC * SS];

/* fake device state */
static struct
{
    unsigned char cbw[31];
    int have_cbw;
    unsigned char data[65536]; /* data stage waiting to be read by the host */
    unsigned data_len;
    unsigned char sense[18];
    int status;          /* CSW status to report */
    int want_out;        /* host must send this many bytes next (WRITE) */
    unsigned char *out_dst;
    unsigned tur_fail;   /* TEST UNIT READY fails this many times first (unit attention) */
    int write_protect;
    int stall_data_once; /* the next READ data stage stalls */
    int bad_tag;         /* CSW carries a wrong tag */
    int fail_all;        /* every transfer errors */
    int resets, clears, alive;
    unsigned long long cap_blocks; /* reported by READ CAPACITY */
    unsigned last_cdb[16];
    unsigned last_rw[16]; /* CDB of the last READ / WRITE */
    int sync_unsupported;
    int dead_after;      /* device vanishes after this many commands (-1 = never) */
    int cmds;
} F;

static unsigned be32(const unsigned char *p) { return (unsigned)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void wbe32(unsigned char *p, unsigned v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static void sense_set(int key, int asc)
{
    memset(F.sense, 0, sizeof F.sense);
    F.sense[0] = 0x70;
    F.sense[2] = (unsigned char)key;
    F.sense[12] = (unsigned char)asc;
}

static void run_cbw(void)
{
    const unsigned char *c = F.cbw + 15;
    for (int i = 0; i < 16; i++)
        F.last_cdb[i] = c[i];
    F.cmds++;
    if (c[0] == 0x28 || c[0] == 0x88 || c[0] == 0x2A || c[0] == 0x8A)
        for (int i = 0; i < 16; i++)
            F.last_rw[i] = c[i];
    F.status = 0;
    F.data_len = 0;
    F.want_out = 0;
    memset(F.data, 0, 64);
    unsigned xfer = F.cbw[8] | F.cbw[9] << 8 | F.cbw[10] << 16 | (unsigned)F.cbw[11] << 24;
    switch (c[0])
    {
    case 0x12: /* INQUIRY */
        F.data[0] = 0;
        memcpy(F.data + 8, "HARIS   ", 8);
        memcpy(F.data + 16, "TEST STICK      ", 16);
        F.data_len = 36;
        break;
    case 0x00: /* TEST UNIT READY */
        if (F.tur_fail)
        {
            F.tur_fail--;
            F.status = 1;
            sense_set(6, 0x28);
        }
        break;
    case 0x03: /* REQUEST SENSE */
        memcpy(F.data, F.sense, 18);
        F.data_len = 18;
        sense_set(0, 0);
        break;
    case 0x25: /* READ CAPACITY(10) */
        if (F.cap_blocks - 1 > 0xFFFFFFFEull)
            wbe32(F.data, 0xFFFFFFFFu);
        else
            wbe32(F.data, (unsigned)(F.cap_blocks - 1));
        wbe32(F.data + 4, SS);
        F.data_len = 8;
        break;
    case 0x9E: /* READ CAPACITY(16) */
        wbe32(F.data, (unsigned)((F.cap_blocks - 1) >> 32));
        wbe32(F.data + 4, (unsigned)(F.cap_blocks - 1));
        wbe32(F.data + 8, SS);
        F.data_len = 32;
        break;
    case 0x1A: /* MODE SENSE(6) */
        F.data[0] = 3;
        F.data[2] = F.write_protect ? 0x80 : 0;
        F.data_len = 4;
        break;
    case 0x28: /* READ(10) */
    case 0x88:
    {
        unsigned long long lba;
        unsigned n;
        if (c[0] == 0x28)
        {
            lba = be32(c + 2);
            n = c[7] << 8 | c[8];
        }
        else
        {
            lba = (unsigned long long)be32(c + 2) << 32 | be32(c + 6);
            n = be32(c + 10);
        }
        CHECK(n * SS == xfer);
        if (lba + n <= NSEC)
        {
            memcpy(F.data, DISK + lba * SS, (size_t)n * SS);
            F.data_len = n * SS;
        }
        else
        {
            F.status = 1;
            sense_set(5, 0x21);
        }
        break;
    }
    case 0x2A: /* WRITE(10) */
    case 0x8A:
    {
        unsigned long long lba;
        unsigned n;
        if (c[0] == 0x2A)
        {
            lba = be32(c + 2);
            n = c[7] << 8 | c[8];
        }
        else
        {
            lba = (unsigned long long)be32(c + 2) << 32 | be32(c + 6);
            n = be32(c + 10);
        }
        CHECK(n * SS == xfer);
        F.want_out = (int)xfer;
        F.out_dst = lba + n <= NSEC ? DISK + lba * SS : NULL;
        break;
    }
    case 0x35: /* SYNCHRONIZE CACHE */
        if (F.sync_unsupported)
        {
            F.status = 1;
            sense_set(5, 0x20);
        }
        break;
    default:
        F.status = 1;
        sense_set(5, 0x20);
    }
}

static int f_out(void *ctx, const void *buf, unsigned len)
{
    (void)ctx;
    if (F.fail_all || (F.dead_after >= 0 && F.cmds >= F.dead_after))
        return MSC_ERR;
    if (F.want_out)
    {
        CHECK((int)len == F.want_out);
        if (F.out_dst)
            memcpy(F.out_dst, buf, len);
        F.want_out = 0;
        return MSC_OK;
    }
    CHECK(len == 31);
    memcpy(F.cbw, buf, 31);
    CHECK(memcmp(F.cbw, "USBC", 4) == 0);
    CHECK(F.cbw[13] == 0);
    F.have_cbw = 1;
    run_cbw();
    return MSC_OK;
}

static int f_in(void *ctx, void *buf, unsigned len, unsigned *got)
{
    (void)ctx;
    if (F.fail_all || (F.dead_after >= 0 && F.cmds >= F.dead_after))
        return MSC_ERR;
    if (F.data_len)
    {
        if (F.stall_data_once && F.cbw[15] == 0x28)
        {
            F.stall_data_once = 0;
            F.data_len = 0;
            F.status = 1;
            sense_set(3, 0x11);
            return MSC_STALL;
        }
        CHECK((F.cbw[12] & 0x80) != 0);
        unsigned n = len < F.data_len ? len : F.data_len;
        memcpy(buf, F.data, n);
        *got = n;
        F.data_len = 0;
        return MSC_OK;
    }
    if (len != 13) /* a data stage the drive has nothing for (it failed the command): it stalls */
        return MSC_STALL;
    unsigned char *c = buf;
    memset(c, 0, 13);
    memcpy(c, "USBS", 4);
    memcpy(c + 4, F.cbw + 4, 4);
    if (F.bad_tag)
        c[4] ^= 0x55;
    c[12] = (unsigned char)F.status;
    *got = 13;
    F.have_cbw = 0;
    return MSC_OK;
}

static int f_clear(void *ctx, int in) { (void)ctx; (void)in; F.clears++; return 0; }
static int f_reset(void *ctx) { (void)ctx; F.resets++; return 0; }
static void f_delay(void *ctx, unsigned ms) { (void)ctx; (void)ms; }
static int f_alive(void *ctx) { (void)ctx; return F.alive; }

static UsbMscIo make_io(void)
{
    UsbMscIo io;
    memset(&io, 0, sizeof io);
    io.bulk_out = f_out;
    io.bulk_in = f_in;
    io.clear_halt = f_clear;
    io.reset = f_reset;
    io.delay_ms = f_delay;
    io.alive = f_alive;
    io.max_xfer = 16384; /* small on purpose: bigger requests must be split */
    return io;
}

static void fresh(void)
{
    memset(&F, 0, sizeof F);
    F.alive = 1;
    F.cap_blocks = NSEC;
    F.dead_after = -1;
    sense_set(0, 0);
    for (unsigned i = 0; i < sizeof DISK; i++)
        DISK[i] = (unsigned char)(i * 7 + (i >> 9));
    blk_reset();
}

int main(void)
{
    static UsbMsc m;
    UsbMscIo io = make_io();

    /* attach: INQUIRY, ready, capacity, block device */
    fresh();
    F.tur_fail = 2;
    int idx = usb_msc_attach(&m, &io);
    CHECK(idx == 0);
    const BlkDev *d = blk_get(0);
    CHECK(d && d->kind == BLK_USB && strcmp(d->name, "usb0") == 0);
    CHECK(d && strcmp(d->model, "HARIS TEST STICK") == 0);
    CHECK(d && d->sector_size == SS && d->sectors == NSEC && d->writable);
    CHECK(blk_generation() > 0);

    /* read: bigger than max_xfer (16 KiB = 32 sectors), so several commands */
    static unsigned char buf[64 * SS];
    CHECK(blk_read(0, 5, 64, buf) == 0);
    CHECK(memcmp(buf, DISK + 5 * SS, sizeof buf) == 0);
    CHECK(F.cbw[15] == 0x28);
    CHECK(blk_read(0, NSEC - 1, 1, buf) == 0 && memcmp(buf, DISK + (NSEC - 1) * SS, SS) == 0);
    CHECK(blk_read(0, NSEC, 1, buf) != 0); /* outside the device: blk.c refuses */

    /* write is locked by default */
    memset(buf, 0xA5, sizeof buf);
    CHECK(blk_write(0, 100, 40, buf) != 0);
    blk_set_locked(0, 0);
    CHECK(blk_write(0, 100, 40, buf) == 0);
    CHECK(memcmp(DISK + 100 * SS, buf, 40 * SS) == 0);
    CHECK(DISK[140 * SS] != 0xA5 || DISK[140 * SS + 1] != 0xA5); /* nothing written past the end */
    CHECK(blk_flush(0) == 0);
    CHECK(F.cbw[15] == 0x35);
    F.sync_unsupported = 1;
    CHECK(blk_flush(0) == 0); /* "not supported" is not an error */

    /* a stalled data stage: the status still arrives, the command fails with a medium error */
    F.stall_data_once = 1;
    int clears = F.clears;
    CHECK(blk_read(0, 0, 1, buf) != 0);
    CHECK(F.clears == clears + 1);
    CHECK(blk_read(0, 0, 1, buf) == 0); /* and the next one works */

    /* wrong tag in the status: reset recovery, command fails */
    F.bad_tag = 1;
    int resets = F.resets;
    CHECK(blk_read(0, 0, 1, buf) != 0);
    CHECK(F.resets > resets);
    F.bad_tag = 0;
    CHECK(blk_read(0, 0, 1, buf) == 0);

    /* unplugged: no more commands are sent */
    F.alive = 0;
    int cmds = F.cmds;
    CHECK(blk_read(0, 0, 1, buf) != 0);
    CHECK(F.cmds == cmds);
    usb_msc_detach(&m);
    CHECK(blk_get(0)->removed == 1);
    CHECK(blk_read(0, 0, 1, buf) != 0);
    CHECK(blk_write(0, 0, 1, buf) != 0);

    /* write protect switch */
    fresh();
    F.write_protect = 1;
    CHECK(usb_msc_attach(&m, &io) == 0);
    CHECK(!blk_get(0)->writable);
    blk_set_locked(0, 0);
    CHECK(blk_write(0, 0, 1, buf) != 0);

    /* more than 2 TiB: READ CAPACITY(16), and READ(16)/WRITE(16) beyond 32 bits */
    fresh();
    F.cap_blocks = 0x300000000ull; /* 12 TiB with 512-byte sectors */
    CHECK(usb_msc_attach(&m, &io) == 0);
    CHECK(blk_get(0)->sectors == 0x300000000ull);
    CHECK(F.cmds > 0);
    /* the fake only has NSEC sectors of data: a read there makes the fake answer with an error, but the CDB shows the form */
    blk_read(0, 0x200000000ull, 1, buf);
    CHECK(F.last_rw[0] == 0x88);
    CHECK(F.last_rw[5] == 2 && F.last_rw[9] == 0); /* LBA 0x2_0000_0000 as 8 big-endian bytes */

    /* not a disk / empty reader / dead device */
    fresh();
    F.fail_all = 1;
    CHECK(usb_msc_attach(&m, &io) < 0);
    CHECK(blk_count() == 0);

    fresh();
    F.tur_fail = 100;
    CHECK(usb_msc_attach(&m, &io) < 0); /* never gets ready */
    CHECK(blk_count() == 0);

    /* slot reuse after unplug + reap */
    fresh();
    CHECK(usb_msc_attach(&m, &io) == 0);
    usb_msc_detach(&m);
    blk_reap();
    CHECK(blk_count() == 0);
    CHECK(usb_msc_attach(&m, &io) == 0);
    CHECK(strcmp(blk_get(0)->name, "usb0") == 0);

    if (fails)
    {
        printf("%d check(s) failed\n", fails);
        return 1;
    }
    printf("usb_msc: all checks passed\n");
    return 0;
}
