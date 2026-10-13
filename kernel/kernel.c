/* ============================================================
 * HarisOS - kernel (kernel/kernel.c)
 *
 * This is a separate program from the UEFI loader. The loader
 * (boot/uefi/uefi_loader.c) reads KERNEL.EFI from the boot disk,
 * and starts it by calling kernel_main() below.
 *
 * Boot order:  firmware -> BOOTX64.EFI (loader) -> KERNEL.EFI (this file)
 * ============================================================ */
#include "uefi.h"

#define SCAN_ESC 0x17
#define LINE_MAX 64

static EFI_SYSTEM_TABLE *ST;    /* saved system table pointer */

/* ------------------------------------------------------------
 * Print helpers (UEFI strings are UTF-16: use L"..." literals,
 * and "\r\n" for a new line)
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

static void print_char(CHAR16 c)
{
    CHAR16 s[2];
    s[0] = c;
    s[1] = 0;
    print(s);
}

/* ------------------------------------------------------------
 * Keyboard: wait for a key, return its character (0 for keys
 * without one, like arrows). The scan code tells special keys apart.
 * ------------------------------------------------------------ */
static CHAR16 read_key(UINT16 *scan_code)
{
    UINTN index;
    EFI_INPUT_KEY key;

    for (;;) {
        ST->BootServices->WaitForEvent(1, &ST->ConIn->WaitForKey, &index);
        if (ST->ConIn->ReadKeyStroke(ST->ConIn, &key) == EFI_SUCCESS) {
            *scan_code = key.ScanCode;
            return key.UnicodeChar;
        }
    }
}

static void prompt_loop(void)
{
    CHAR16 line[LINE_MAX];

    for (;;) {
        print_color(L"haris> ", COLOR_YELLOW);

        UINTN len = 0;
        for (;;) {
            UINT16 scan;
            CHAR16 c = read_key(&scan);

            if (scan == SCAN_ESC) {                 /* ESC: power off */
                print(L"\r\nShutting down...\r\n");
                ST->BootServices->Stall(500000);    /* half a second */
                ST->RuntimeServices->ResetSystem(EFI_RESET_SHUTDOWN, EFI_SUCCESS, 0, NULL);
                for (;;) { }
            }
            if (c == L'\r') {                       /* Enter */
                print(L"\r\n");
                break;
            }
            if (c == L'\b') {                       /* Backspace */
                if (len > 0) {
                    len--;
                    print(L"\b \b");                /* back, erase, back */
                }
                continue;
            }
            if (c >= L' ' && len < LINE_MAX - 1) { /* printable character */
                line[len++] = c;
                print_char(c);
            }
        }
        line[len] = 0;

        if (len > 0) {
            print(L"You typed: ");
            print(line);
            print(L"\r\n");
        }
    }
}

/* ------------------------------------------------------------
 * Kernel entry point: the loader starts us here.
 * ------------------------------------------------------------ */
EFI_STATUS EFIAPI kernel_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    (void)ImageHandle;
    ST = SystemTable;

    print_color(L" HarisOS kernel started ", COLOR_BANNER);
    print(L"\r\n\r\n");

    print_color(L"GREEN ", COLOR_GREEN);
    print_color(L"RED ", COLOR_RED);
    print_color(L"YELLOW", COLOR_YELLOW);
    print(L"\r\n\r\n");

    print(L"Type something and press Enter. ESC = shut down.\r\n\r\n");

    prompt_loop();

    return EFI_SUCCESS;
}
