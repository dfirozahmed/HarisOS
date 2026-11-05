/* Host test for the pure parts of sdhci.c: the clock divisor and the card size from the CSD. The register-level start-up was proven
 * against QEMU's sdhci-pci (card found, single and multi block read / write). The CSD words below are real CSD layouts built
 * field by field, so a shifted bit position shows up here. */
#include <stdio.h>
#include <string.h>
#include "../kernel/drivers/storage/sdhci.c"

int io_dummy;
int blk_register(const BlkDev *d) { (void)d; return 0; }
int pci_count(void) { return 0; }
const PciDevice *pci_get(int i) { (void)i; return 0; }
unsigned int pci_cfg_read(unsigned b, unsigned d, unsigned f, unsigned o) { (void)b; (void)d; (void)f; (void)o; return 0; }
void pci_cfg_write(unsigned b, unsigned d, unsigned f, unsigned o, unsigned v) { (void)b; (void)d; (void)f; (void)o; (void)v; }
unsigned long long pmm_alloc_contiguous_tag(unsigned long long c, PmmTag t) { (void)c; (void)t; return 0; }
void pmm_free_contiguous(unsigned long long p, unsigned long long c) { (void)p; (void)c; }

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* Build a response from CSD fields: a CSD bit p lands at response bit p - 8. */
static void put(u32 r[4], int csd_hi, int csd_lo, u64 v)
{
    for (int b = csd_lo; b <= csd_hi; b++)
        if ((v >> (b - csd_lo)) & 1)
            r[(b - 8) / 32] |= 1u << ((b - 8) % 32);
}

int main(void)
{
    /* clock: spec 3.0, 10 bit divider N, SDCLK = base / (2N) */
    CHECK(clock_ctrl(100000, 400, 2) == (u16)((125u & 0xFF) << 8 | 1));      /* N = ceil(100000 / 800) = 125 */
    CHECK(clock_ctrl(100000, 25000, 2) == (u16)((2u << 8) | 1));            /* N = 2 -> 25 MHz */
    CHECK(clock_ctrl(25000, 25000, 2) == 1);                                 /* base already slow enough: undivided */
    CHECK(clock_ctrl(200000, 400, 2) == (u16)(((250u & 0xFF) << 8) | 1));    /* N = 250 */
    CHECK(clock_ctrl(1000000, 100, 2) == (u16)(((1023u & 0xFF) << 8) | (3u << 6) | 1)); /* clamped to 1023 */
    /* spec 1.0 / 2.0: power of two divisor, register holds D / 2 */
    CHECK(clock_ctrl(50000, 25000, 1) == (u16)((1u << 8) | 1));              /* D = 2 */
    CHECK(clock_ctrl(50000, 400, 1) == (u16)((64u << 8) | 1));               /* D = 128 (50000/128 = 390 kHz) */
    CHECK(clock_ctrl(20000, 25000, 1) == 1);                                 /* D = 1 */

    /* CSD 2.0 (SDHC): C_SIZE 0x1D1F -> (0x1D1F + 1) * 512 KiB = 3.7 GB */
    u32 r[4] = {0, 0, 0, 0};
    put(r, 127, 126, 1);
    put(r, 69, 48, 0x1D1F);
    CHECK(csd_sectors(r) == (u64)(0x1D1F + 1) * 1024);
    /* a 16 MiB card as QEMU makes it (CSD 2.0, C_SIZE 31 = 32 x 512 KiB): 32768 sectors */
    memset(r, 0, sizeof r);
    put(r, 127, 126, 1);
    put(r, 69, 48, 31);
    CHECK(csd_sectors(r) == 32768);
    /* CSD 1.0 (SDSC): READ_BL_LEN 10 (1024 byte blocks), C_SIZE 2047, C_SIZE_MULT 7 -> 2048 << 9 = 1048576 blocks of 1 KiB = 1 GiB */
    memset(r, 0, sizeof r);
    put(r, 83, 80, 10);
    put(r, 73, 62, 2047);
    put(r, 49, 47, 7);
    CHECK(csd_sectors(r) == 2u * 1024 * 1024);
    /* unknown structure version, or a nonsense block length: no size */
    memset(r, 0, sizeof r);
    put(r, 127, 126, 2);
    CHECK(csd_sectors(r) == 0);
    memset(r, 0, sizeof r);
    put(r, 83, 80, 15);
    CHECK(csd_sectors(r) == 0);

    if (fails) { printf("%d check(s) failed\n", fails); return 1; }
    printf("sdhci tests passed\n");
    return 0;
}
