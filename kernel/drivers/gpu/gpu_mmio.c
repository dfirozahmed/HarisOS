#include "gpu_mmio.h"

int gpu_mmio_can_access(uint64_t physical_base)
{
    return physical_base != 0 && physical_base <= 0xFFFFFFFFULL;
}

volatile uint8_t *gpu_mmio_ptr(uint64_t physical_base)
{
    if (!gpu_mmio_can_access(physical_base))
        return (volatile uint8_t *)0;

    return (volatile uint8_t *)(unsigned long long)physical_base;
}

uint32_t gpu_mmio_read32(uint64_t physical_base, uint32_t offset)
{
    volatile uint8_t *p = gpu_mmio_ptr(physical_base);
    if (!p)
        return 0xFFFFFFFFu;
    return *(volatile uint32_t *)(p + offset);
}

uint16_t gpu_mmio_read16(uint64_t physical_base, uint32_t offset)
{
    volatile uint8_t *p = gpu_mmio_ptr(physical_base);
    if (!p)
        return 0xFFFFu;
    return *(volatile uint16_t *)(p + offset);
}

uint8_t gpu_mmio_read8(uint64_t physical_base, uint32_t offset)
{
    volatile uint8_t *p = gpu_mmio_ptr(physical_base);
    if (!p)
        return 0xFFu;
    return *(volatile uint8_t *)(p + offset);
}

void gpu_mmio_write32(uint64_t physical_base, uint32_t offset, uint32_t value)
{
    volatile uint8_t *p = gpu_mmio_ptr(physical_base);
    if (!p)
        return;
    *(volatile uint32_t *)(p + offset) = value;
    __asm__ volatile("" ::: "memory");
}
