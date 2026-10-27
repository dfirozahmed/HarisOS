#include "amd_gpu.h"
#include "gpu_mmio.h"

static const char *status = "not probed";

int amd_gpu_supported(uint16_t vendor, uint16_t device)
{
    return vendor == 0x1002 &&
           (device == 0x1636 || device == 0x15E7 ||
            device == 0x1638 || device == 0x164C ||
            device == 0x15D8 || device == 0x15DD ||
            device == 0x67DF || device == 0x731F ||
            device == 0x73BF || device == 0x744C);
}

int amd_gpu_is_target(uint16_t vendor, uint16_t device,
                     uint16_t subsys_vendor, uint16_t subsys_device,
                     uint8_t revision)
{
    /* Exact machine reported by Windows:
       AMD 1002:1636, HP subsystem 103C:87CF, revision C3. */
    return vendor == 0x1002 && device == 0x1636 &&
           subsys_vendor == 0x103C && subsys_device == 0x87CF &&
           revision == 0xC3;
}

const char *amd_gpu_name(uint16_t device)
{
    switch (device)
    {
    case 0x1636: return "AMD Radeon(TM) Graphics (Renoir / Ryzen 5 4500U)";
    case 0x1638: return "AMD Radeon Graphics (Renoir)";
    case 0x164C: return "AMD Radeon Graphics (Renoir)";
    case 0x15E7: return "AMD Radeon Graphics (Renoir)";
    case 0x15D8: return "AMD Radeon Vega (Raven/Picasso)";
    case 0x15DD: return "AMD Radeon Vega (Raven)";
    case 0x67DF: return "AMD Radeon RX 480/580";
    case 0x731F: return "AMD Radeon RX 5700";
    case 0x73BF: return "AMD Radeon RX 6800/6900";
    case 0x744C: return "AMD Radeon RX 7600";
    default:     return "AMD Radeon Graphics";
    }
}

int amd_gpu_probe(uint16_t device, uint64_t mmio_base, uint32_t mmio_size,
                  int bar_is_64bit, int bar_is_io)
{
    (void)device;
    (void)mmio_size;

    if (bar_is_io)
    {
        status = "PCI BAR0 is I/O space; native MMIO backend unavailable";
        return 0;
    }

    if (mmio_base == 0)
    {
        status = "no MMIO BAR detected; UEFI GOP fallback";
        return 0;
    }

    if (!gpu_mmio_can_access(mmio_base))
    {
        status = "MMIO BAR is above 4 GiB; current 32-bit kernel cannot map it";
        return 0;
    }

    /* Mapping is identity-based for now. Read-only helpers are exposed, but
       no vendor register is touched here because register layouts differ by
       Intel generation / AMD ASIC family. */
    if (!gpu_mmio_ptr(mmio_base))
    {
        status = "MMIO identity mapping unavailable; UEFI GOP fallback";
        return 0;
    }

    status = bar_is_64bit
           ? "MMIO identity mapping ready (64-bit BAR); vendor registers not initialized"
           : "MMIO identity mapping ready (32-bit BAR); vendor registers not initialized";
    return 1;
}

const char *amd_gpu_native_status(void)
{
    return status;
}
