#ifndef FIROZOS_AMD_GPU_H
#define FIROZOS_AMD_GPU_H
#include "gpu_types.h"

int amd_gpu_supported(uint16_t vendor, uint16_t device);
int amd_gpu_is_target(uint16_t vendor, uint16_t device, uint16_t subsys_vendor, uint16_t subsys_device, uint8_t revision);
const char *amd_gpu_name(uint16_t device);

/* Safe native-driver probe. It only inspects PCI BAR/configuration state. */
int amd_gpu_probe(uint16_t device, uint64_t mmio_base, uint32_t mmio_size,
                  int bar_is_64bit, int bar_is_io);
const char *amd_gpu_native_status(void);

#endif
