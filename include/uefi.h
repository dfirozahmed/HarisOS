/* ============================================================
 * HarisOS - minimal UEFI definitions (x86_64)
 * Shared by the loader (boot/uefi/uefi_loader.c) and the
 * kernel (boot/efi_entry.c).
 * ============================================================ */
#ifndef HARIS_UEFI_H
#define HARIS_UEFI_H

typedef unsigned char UINT8;
typedef unsigned short UINT16;
typedef unsigned int UINT32;
typedef unsigned long long UINT64;
typedef UINT64 UINTN;
typedef UINT16 CHAR16;
typedef UINT8 BOOLEAN;
typedef UINTN EFI_STATUS;
typedef void *EFI_HANDLE;
typedef void *EFI_EVENT;

#define EFIAPI __attribute__((ms_abi))

#define NULL ((void *)0)
#define TRUE 1
#define FALSE 0
#define EFI_SUCCESS 0
#define EFI_LOAD_ERROR ((EFI_STATUS)0x8000000000000001ULL)
#define EFI_INVALID_PARAMETER ((EFI_STATUS)0x8000000000000002ULL)
#define EFI_UNSUPPORTED ((EFI_STATUS)0x8000000000000003ULL)
#define EFI_DEVICE_ERROR ((EFI_STATUS)0x8000000000000007ULL)
#define EFI_ERROR(s) (((EFI_STATUS)(s)) & 0x8000000000000000ULL)

typedef struct
{
    UINT32 Data1;
    UINT16 Data2;
    UINT16 Data3;
    UINT8 Data4[8];
} EFI_GUID;

typedef struct
{
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;
} EFI_TABLE_HEADER;

typedef struct
{
    UINT16 ScanCode;
    CHAR16 UnicodeChar;
} EFI_INPUT_KEY;

/* --- Keyboard (Simple Text Input Protocol) --- */
typedef struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL
{
    EFI_STATUS(EFIAPI *Reset)(struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, BOOLEAN ExtendedVerification);
    EFI_STATUS(EFIAPI *ReadKeyStroke)(struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, EFI_INPUT_KEY *Key);
    EFI_EVENT WaitForKey;
} EFI_SIMPLE_TEXT_INPUT_PROTOCOL;

/* --- Screen (Simple Text Output Protocol) --- */
typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL
{
    EFI_STATUS(EFIAPI *Reset)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, BOOLEAN ExtendedVerification);
    EFI_STATUS(EFIAPI *OutputString)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *String);
    void *TestString;
    void *QueryMode;
    void *SetMode;
    EFI_STATUS(EFIAPI *SetAttribute)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN Attribute);
    EFI_STATUS(EFIAPI *ClearScreen)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This);
    void *SetCursorPosition;
    void *EnableCursor;
    void *Mode;
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

/* --- Boot services (unused entries are placeholders that keep the layout correct) --- */
typedef struct
{
    EFI_TABLE_HEADER Hdr;
    void *RaiseTPL;
    void *RestoreTPL;
    void *AllocatePages;
    void *FreePages;
    void *GetMemoryMap;
    EFI_STATUS(EFIAPI *AllocatePool)(UINT32 PoolType, UINTN Size, void **Buffer);
    EFI_STATUS(EFIAPI *FreePool)(void *Buffer);
    void *CreateEvent;
    void *SetTimer;
    EFI_STATUS(EFIAPI *WaitForEvent)(UINTN NumberOfEvents, EFI_EVENT *Event, UINTN *Index);
    void *SignalEvent;
    void *CloseEvent;
    void *CheckEvent;
    void *InstallProtocolInterface;
    void *ReinstallProtocolInterface;
    void *UninstallProtocolInterface;
    EFI_STATUS(EFIAPI *HandleProtocol)(EFI_HANDLE Handle, EFI_GUID *Protocol, void **Interface);
    void *Reserved;
    void *RegisterProtocolNotify;
    void *LocateHandle;
    void *LocateDevicePath;
    void *InstallConfigurationTable;
    EFI_STATUS(EFIAPI *LoadImage)(BOOLEAN BootPolicy, EFI_HANDLE ParentImageHandle, void *DevicePath,
                                  void *SourceBuffer, UINTN SourceSize, EFI_HANDLE *ImageHandle);
    EFI_STATUS(EFIAPI *StartImage)(EFI_HANDLE ImageHandle, UINTN *ExitDataSize, CHAR16 **ExitData);
    void *Exit;
    EFI_STATUS(EFIAPI *UnloadImage)(EFI_HANDLE ImageHandle);
    void *ExitBootServices;
    void *GetNextMonotonicCount;
    EFI_STATUS(EFIAPI *Stall)(UINTN Microseconds);
    EFI_STATUS(EFIAPI *SetWatchdogTimer)(UINTN Timeout, UINT64 WatchdogCode, UINTN DataSize, CHAR16 *WatchdogData);
    void *ConnectController;
    void *DisconnectController;
    void *OpenProtocol;
    void *CloseProtocol;
    void *OpenProtocolInformation;
    void *ProtocolsPerHandle;
    void *LocateHandleBuffer;
    EFI_STATUS(EFIAPI *LocateProtocol)(EFI_GUID *Protocol, void *Registration, void **Interface);
} EFI_BOOT_SERVICES;

typedef struct
{
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
    void(EFIAPI *ResetSystem)(UINT32 ResetType, EFI_STATUS ResetStatus, UINTN DataSize, void *ResetData);
} EFI_RUNTIME_SERVICES;

#define EFI_RESET_SHUTDOWN 2

/* --- System table: the one pointer the firmware gives us --- */
typedef struct
{
    EFI_TABLE_HEADER Hdr;
    CHAR16 *FirmwareVendor;
    UINT32 FirmwareRevision;
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

/* --- Graphics Output Protocol (GOP) --- */
typedef struct
{
    UINT32 Version;
    UINT32 HorizontalResolution;
    UINT32 VerticalResolution;
    UINT32 PixelFormat;
    UINT32 PixelInformation[4];
    UINT32 PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct
{
    UINT32 MaxMode;
    UINT32 Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    UINTN SizeOfInfo;
    UINT64 FrameBufferBase;
    UINTN FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct EFI_GRAPHICS_OUTPUT_PROTOCOL EFI_GRAPHICS_OUTPUT_PROTOCOL;
struct EFI_GRAPHICS_OUTPUT_PROTOCOL
{
    EFI_STATUS(EFIAPI *QueryMode)(EFI_GRAPHICS_OUTPUT_PROTOCOL *This, UINT32 ModeNumber, UINTN *SizeOfInfo, EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info);
    EFI_STATUS(EFIAPI *SetMode)(EFI_GRAPHICS_OUTPUT_PROTOCOL *This, UINT32 ModeNumber);
    void *Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
};

/* --- Loaded Image protocol (we only need DeviceHandle: the disk we booted from) --- */
typedef struct
{
    UINT32 Revision;
    EFI_HANDLE ParentHandle;
    EFI_SYSTEM_TABLE *SystemTable;
    EFI_HANDLE DeviceHandle;
} EFI_LOADED_IMAGE_PROTOCOL;

/* --- File system access --- */
typedef struct EFI_FILE_PROTOCOL
{
    UINT64 Revision;
    EFI_STATUS(EFIAPI *Open)(struct EFI_FILE_PROTOCOL *This, struct EFI_FILE_PROTOCOL **NewHandle,
                             CHAR16 *FileName, UINT64 OpenMode, UINT64 Attributes);
    EFI_STATUS(EFIAPI *Close)(struct EFI_FILE_PROTOCOL *This);
    void *Delete;
    EFI_STATUS(EFIAPI *Read)(struct EFI_FILE_PROTOCOL *This, UINTN *BufferSize, void *Buffer);
    void *Write;
    EFI_STATUS(EFIAPI *GetPosition)(struct EFI_FILE_PROTOCOL *This, UINT64 *Position);
    EFI_STATUS(EFIAPI *SetPosition)(struct EFI_FILE_PROTOCOL *This, UINT64 Position);
    void *GetInfo;
    void *SetInfo;
    void *Flush;
} EFI_FILE_PROTOCOL;

typedef struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL
{
    UINT64 Revision;
    EFI_STATUS(EFIAPI *OpenVolume)(struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This, EFI_FILE_PROTOCOL **Root);
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

#define EFI_FILE_MODE_READ 0x0000000000000001ULL
#define EFI_LOADER_DATA 2

/* ------------------------------------------------------------
 * Colors: attribute = text color | (background color << 4)
 * ------------------------------------------------------------ */
#define COLOR_DEFAULT 0x07 /* light gray on black */
#define COLOR_BANNER 0x1F  /* white on blue       */
#define COLOR_GREEN 0x0A
#define COLOR_RED 0x0C
#define COLOR_YELLOW 0x0E
#define COLOR_WHITE 0x0F

#endif /* HARIS_UEFI_H */
