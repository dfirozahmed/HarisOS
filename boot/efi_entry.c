/* UEFI-specific entry point. Firmware interfaces stop at this boundary. */
#include "uefi.h"
#include "bootinfo.h"
#include "smp.h"

static EFI_GUID GopGuid = {0x9042A9DE, 0x23DC, 0x4A38, {0x96, 0xFB, 0x7A, 0xDE, 0xD0, 0x80, 0x51, 0x6A}};

void kernel_main(const BootInfo *info);

/* The firmware's runtime services stay usable after ExitBootServices, so the kernel can ask the
 * firmware to power off or reboot. This small wrapper hides the UEFI calling convention. */
static EFI_SYSTEM_TABLE *g_system_table;

static void efi_reset(unsigned type)
{
    EFI_SYSTEM_TABLE *st = g_system_table;
    if (st && st->RuntimeServices && st->RuntimeServices->ResetSystem)
        st->RuntimeServices->ResetSystem(type, EFI_SUCCESS, 0, NULL); /* EfiResetCold = 0, EfiResetShutdown = 2 */
}

/* Lives in the kernel image (EfiLoaderData), which stays valid after ExitBootServices. */
static BootInfo boot_info;

static EFI_GUID Acpi20Guid = {0x8868E871, 0xE4F1, 0x11D3, {0xBC, 0x22, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81}};
static EFI_GUID Acpi10Guid = {0xEB9D2D30, 0x2D88, 0x11D3, {0x9A, 0x16, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D}};

static int same_guid(const EFI_GUID *a, const EFI_GUID *b)
{
    if (a->Data1 != b->Data1 || a->Data2 != b->Data2 || a->Data3 != b->Data3)
        return 0;
    for (int i = 0; i < 8; i++)
        if (a->Data4[i] != b->Data4[i])
            return 0;
    return 1;
}

/* The ACPI RSDP, from the firmware's configuration table (the 2.0 one is preferred). */
static unsigned long long find_rsdp(EFI_SYSTEM_TABLE *st)
{
    EFI_CONFIGURATION_TABLE *t = (EFI_CONFIGURATION_TABLE *)st->ConfigurationTable;
    unsigned long long v1 = 0;
    for (UINTN i = 0; t && i < st->NumberOfTableEntries; i++)
    {
        if (same_guid(&t[i].VendorGuid, &Acpi20Guid))
            return (unsigned long long)(UINTN)t[i].VendorTable;
        if (same_guid(&t[i].VendorGuid, &Acpi10Guid))
            v1 = (unsigned long long)(UINTN)t[i].VendorTable;
    }
    return v1;
}

/* Memory for the SMP start-up code (smp.h): a processor woken by a SIPI starts in real mode, so its code must be below 1 MiB.
 * Allocated and filled now, while the firmware still hands out memory. The code page is EfiLoaderCode, because some firmware
 * maps data as non-executable. Below 640 KiB, which no PC uses for anything else. */
static void prepare_smp(EFI_BOOT_SERVICES *bs, BootInfo *out)
{
    UINT64 code = 0x9FFFF, data = 0x9FFFF;
    if (EFI_ERROR(bs->AllocatePages(EFI_ALLOCATE_MAX_ADDRESS, EFI_LOADER_CODE, 1, &code)))
        return;
    if (EFI_ERROR(bs->AllocatePages(EFI_ALLOCATE_MAX_ADDRESS, EFI_LOADER_DATA, SMP_DATA_PAGES, &data)))
        return;
    if (smp_install(code, data) != 0)
        return;
    out->smp_code = code;
    out->smp_data = data;
}

/* Leave UEFI Boot Services. After this returns EFI_SUCCESS the firmware no longer
 * owns the machine: no ConOut, no LocateProtocol, no AllocatePool, no Stall.
 * Rules: nothing may touch Boot Services between GetMemoryMap and ExitBootServices,
 * otherwise MapKey becomes stale and ExitBootServices fails. */
static EFI_STATUS leave_boot_services(EFI_HANDLE image, EFI_SYSTEM_TABLE *st, BootInfo *out)
{
    EFI_BOOT_SERVICES *bs = st->BootServices;
    EFI_MEMORY_DESCRIPTOR *map = NULL;
    UINTN map_size = 0, map_key = 0, desc_size = 0;
    UINT32 desc_ver = 0;
    EFI_STATUS status;

    /* 1st call only reports the needed size (EFI_BUFFER_TOO_SMALL is expected). */
    status = bs->GetMemoryMap(&map_size, NULL, &map_key, &desc_size, &desc_ver);
    if (status != EFI_BUFFER_TOO_SMALL)
        return EFI_ERROR(status) ? status : EFI_LOAD_ERROR;

    /* AllocatePool itself adds map entries, so leave room for a few extra. */
    map_size += 4 * desc_size;
    status = bs->AllocatePool(EFI_LOADER_DATA, map_size, (void **)&map);
    if (EFI_ERROR(status))
        return status;

    for (int attempt = 0; attempt < 5; attempt++)
    {
        UINTN size = map_size;
        status = bs->GetMemoryMap(&size, map, &map_key, &desc_size, &desc_ver);
        if (EFI_ERROR(status))
            return status;

        status = bs->ExitBootServices(image, map_key);
        if (!EFI_ERROR(status))
        {
            /* Boot Services are gone from here on. The map buffer is EfiLoaderData,
             * which the firmware does not reuse, so the kernel may read it. */
            out->memmap = map;
            out->memmap_size = size;
            out->desc_size = desc_size;
            out->desc_version = desc_ver;
            return EFI_SUCCESS;
        }
        /* Stale map key: fetch the map again and retry. */
    }
    return status;
}

EFI_STATUS EFIAPI efi_kernel_entry(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    EFI_STATUS status;

    if (!SystemTable || !SystemTable->BootServices)
        return EFI_INVALID_PARAMETER;

    status = SystemTable->BootServices->LocateProtocol(&GopGuid, NULL, (void **)&gop);
    if (EFI_ERROR(status) || !gop || !gop->Mode || !gop->Mode->Info)
    {
        if (SystemTable->ConOut)
        {
            SystemTable->ConOut->ClearScreen(SystemTable->ConOut);
            SystemTable->ConOut->OutputString(SystemTable->ConOut, L"HarisOS: GOP graphics unavailable.\r\n");
        }
        return EFI_UNSUPPORTED;
    }

    unsigned int width = gop->Mode->Info->HorizontalResolution;
    unsigned int height = gop->Mode->Info->VerticalResolution;
    unsigned int pitch = gop->Mode->Info->PixelsPerScanLine;
    /* PixelFormat 1 = BGR bytes in memory, i.e. the 32-bit word is 0x00RRGGBB (what red_mask_mode means);
     * 0 = RGB bytes, i.e. the word is 0x00BBGGRR */
    unsigned int red_mask_mode = (gop->Mode->Info->PixelFormat == 1);
    void *framebuffer = (void *)(UINTN)gop->Mode->FrameBufferBase;

    if (!framebuffer || width < 320 || height < 200 || pitch < width)
    {
        if (SystemTable->ConOut)
        {
            SystemTable->ConOut->ClearScreen(SystemTable->ConOut);
            SystemTable->ConOut->OutputString(SystemTable->ConOut, L"HarisOS: invalid framebuffer.\r\n");
        }
        return EFI_DEVICE_ERROR;
    }

    /* The framebuffer address/size above was read while Boot Services were still alive.
     * It stays valid afterwards, so we only store it and then leave UEFI. */
    boot_info.framebuffer = framebuffer;
    boot_info.width = width;
    boot_info.height = height;
    boot_info.pitch = pitch;
    boot_info.red_mask_mode = red_mask_mode;
    g_system_table = SystemTable;
    boot_info.reset_system = efi_reset;
    boot_info.acpi_rsdp = find_rsdp(SystemTable);
    prepare_smp(SystemTable->BootServices, &boot_info);

    status = leave_boot_services(ImageHandle, SystemTable, &boot_info);
    if (EFI_ERROR(status))
    {
        /* Still inside UEFI, so printing is allowed. */
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"HarisOS: ExitBootServices failed.\r\n");
        return status;
    }

#if defined(__x86_64__) || defined(__i386__)
    __asm__ volatile("cli"); /* firmware's interrupt handlers/timers are no longer ours to rely on */
#endif

    kernel_main(&boot_info); /* never returns */
    for (;;)
    {
    }
    return EFI_SUCCESS;
}
