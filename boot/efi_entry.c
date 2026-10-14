/* UEFI-specific entry point. Firmware interfaces stop at this boundary. */
#include "uefi.h"
#include "desktop.h"

static EFI_GUID GopGuid = {0x9042A9DE, 0x23DC, 0x4A38, {0x96, 0xFB, 0x7A, 0xDE, 0xD0, 0x80, 0x51, 0x6A}};

void kernel_main(void);

EFI_STATUS EFIAPI efi_kernel_entry(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    EFI_STATUS status;
    (void)ImageHandle;

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

    desktop_set_framebuffer(framebuffer, width, height, pitch, red_mask_mode);
    kernel_main();
    return EFI_SUCCESS; /* normally unreachable */
}
