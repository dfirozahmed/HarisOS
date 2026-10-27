#ifndef FIROZOS_AMD_FIRMWARE_H
#define FIROZOS_AMD_FIRMWARE_H
#include "gpu_types.h"

#define AMD_FW_MAX 12

typedef struct {
    const char *name;
    uint32_t address;
    uint32_t size;
    uint32_t status; /* 0 missing, 1 staged */
} AmdFirmwareEntry;

void amd_firmware_init(void);
int amd_firmware_staged(void);
int amd_firmware_count(void);
const AmdFirmwareEntry *amd_firmware_get(int index);
const char *amd_firmware_status(void);

#endif
