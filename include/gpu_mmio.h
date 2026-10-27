#ifndef FIROZOS_GPU_MMIO_H
#define FIROZOS_GPU_MMIO_H
#include "gpu_types.h"

/*
 * FirozOS currently runs the kernel in a 32-bit address model.  The BIOS
 * paging path identity-maps the complete 32-bit physical address space;
 * UEFI keeps its firmware identity map.  Therefore a BAR below 4 GiB can
 * be used through an identity-mapped MMIO pointer.  No BAR is remapped and
 * no device register is written by this helper.
 */
int gpu_mmio_can_access(uint64_t physical_base);
volatile uint8_t *gpu_mmio_ptr(uint64_t physical_base);
uint32_t gpu_mmio_read32(uint64_t physical_base, uint32_t offset);
uint16_t gpu_mmio_read16(uint64_t physical_base, uint32_t offset);
uint8_t  gpu_mmio_read8(uint64_t physical_base, uint32_t offset);
/* Only for the explicit, opt-in bring-up code (FIROZOS_GART_ENABLE). */
void gpu_mmio_write32(uint64_t physical_base, uint32_t offset, uint32_t value);

#endif
