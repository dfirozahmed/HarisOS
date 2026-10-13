/* ============================================================
 * HarisOS - UEFI loader (boot/uefi/uefi_loader.c)
 *
 * The firmware starts this file as BOOTX64.EFI. Its only job is:
 *   1. show a banner
 *   2. read \KERNEL.EFI from the same disk it was booted from
 *   3. start the kernel (kernel/kernel.c) with LoadImage/StartImage
 *
 * The kernel itself lives in kernel/kernel.c.
 * ============================================================ */
#include "uefi.h"

static EFI_SYSTEM_TABLE *ST;    /* saved system table pointer */

static EFI_GUID LoadedImageGuid    = { 0x5B1B31A1, 0x9562, 0x11D2, { 0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } };
static EFI_GUID SimpleFileSystemGuid = { 0x964E5B22, 0x6459, 0x11D2, { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } };

/* ------------------------------------------------------------
 * Print helpers
 * ------------------------------------------------------------ */
static void print(const CHAR16 *s)
{
    ST->ConOut->OutputString(ST->ConOut, (CHAR16 *)s);
}

static void print_color(const CHAR16 *s, UINTN attribute)
{
    ST->ConOut->SetAttribute(ST->ConOut, attribute);
    print(s);
    ST->ConOut->SetAttribute(ST->ConOut, COLOR_DEFAULT);
}

static void print_hex(UINT64 v)
{
    static const CHAR16 digits[] = L"0123456789ABCDEF";
    CHAR16 buf[19];
    buf[0] = L'0';
    buf[1] = L'x';
    for (int i = 0; i < 16; i++)
        buf[2 + i] = digits[(v >> (60 - 4 * i)) & 0xF];
    buf[18] = 0;
    print(buf);
}

static void fail(const CHAR16 *what, EFI_STATUS status)
{
    print_color(L"ERROR: ", COLOR_RED);
    print(what);
    print(L" (status ");
    print_hex(status);
    print(L")\r\n");
}

/* ------------------------------------------------------------
 * Read a whole file from the disk we booted from into memory.
 * On success *out_buf / *out_size describe the file contents.
 * ------------------------------------------------------------ */
static EFI_STATUS load_file(EFI_HANDLE image, const CHAR16 *path, void **out_buf, UINTN *out_size)
{
    EFI_STATUS st;
    EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
    EFI_FILE_PROTOCOL *root = NULL, *file = NULL;
    UINT64 size = 0;
    UINTN read_size;
    void *buf = NULL;

    /* Which device did the loader come from? */
    st = ST->BootServices->HandleProtocol(image, &LoadedImageGuid, (void **)&li);
    if (EFI_ERROR(st)) { fail(L"LoadedImage protocol", st); return st; }

    /* Open that device's file system */
    st = ST->BootServices->HandleProtocol(li->DeviceHandle, &SimpleFileSystemGuid, (void **)&fs);
    if (EFI_ERROR(st)) { fail(L"file system protocol", st); return st; }

    st = fs->OpenVolume(fs, &root);
    if (EFI_ERROR(st)) { fail(L"OpenVolume", st); return st; }

    st = root->Open(root, &file, (CHAR16 *)path, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(st)) { fail(L"cannot open kernel file", st); root->Close(root); return st; }

    /* File size: seek to the end, ask for the position, seek back */
    file->SetPosition(file, 0xFFFFFFFFFFFFFFFFULL);
    file->GetPosition(file, &size);
    file->SetPosition(file, 0);

    st = ST->BootServices->AllocatePool(EFI_LOADER_DATA, (UINTN)size, &buf);
    if (EFI_ERROR(st)) { fail(L"AllocatePool", st); file->Close(file); root->Close(root); return st; }

    read_size = (UINTN)size;
    st = file->Read(file, &read_size, buf);
    file->Close(file);
    root->Close(root);
    if (EFI_ERROR(st) || read_size != (UINTN)size) {
        fail(L"reading kernel file", st);
        ST->BootServices->FreePool(buf);
        return EFI_ERROR(st) ? st : (EFI_STATUS)0x8000000000000001ULL;
    }

    *out_buf  = buf;
    *out_size = (UINTN)size;
    return EFI_SUCCESS;
}

/* ------------------------------------------------------------
 * Entry point: the firmware calls this (like main()).
 * ------------------------------------------------------------ */
EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    EFI_STATUS st;
    void *kernel_buf = NULL;
    UINTN kernel_size = 0;
    EFI_HANDLE kernel_image = NULL;

    ST = SystemTable;

    /* The firmware reboots us after 5 minutes unless we disable this timer. */
    ST->BootServices->SetWatchdogTimer(0, 0, 0, NULL);

    ST->ConOut->ClearScreen(ST->ConOut);

    print_color(L" Welcome to HarisOS ", COLOR_BANNER);
    print(L"\r\n\r\n");

    print(L"Loading \\KERNEL.EFI ...\r\n");
    st = load_file(ImageHandle, L"\\KERNEL.EFI", &kernel_buf, &kernel_size);
    if (EFI_ERROR(st))
        goto halt;

    print(L"Kernel loaded, starting it...\r\n");
    ST->BootServices->Stall(300000);

    /* Prepare the kernel image (firmware does the PE relocation for us) ... */
    st = ST->BootServices->LoadImage(FALSE, ImageHandle, NULL, kernel_buf, kernel_size, &kernel_image);
    ST->BootServices->FreePool(kernel_buf);
    if (EFI_ERROR(st)) {
        fail(L"LoadImage (is KERNEL.EFI a valid EFI image?)", st);
        goto halt;
    }

    /* ... and jump to kernel_main(). It does not return in normal use. */
    st = ST->BootServices->StartImage(kernel_image, NULL, NULL);
    if (EFI_ERROR(st))
        fail(L"kernel exited with an error", st);
    else
        print(L"Kernel returned.\r\n");

halt:
    print(L"\r\nPress any key to stop.\r\n");
    {
        UINTN index;
        ST->BootServices->WaitForEvent(1, &ST->ConIn->WaitForKey, &index);
    }
    return st;
}
