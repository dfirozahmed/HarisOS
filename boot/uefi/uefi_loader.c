/* ============================================================
 * HarisOS - UEFI version
 *
 * A UEFI application is just a Windows-style PE32+ executable
 * that the firmware loads and calls. The firmware hands us a
 * pointer to the "System Table", which contains function
 * pointers for everything we need: text output, keyboard,
 * timers, shutdown...
 *
 * Unlike BIOS boot:
 *   - the firmware already put the CPU in 64-bit mode
 *   - no assembly, no boot sector, no GDT setup needed
 *   - we get ready-made services for screen and keyboard
 *
 * ============================================================ */

/* ------------------------------------------------------------
 * Minimal UEFI types (x86_64)
 * ------------------------------------------------------------ */
typedef unsigned char      UINT8;
typedef unsigned short     UINT16;
typedef unsigned int       UINT32;
typedef unsigned long long UINT64;
typedef UINT64             UINTN;
typedef UINT16             CHAR16;
typedef UINT8              BOOLEAN;
typedef UINTN              EFI_STATUS;
typedef void              *EFI_HANDLE;
typedef void              *EFI_EVENT;

#define EFIAPI __attribute__((ms_abi))

#define NULL        ((void *)0)
#define EFI_SUCCESS 0

typedef struct {
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;
} EFI_TABLE_HEADER;

typedef struct {
    UINT16 ScanCode;
    CHAR16 UnicodeChar;
} EFI_INPUT_KEY;

/* --- Keyboard (Simple Text Input Protocol) --- */
typedef struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL {
    EFI_STATUS (EFIAPI *Reset)(struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, BOOLEAN ExtendedVerification);
    EFI_STATUS (EFIAPI *ReadKeyStroke)(struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, EFI_INPUT_KEY *Key);
    EFI_EVENT  WaitForKey;
} EFI_SIMPLE_TEXT_INPUT_PROTOCOL;

/* --- Screen (Simple Text Output Protocol) --- */
typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    EFI_STATUS (EFIAPI *Reset)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, BOOLEAN ExtendedVerification);
    EFI_STATUS (EFIAPI *OutputString)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *String);
    void *TestString;
    void *QueryMode;
    void *SetMode;
    EFI_STATUS (EFIAPI *SetAttribute)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN Attribute);
    EFI_STATUS (EFIAPI *ClearScreen)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This);
    void *SetCursorPosition;
    void *EnableCursor;
    void *Mode;
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

/* --- Boot services (only the entries we use are typed; the rest are
 *     placeholders that keep the structure layout correct) --- */
typedef struct {
    EFI_TABLE_HEADER Hdr;
    void *RaiseTPL;
    void *RestoreTPL;
    void *AllocatePages;
    void *FreePages;
    void *GetMemoryMap;
    void *AllocatePool;
    void *FreePool;
    void *CreateEvent;
    void *SetTimer;
    EFI_STATUS (EFIAPI *WaitForEvent)(UINTN NumberOfEvents, EFI_EVENT *Event, UINTN *Index);
    void *SignalEvent;
    void *CloseEvent;
    void *CheckEvent;
    void *InstallProtocolInterface;
    void *ReinstallProtocolInterface;
    void *UninstallProtocolInterface;
    void *HandleProtocol;
    void *Reserved;
    void *RegisterProtocolNotify;
    void *LocateHandle;
    void *LocateDevicePath;
    void *InstallConfigurationTable;
    void *LoadImage;
    void *StartImage;
    void *Exit;
    void *UnloadImage;
    void *ExitBootServices;
    void *GetNextMonotonicCount;
    EFI_STATUS (EFIAPI *Stall)(UINTN Microseconds);
    EFI_STATUS (EFIAPI *SetWatchdogTimer)(UINTN Timeout, UINT64 WatchdogCode, UINTN DataSize, CHAR16 *WatchdogData);
} EFI_BOOT_SERVICES;

typedef struct {
    EFI_TABLE_HEADER Hdr;
    void *GetTime;
    void *SetTime;
    void *GetWakeupTime;
    void *SetWakeupTime;
    void *SetVirtualAddressMap;
    void *ConvertPointer;
    void *GetVariable;
    void *GetNextVariableName;
    void *SetVariable;
    void *GetNextHighMonotonicCount;
    void (EFIAPI *ResetSystem)(UINT32 ResetType, EFI_STATUS ResetStatus, UINTN DataSize, void *ResetData);
} EFI_RUNTIME_SERVICES;

#define EFI_RESET_SHUTDOWN 2

/* --- System table: the one pointer the firmware gives us --- */
typedef struct {
    EFI_TABLE_HEADER Hdr;
    CHAR16 *FirmwareVendor;
    UINT32  FirmwareRevision;
    EFI_HANDLE ConsoleInHandle;
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL *ConIn;
    EFI_HANDLE ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
    EFI_RUNTIME_SERVICES *RuntimeServices;
    EFI_BOOT_SERVICES *BootServices;
    UINTN NumberOfTableEntries;
    void *ConfigurationTable;
} EFI_SYSTEM_TABLE;

/* ------------------------------------------------------------
 * Colors: attribute = text color | (background color << 4)
 * Background colors can only be 0-7.
 * ------------------------------------------------------------ */
#define COLOR_DEFAULT 0x07      /* light gray on black */
#define COLOR_BANNER  0x1F      /* white on blue       */
#define COLOR_GREEN   0x0A
#define COLOR_RED     0x0C
#define COLOR_YELLOW  0x0E
#define COLOR_WHITE   0x0F

#define SCAN_ESC      0x17

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

#define LINE_MAX 64

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
            if (c >= L' ' && len < LINE_MAX - 1) {  /* printable character */
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
 * Entry point: the firmware calls this (like main()).
 * ------------------------------------------------------------ */
EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    (void)ImageHandle;
    ST = SystemTable;

    /* The firmware reboots us after 5 minutes unless we disable this timer. */
    ST->BootServices->SetWatchdogTimer(0, 0, 0, NULL);

    ST->ConOut->ClearScreen(ST->ConOut);

    print_color(L" Welcome to HarisOS ", COLOR_BANNER);
    print(L"\r\n\r\n");

    print_color(L"GREEN ", COLOR_GREEN);
    print_color(L"RED ", COLOR_RED);
    print_color(L"YELLOW", COLOR_YELLOW);
    print(L"\r\n\r\n");

    print(L"Type something and press Enter. ESC = shut down.\r\n\r\n");

    prompt_loop();

    return EFI_SUCCESS;
}
