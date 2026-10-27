#include "amd_firmware.h"
#include "gpu_log.h"
#include "heap.h"
#include "vfs.h"
#include "exec.h"

static const char *names[AMD_FW_MAX] = {
    "renoir_gpu_info.bin", "renoir_asd.bin", "renoir_ce.bin", "renoir_me.bin",
    "renoir_mec.bin", "renoir_mec2.bin", "renoir_pfp.bin", "renoir_rlc.bin",
    "renoir_sdma.bin", "renoir_ta.bin", "renoir_vcn.bin", "renoir_dmcub.bin"
};

/* The same files as they are called on haris.img: a FAT root folder here only has 8.3 names (see CMakeLists.txt). */
static const char *disk_names[AMD_FW_MAX] = {
    "R_GPUINF.BIN", "R_ASD.BIN", "R_CE.BIN", "R_ME.BIN",
    "R_MEC.BIN", "R_MEC2.BIN", "R_PFP.BIN", "R_RLC.BIN",
    "R_SDMA.BIN", "R_TA.BIN", "R_VCN.BIN", "R_DMCUB.BIN"
};

#define FW_FILE_MAX (512u * 1024u) /* the biggest image (MEC) is 268 KB */

static AmdFirmwareEntry entries[AMD_FW_MAX];
static int staged_count;
static const char *status = "firmware staging ABI not present";

/* The FirozOS loader handed the images over in a record at physical 0x9800. The HarisOS loader does not; the images are
   files in the top folder of a drive instead and amd_firmware_load() reads them into kernel heap blocks. That needs the
   drives mounted, so amd_firmware_init() (early) only clears the table. */
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
    status = "Renoir firmware not loaded yet (drives not mounted)";
}

int amd_firmware_load(void)
{
    int i;
    for (i = 0; i < AMD_FW_MAX; ++i) {
        int node = exec_find(disk_names[i]);
        const VfsNode *vn = node < 0 ? (const VfsNode *)0 : vfs_node(node);
        serial_print("  fw "); serial_print(names[i]); serial_print(": ");
        if (!vn || vn->is_dir || vn->size == 0 || vn->size > FW_FILE_MAX) {
            serial_print(vn ? "bad size\n" : "not on a drive\n");
            continue;
        }
        unsigned char *buf = (unsigned char *)kmalloc(vn->size);
        if (!buf) {
            serial_print("out of memory\n");
            continue;
        }
        /* The PSP and the GPU are given 32-bit addresses (see GPU_DMA_LIMIT): refuse a block above 4 GiB. */
        if ((unsigned long long)buf + vn->size > GPU_DMA_LIMIT) {
            kfree(buf);
            serial_print("heap block above 4 GiB\n");
            continue;
        }
        if (vfs_read(node, 0, buf, vn->size) != (int)vn->size) {
            kfree(buf);
            serial_print("read failed\n");
            continue;
        }
        entries[i].address = (uint32_t)(unsigned long long)buf;
        entries[i].size = vn->size;
        entries[i].status = 1;
        staged_count++;
        serial_dec32(vn->size);
        serial_print(" bytes\n");
    }
    if (staged_count == AMD_FW_MAX)
        status = "all Renoir firmware images loaded from the boot drive";
    else if (staged_count)
        status = "partial Renoir firmware (some files missing on the boot drive)";
    else
        status = "Renoir firmware files not found on a drive";
    return staged_count;
}

int amd_firmware_staged(void) { return staged_count > 0; }
int amd_firmware_count(void) { return staged_count; }
const AmdFirmwareEntry *amd_firmware_get(int index)
{
    if (index < 0 || index >= AMD_FW_MAX) return (const AmdFirmwareEntry *)0;
    return &entries[index];
}
const char *amd_firmware_status(void) { return status; }
