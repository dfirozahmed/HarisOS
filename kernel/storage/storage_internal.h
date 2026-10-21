/* Helpers shared by the storage drivers (ahci.c, nvme.c). Freestanding: no memcpy / memset exist, and a
 * plain copy or fill loop may be turned by the compiler into a call to them, so these loops use volatile. */
#ifndef HARIS_STORAGE_INTERNAL_H
#define HARIS_STORAGE_INTERNAL_H

#include "io.h"
#include "pmm.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

static inline u32 mmio_rd32(volatile u8 *base, u32 off) { return *(volatile u32 *)(base + off); }
static inline void mmio_wr32(volatile u8 *base, u32 off, u32 v) { *(volatile u32 *)(base + off) = v; }
static inline u64 mmio_rd64(volatile u8 *base, u32 off)
{
    return (u64)mmio_rd32(base, off) | (u64)mmio_rd32(base, off + 4) << 32;
}
static inline void mmio_wr64(volatile u8 *base, u32 off, u64 v)
{
    mmio_wr32(base, off, (u32)v);
    mmio_wr32(base, off + 4, (u32)(v >> 32));
}
static inline void mem_fence(void) { __asm__ volatile("mfence" ::: "memory"); }

static inline void stor_zero(void *p, unsigned long n)
{
    volatile u8 *b = (volatile u8 *)p;
    for (unsigned long i = 0; i < n; i++)
        b[i] = 0;
}

static inline void stor_copy(void *dst, const void *src, unsigned long n)
{
    volatile u8 *d = (volatile u8 *)dst;
    const volatile u8 *s = (const volatile u8 *)src;
    for (unsigned long i = 0; i < n; i++)
        d[i] = s[i];
}

/* a read of port 0x80 takes about 1 us: a delay that needs no timer */
static inline void stor_udelay(unsigned us)
{
    for (unsigned i = 0; i < us; i++)
        io_in8(0x80);
}

/* Wait up to `ms` milliseconds for `cond` to become true (checked every 50 us). Evaluates to 1 if it did. */
#define STOR_WAIT_MS(cond, ms)                                      \
    ({                                                              \
        int _ok = 0;                                                \
        for (unsigned _i = 0; _i < (unsigned)(ms) * 20u; _i++)      \
        {                                                           \
            if (cond)                                               \
            {                                                       \
                _ok = 1;                                            \
                break;                                              \
            }                                                       \
            stor_udelay(50);                                        \
        }                                                           \
        _ok;                                                        \
    })

/* Zeroed, physically contiguous, page-aligned memory for DMA. `below_4g` = the device only has 32-bit
 * addresses. Returns the address (RAM is identity mapped), or 0. */
static inline u64 stor_dma_alloc(unsigned pages, int below_4g)
{
    u64 p = pmm_alloc_contiguous_tag(pages, PMM_TAG_STORAGE);
    if (!p)
        return 0;
    if (below_4g && p + (u64)pages * PMM_PAGE_SIZE > 0x100000000ULL)
    {
        pmm_free_contiguous(p, pages);
        return 0;
    }
    stor_zero((void *)p, (unsigned long)pages * PMM_PAGE_SIZE);
    return p;
}

/* Drivers: each registers the disks it finds with blk_register() and returns how many. */
int ahci_init(void);
int nvme_init(void);

#endif
