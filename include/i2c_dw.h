/* ============================================================
 * Synopsys DesignWare I2C controller, master mode, polled. This is the I2C controller in
 * AMD (AMDI0010) and Intel (LPSS) chipsets, where laptop touchpads are connected.
 *
 *   dw_i2c_probe()   is there a DesignWare controller at this address?
 *   dw_i2c_setup()   target a 7-bit address, set the bus speed, switch the controller on
 *   dw_i2c_xfer()    write some bytes, then (optionally, with a repeated start) read some
 * ============================================================ */
#ifndef HARIS_I2C_DW_H
#define HARIS_I2C_DW_H

typedef struct
{
    volatile unsigned char *base;
    unsigned tx_depth, rx_depth; /* FIFO sizes in bytes */
} DwI2c;

/* Returns 0 if a controller answers at `base` (physical address, identity mapped). */
int dw_i2c_probe(DwI2c *c, unsigned long long base);

/* clk_hz: the controller's input clock; bus_hz: wanted SCL speed (100 kHz or up to 400 kHz). */
int dw_i2c_setup(DwI2c *c, unsigned addr, unsigned clk_hz, unsigned bus_hz);

/* Sends w[0..wn), then reads r[0..rn). Either count may be 0 (not both). Returns 0 on success,
 * -1 when the device did not answer (NACK) or the transfer timed out. */
int dw_i2c_xfer(DwI2c *c, const unsigned char *w, unsigned wn, unsigned char *r, unsigned rn);

#endif
