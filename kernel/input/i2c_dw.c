/* DesignWare I2C master. See include/i2c_dw.h. */
#include "i2c_dw.h"
#include "io.h"

#if defined(__x86_64__)

#define IC_CON 0x00
#define IC_TAR 0x04
#define IC_DATA_CMD 0x10
#define IC_SS_HCNT 0x14
#define IC_SS_LCNT 0x18
#define IC_FS_HCNT 0x1C
#define IC_FS_LCNT 0x20
#define IC_INTR_MASK 0x30
#define IC_RAW_INTR_STAT 0x34
#define IC_RX_TL 0x38
#define IC_TX_TL 0x3C
#define IC_CLR_INTR 0x40
#define IC_CLR_TX_ABRT 0x54
#define IC_ENABLE 0x6C
#define IC_STATUS 0x70
#define IC_TXFLR 0x74
#define IC_RXFLR 0x78
#define IC_ENABLE_STATUS 0x9C
#define IC_COMP_PARAM_1 0xF4
#define IC_COMP_TYPE 0xFC

#define COMP_TYPE_DW 0x44570140u /* "DW" 0x0140 */

#define CMD_READ 0x100u
#define CMD_STOP 0x200u
#define CMD_RESTART 0x400u

#define INTR_TX_ABRT (1u << 6)
#define ST_ACTIVITY 0x01u
#define ST_TFE 0x04u /* transmit FIFO empty */

static void udelay(unsigned us) /* a read of port 0x80 takes about 1 us */
{
    for (unsigned i = 0; i < us; i++)
        io_in8(0x80);
}

static unsigned rd(DwI2c *c, unsigned off) { return *(volatile unsigned *)(c->base + off); }
static void wr(DwI2c *c, unsigned off, unsigned v) { *(volatile unsigned *)(c->base + off) = v; }

static int set_enabled(DwI2c *c, int on)
{
    for (int tries = 0; tries < 100; tries++)
    {
        wr(c, IC_ENABLE, on ? 1u : 0u);
        for (int i = 0; i < 100; i++)
        {
            if ((rd(c, IC_ENABLE_STATUS) & 1u) == (on ? 1u : 0u))
                return 0;
            udelay(10);
        }
    }
    return -1;
}

int dw_i2c_probe(DwI2c *c, unsigned long long base)
{
    c->base = (volatile unsigned char *)base;
    if (rd(c, IC_COMP_TYPE) != COMP_TYPE_DW)
        return -1;
    unsigned param = rd(c, IC_COMP_PARAM_1);
    c->rx_depth = ((param >> 8) & 0xFF) + 1;
    c->tx_depth = ((param >> 16) & 0xFF) + 1;
    return 0;
}

int dw_i2c_setup(DwI2c *c, unsigned addr, unsigned clk_hz, unsigned bus_hz)
{
    if (set_enabled(c, 0))
        return -1;

    /* SCL high and low times in input-clock cycles. The controller adds a few cycles of its own,
     * so take them off; the split gives the low phase a bit more time than the high one. */
    unsigned period = clk_hz / bus_hz;
    unsigned hcnt = period * 2 / 5 - 8, lcnt = period * 3 / 5 - 1;
    int fast = bus_hz > 100000;
    wr(c, fast ? IC_FS_HCNT : IC_SS_HCNT, hcnt);
    wr(c, fast ? IC_FS_LCNT : IC_SS_LCNT, lcnt);

    /* master, 7-bit addresses, speed, restart allowed, slave side off */
    wr(c, IC_CON, 1u | ((fast ? 2u : 1u) << 1) | (1u << 5) | (1u << 6));
    wr(c, IC_TAR, addr & 0x7F);
    wr(c, IC_INTR_MASK, 0); /* we poll the raw status */
    wr(c, IC_RX_TL, 0);
    wr(c, IC_TX_TL, 0);
    rd(c, IC_CLR_INTR);
    return set_enabled(c, 1);
}

int dw_i2c_xfer(DwI2c *c, const unsigned char *w, unsigned wn, unsigned char *r, unsigned rn)
{
    unsigned total = wn + rn, sent = 0, got = 0;
    if (total == 0)
        return -1;
    rd(c, IC_CLR_TX_ABRT);

    unsigned idle = 0;
    while (got < rn || sent < total)
    {
        int progress = 0;

        /* queue commands: never more reads in flight than the receive FIFO can hold */
        while (sent < total && rd(c, IC_TXFLR) < c->tx_depth)
        {
            unsigned cmd;
            if (sent < wn)
                cmd = w[sent];
            else
            {
                if (sent - wn - got >= c->rx_depth)
                    break;
                cmd = CMD_READ;
                if (sent == wn && wn)
                    cmd |= CMD_RESTART; /* repeated start between the write and the read */
            }
            if (sent == total - 1)
                cmd |= CMD_STOP;
            wr(c, IC_DATA_CMD, cmd);
            sent++;
            progress = 1;
        }

        while (got < rn && rd(c, IC_RXFLR) > 0)
        {
            r[got++] = (unsigned char)rd(c, IC_DATA_CMD);
            progress = 1;
        }

        if (rd(c, IC_RAW_INTR_STAT) & INTR_TX_ABRT) /* NACK: nobody there, or it did not like the byte */
        {
            rd(c, IC_CLR_TX_ABRT);
            return -1;
        }
        if (progress)
            idle = 0;
        else if (++idle > 200000) /* about 100 ms without anything happening */
            return -1;
        else
            udelay(1);
    }

    /* the last byte has to leave the FIFO and the STOP must be on the bus before the next transfer */
    for (unsigned i = 0; i < 20000; i++)
    {
        if ((rd(c, IC_STATUS) & ST_TFE) && !(rd(c, IC_STATUS) & ST_ACTIVITY))
            break;
        if (rd(c, IC_RAW_INTR_STAT) & INTR_TX_ABRT)
        {
            rd(c, IC_CLR_TX_ABRT);
            return -1;
        }
        udelay(1);
    }
    return 0;
}

#else

int dw_i2c_probe(DwI2c *c, unsigned long long base)
{
    (void)c; (void)base;
    return -1;
}
int dw_i2c_setup(DwI2c *c, unsigned addr, unsigned clk_hz, unsigned bus_hz)
{
    (void)c; (void)addr; (void)clk_hz; (void)bus_hz;
    return -1;
}
int dw_i2c_xfer(DwI2c *c, const unsigned char *w, unsigned wn, unsigned char *r, unsigned rn)
{
    (void)c; (void)w; (void)wn; (void)r; (void)rn;
    return -1;
}

#endif
