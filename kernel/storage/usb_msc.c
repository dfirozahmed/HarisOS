/* USB mass storage: Bulk-Only Transport + SCSI. See include/usb_msc.h.
 *
 * Every SCSI command travels as three steps on the two bulk endpoints:
 *   1. CBW (Command Block Wrapper, 31 bytes, OUT): "USBC", a tag, how many data bytes follow and in which
 *      direction, then the SCSI command (CDB, up to 16 bytes).
 *   2. the data, if any (IN for READ, OUT for WRITE).
 *   3. CSW (Command Status Wrapper, 13 bytes, IN): "USBS", the same tag, how many bytes were NOT moved,
 *      and a status: 0 good, 1 command failed (ask REQUEST SENSE why), 2 phase error (reset the device).
 * A STALL on an endpoint is cleared with CLEAR_FEATURE(ENDPOINT_HALT) and the transfer carries on with the next
 * step; anything that breaks the order of the three steps is repaired with a Bulk-Only reset. */
#include "blk.h"
#include "usb_msc.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;


#define SCSI_TEST_UNIT_READY 0x00
#define SCSI_REQUEST_SENSE 0x03
#define SCSI_INQUIRY 0x12
#define SCSI_MODE_SENSE6 0x1A
#define SCSI_READ_CAPACITY10 0x25
#define SCSI_READ10 0x28
#define SCSI_WRITE10 0x2A
#define SCSI_SYNC_CACHE10 0x35
#define SCSI_READ16 0x88
#define SCSI_WRITE16 0x8A
#define SCSI_SERVICE_ACTION_IN16 0x9E /* READ CAPACITY(16) is service action 0x10 */

#define SENSE_NOT_READY 2
#define SENSE_ILLEGAL_REQUEST 5
#define SENSE_UNIT_ATTENTION 6
#define ASC_NO_MEDIUM 0x3A

#define CMD_OK 0
#define CMD_FAILED 1 /* the drive said "check condition" */
#define CMD_ERR (-1) /* transport problem (already repaired with a reset, but this command is lost) */

static void put_be32(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
}

static u32 get_be32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static u64 get_be64(const u8 *p) { return (u64)get_be32(p) << 32 | get_be32(p + 4); }

static void zero(void *p, unsigned n)
{
    volatile u8 *b = (volatile u8 *)p;
    for (unsigned i = 0; i < n; i++)
        b[i] = 0;
}

static void wait_ms(UsbMsc *m, unsigned ms)
{
    if (m->io.delay_ms)
        m->io.delay_ms(m->io.ctx, ms);
}

static int alive(UsbMsc *m) { return !m->io.alive || m->io.alive(m->io.ctx); }

/* Reset recovery: Bulk-Only reset + clear both halts. */
static void recover(UsbMsc *m)
{
    if (m->io.reset)
        m->io.reset(m->io.ctx);
}

/* One SCSI command. `data` / `len`: the data stage (len <= io.max_xfer), `in` = direction device -> host.
 * *moved = bytes that really went over the wire in the data stage. Returns CMD_*. */
static int bot_cmd(UsbMsc *m, const u8 *cdb, unsigned cdb_len, void *data, unsigned len, int in, unsigned *moved)
{
    u8 cbw[31];
    zero(cbw, sizeof cbw);
    u32 tag = ++m->tag;
    cbw[0] = 'U';
    cbw[1] = 'S';
    cbw[2] = 'B';
    cbw[3] = 'C';
    cbw[4] = (u8)tag;
    cbw[5] = (u8)(tag >> 8);
    cbw[6] = (u8)(tag >> 16);
    cbw[7] = (u8)(tag >> 24);
    cbw[8] = (u8)len; /* data transfer length, little endian (the only little-endian field of the SCSI parts) */
    cbw[9] = (u8)(len >> 8);
    cbw[10] = (u8)(len >> 16);
    cbw[11] = (u8)(len >> 24);
    cbw[12] = (len && in) ? 0x80 : 0; /* direction */
    cbw[13] = 0;                      /* LUN 0 */
    cbw[14] = (u8)cdb_len;
    for (unsigned i = 0; i < cdb_len && i < 16; i++)
        cbw[15 + i] = cdb[i];

    if (moved)
        *moved = 0;
    if (!alive(m))
        return CMD_ERR;
    int r = m->io.bulk_out(m->io.ctx, cbw, sizeof cbw);
    if (r != MSC_OK)
    {
        recover(m); /* a stalled or failed CBW: the device state is unknown */
        return CMD_ERR;
    }

    if (len)
    {
        unsigned got = 0;
        if (in)
            r = m->io.bulk_in(m->io.ctx, data, len, &got);
        else
        {
            r = m->io.bulk_out(m->io.ctx, data, len);
            got = len;
        }
        if (moved)
            *moved = got;
        if (r == MSC_STALL)
            m->io.clear_halt(m->io.ctx, in); /* the drive ended the data stage early: go on with the status */
        else if (r != MSC_OK)
        {
            recover(m);
            return CMD_ERR;
        }
    }

    u8 csw[13];
    unsigned got = 0;
    r = m->io.bulk_in(m->io.ctx, csw, sizeof csw, &got);
    if (r == MSC_STALL)
    {
        m->io.clear_halt(m->io.ctx, 1);
        r = m->io.bulk_in(m->io.ctx, csw, sizeof csw, &got); /* the status is sent again after the clear */
    }
    u32 csw_tag = (u32)csw[4] | (u32)csw[5] << 8 | (u32)csw[6] << 16 | (u32)csw[7] << 24;
    if (r != MSC_OK || got != sizeof csw || get_be32(csw) != 0x55534253u /* "USBS" */ || csw_tag != tag ||
        csw[12] == 2) /* 2 = phase error */
    {
        recover(m);
        return CMD_ERR;
    }
    if (!in && len && moved)
    {
        u32 residue = (u32)csw[8] | (u32)csw[9] << 8 | (u32)csw[10] << 16 | (u32)csw[11] << 24;
        *moved = residue < len ? len - residue : 0; /* what the drive did not take is in the residue */
    }
    return csw[12] != 0 ? CMD_FAILED : CMD_OK;
}

/* REQUEST SENSE: why did the last command fail? Returns the sense key (0 if unknown) and the additional sense code. */
static int sense(UsbMsc *m, int *asc)
{
    static const u8 cdb[6] = {SCSI_REQUEST_SENSE, 0, 0, 0, 18, 0};
    u8 buf[18];
    unsigned got = 0;
    zero(buf, sizeof buf);
    if (bot_cmd(m, cdb, sizeof cdb, buf, sizeof buf, 1, &got) != CMD_OK || got < 14)
    {
        *asc = 0;
        return 0;
    }
    *asc = buf[12];
    return buf[2] & 0x0F;
}

/* Runs a command that may need a second try: the drive reports a "unit attention" (it was reset or the medium
 * changed) once and works afterwards, or it says "not ready" for a moment. */
static int cmd_retry(UsbMsc *m, const u8 *cdb, unsigned cdb_len, void *data, unsigned len, int in, unsigned *moved)
{
    int r = CMD_ERR;
    for (int attempt = 0; attempt < 3; attempt++)
    {
        r = bot_cmd(m, cdb, cdb_len, data, len, in, moved);
        if (r == CMD_OK || !alive(m))
            return r;
        if (r == CMD_FAILED)
        {
            int asc;
            int key = sense(m, &asc);
            if (key != SENSE_UNIT_ATTENTION && key != SENSE_NOT_READY)
                return r;
        }
        wait_ms(m, 50);
    }
    return r;
}

/* ---------- reading and writing sectors ---------- */

/* up to io.max_xfer bytes: `count` sectors starting at `lba`, to / from `buf` */
static int rw_chunk(UsbMsc *m, int write, u64 lba, unsigned count, void *buf)
{
    u8 cdb[16];
    zero(cdb, sizeof cdb);
    unsigned cdb_len;
    if (lba + count <= 0xFFFFFFFFull && count <= 0xFFFF)
    {
        cdb[0] = write ? SCSI_WRITE10 : SCSI_READ10;
        put_be32(cdb + 2, (u32)lba);
        cdb[7] = (u8)(count >> 8);
        cdb[8] = (u8)count;
        cdb_len = 10;
    }
    else
    {
        cdb[0] = write ? SCSI_WRITE16 : SCSI_READ16;
        put_be32(cdb + 2, (u32)(lba >> 32));
        put_be32(cdb + 6, (u32)lba);
        put_be32(cdb + 10, count);
        cdb_len = 16;
    }
    unsigned len = count * m->sector_size, moved = 0;
    int r = cmd_retry(m, cdb, cdb_len, buf, len, !write, &moved);
    return (r == CMD_OK && moved == len) ? 0 : -1;
}

static int msc_rw(BlkDev *d, int write, u64 lba, unsigned count, void *buf)
{
    UsbMsc *m = (UsbMsc *)d->ctx;
    unsigned per = m->io.max_xfer / m->sector_size;
    u8 *p = (u8 *)buf;
    if (per == 0)
        return -1;
    while (count)
    {
        unsigned n = count < per ? count : per;
        if (rw_chunk(m, write, lba, n, p) != 0)
            return -1;
        lba += n;
        p += (u64)n * m->sector_size;
        count -= n;
    }
    return 0;
}

static int msc_read(BlkDev *d, unsigned long long lba, unsigned count, void *buf)
{
    return msc_rw(d, 0, lba, count, buf);
}

static int msc_write(BlkDev *d, unsigned long long lba, unsigned count, const void *buf)
{
    return msc_rw(d, 1, lba, count, (void *)buf);
}

/* SYNCHRONIZE CACHE: the drive's own write cache goes to the flash before anyone pulls the stick out. */
static int msc_flush(BlkDev *d)
{
    UsbMsc *m = (UsbMsc *)d->ctx;
    static const u8 cdb[10] = {SCSI_SYNC_CACHE10, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    int r = bot_cmd(m, cdb, sizeof cdb, 0, 0, 0, 0);
    if (r == CMD_FAILED)
    {
        int asc;
        return sense(m, &asc) == SENSE_ILLEGAL_REQUEST ? 0 : -1; /* "not supported" = nothing to flush */
    }
    return r == CMD_OK ? 0 : -1;
}

/* ---------- start-up ---------- */

static void trim_copy(char *dst, const u8 *src, int n)
{
    int len = 0;
    for (int i = 0; i < n; i++)
        dst[i] = (src[i] >= 0x20 && src[i] < 0x7F) ? (char)src[i] : ' ';
    for (int i = 0; i < n; i++)
        if (dst[i] != ' ')
            len = i + 1;
    dst[len] = 0;
}

static int read_capacity(UsbMsc *m)
{
    u8 cdb[16], buf[32];
    unsigned got = 0;
    zero(cdb, sizeof cdb);
    zero(buf, sizeof buf);
    cdb[0] = SCSI_READ_CAPACITY10;
    if (cmd_retry(m, cdb, 10, buf, 8, 1, &got) != CMD_OK || got < 8)
        return -1;
    u64 last = get_be32(buf);
    u32 block = get_be32(buf + 4);
    if (last == 0xFFFFFFFFull) /* more than 2 TiB (with 512-byte sectors): the 16-byte version tells the real size */
    {
        zero(cdb, sizeof cdb);
        zero(buf, sizeof buf);
        cdb[0] = SCSI_SERVICE_ACTION_IN16;
        cdb[1] = 0x10;
        put_be32(cdb + 10, 32);
        if (cmd_retry(m, cdb, 16, buf, 32, 1, &got) != CMD_OK || got < 12)
            return -1;
        last = get_be64(buf);
        block = get_be32(buf + 8);
    }
    if ((block != 512 && block != 1024 && block != 2048 && block != 4096) || block > BLK_SECTOR_MAX)
        return -1;
    m->sector_size = block;
    m->sectors = last + 1;
    return m->sectors ? 0 : -1;
}

int usb_msc_probe(UsbMsc *m, const UsbMscIo *io)
{
    m->io.bulk_out = io->bulk_out;
    m->io.bulk_in = io->bulk_in;
    m->io.clear_halt = io->clear_halt;
    m->io.reset = io->reset;
    m->io.delay_ms = io->delay_ms;
    m->io.alive = io->alive;
    m->io.max_xfer = io->max_xfer;
    m->io.ctx = io->ctx;
    m->tag = 0;
    m->sector_size = 0;
    m->sectors = 0;
    m->read_only = 0;
    m->blk = -1;
    m->vendor[0] = m->product[0] = 0;
    if (!io->bulk_out || !io->bulk_in || !io->clear_halt || io->max_xfer < 4096)
        return -1;

    u8 cdb[16], buf[36];
    unsigned got = 0;

    /* INQUIRY: what kind of device is this? (Direct-access disk = 0, "reduced block commands" disk = 0x0E.) */
    zero(cdb, sizeof cdb);
    zero(buf, sizeof buf);
    cdb[0] = SCSI_INQUIRY;
    cdb[4] = sizeof buf;
    if (cmd_retry(m, cdb, 6, buf, sizeof buf, 1, &got) != CMD_OK || got < 36)
        return -1;
    int type = buf[0] & 0x1F, qualifier = buf[0] >> 5;
    if (qualifier != 0 || (type != 0x00 && type != 0x0E))
        return -1;
    trim_copy(m->vendor, buf + 8, 8);
    trim_copy(m->product, buf + 16, 16);

    /* TEST UNIT READY until the drive has spun up / read its flash. "No medium" (an empty card reader) is not
     * going to change in a few seconds, so that gives up sooner. */
    int ready = 0, no_medium = 0;
    zero(cdb, sizeof cdb);
    for (int i = 0; i < 25 && !ready; i++)
    {
        int r = bot_cmd(m, cdb, 6, 0, 0, 0, 0);
        if (r == CMD_OK)
            ready = 1;
        else if (!alive(m))
            return -1;
        else
        {
            if (r == CMD_FAILED)
            {
                int asc, key = sense(m, &asc);
                if (key == SENSE_NOT_READY && asc == ASC_NO_MEDIUM && ++no_medium >= 3)
                    return -1;
            }
            wait_ms(m, 200);
        }
    }
    if (!ready || read_capacity(m) != 0)
        return -1;

    /* MODE SENSE(6): bit 7 of the device-specific parameter (byte 2) is the write-protect switch. A drive that
     * does not answer is taken to be writable. */
    zero(cdb, sizeof cdb);
    zero(buf, sizeof buf);
    cdb[0] = SCSI_MODE_SENSE6;
    cdb[2] = 0x3F;
    cdb[4] = 4;
    if (bot_cmd(m, cdb, 6, buf, 4, 1, &got) == CMD_OK && got >= 3)
        m->read_only = (buf[2] & 0x80) != 0;
    return 0;
}

/* "usb0", "usb1" ...: the lowest number not used by a drive that is still there */
static int free_number(void)
{
    for (int n = 0; n < 10; n++)
    {
        int used = 0;
        for (int i = 0; i < blk_count(); i++)
        {
            const BlkDev *b = blk_get(i);
            if (b->removed == 2 || b->kind != BLK_USB)
                continue;
            if (b->name[3] == (char)('0' + n) && b->name[4] == 0)
                used = 1;
        }
        if (!used)
            return n;
    }
    return 9;
}

int usb_msc_attach(UsbMsc *m, const UsbMscIo *io)
{
    if (usb_msc_probe(m, io) != 0)
        return -1;
    BlkDev bd;
    zero(&bd, sizeof bd);
    bd.name[0] = 'u';
    bd.name[1] = 's';
    bd.name[2] = 'b';
    bd.name[3] = (char)('0' + free_number());
    bd.name[4] = 0;
    int n = 0;
    for (const char *s = m->vendor; *s && n < (int)sizeof bd.model - 2; s++)
        bd.model[n++] = *s;
    if (m->vendor[0] && m->product[0])
        bd.model[n++] = ' ';
    for (const char *s = m->product; *s && n < (int)sizeof bd.model - 1; s++)
        bd.model[n++] = *s;
    bd.model[n] = 0;
    bd.kind = BLK_USB;
    bd.media = BLK_MEDIA_UNKNOWN;
    bd.parent = -1;
    bd.sector_size = m->sector_size;
    bd.sectors = m->sectors;
    bd.writable = !m->read_only;
    bd.read = msc_read;
    bd.write = msc_write;
    bd.flush = msc_flush;
    bd.ctx = m;
    int idx = blk_register(&bd);
    if (idx < 0)
        return -1;
    m->blk = idx;
    part_scan(idx);
    return idx;
}

void usb_msc_detach(UsbMsc *m)
{
    if (m->blk >= 0)
        blk_remove(m->blk);
    m->blk = -1;
}
