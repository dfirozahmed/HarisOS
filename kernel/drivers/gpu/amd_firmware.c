#include "amd_firmware.h"

static const char *names[AMD_FW_MAX] = {
    "renoir_gpu_info.bin", "renoir_asd.bin", "renoir_ce.bin", "renoir_me.bin",
    "renoir_mec.bin", "renoir_mec2.bin", "renoir_pfp.bin", "renoir_rlc.bin",
    "renoir_sdma.bin", "renoir_ta.bin", "renoir_vcn.bin", "renoir_dmcub.bin"
};
static AmdFirmwareEntry entries[AMD_FW_MAX];
static int staged_count;
static const char *status = "firmware staging ABI not present";

/* The FirozOS loader left a staging record at physical 0x9800. The HarisOS loader does not stage the Renoir
   firmware files yet (and that low page is not ours to read), so no image is ever reported as staged. */
void amd_firmware_init(void)
{
    int i;
    staged_count = 0;
    for (i = 0; i < AMD_FW_MAX; ++i) {
        entries[i].name = names[i];
        entries[i].address = 0;
        entries[i].size = 0;
        entries[i].status = 0;
    }
    status = "Renoir firmware files not staged (HarisOS loader does not stage them yet)";
}

int amd_firmware_staged(void) { return staged_count > 0; }
int amd_firmware_count(void) { return staged_count; }
const AmdFirmwareEntry *amd_firmware_get(int index)
{
    if (index < 0 || index >= AMD_FW_MAX) return (const AmdFirmwareEntry *)0;
    return &entries[index];
}
const char *amd_firmware_status(void) { return status; }
