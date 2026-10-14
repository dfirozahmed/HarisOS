/* UEFI-specific entry point. Firmware interfaces stop at this boundary. */
#include "uefi.h"
#include "bootinfo.h"

static EFI_GUID GopGuid = {0x9042A9DE, 0x23DC, 0x4A38, {0x96, 0xFB, 0x7A, 0xDE, 0xD0, 0x80, 0x51, 0x6A}};

void kernel_main(const BootInfo *info);

/* Lives in the kernel image (EfiLoaderData), which stays valid after ExitBootServices. */
static BootInfo boot_info;

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
    unsigned int red_mask_mode = (gop->Mode->Info->PixelFormat == 0);
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
