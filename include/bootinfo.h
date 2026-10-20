/* ============================================================
 * HarisOS - boot information handed from the firmware-specific entry
 * code (boot/efi_entry.c) to the kernel.
 *
 * This header is intentionally UEFI-independent: the kernel only sees
 * these plain structs and never includes uefi.h.
 * ============================================================ */
#ifndef HARIS_BOOTINFO_H
#define HARIS_BOOTINFO_H

/* Same binary layout as EFI_MEMORY_DESCRIPTOR. */
typedef struct
{
    unsigned int type;
    unsigned int pad;
    unsigned long long phys_start;
    unsigned long long virt_start;
    unsigned long long pages; /* 4 KiB pages */
    unsigned long long attr;
} BootMemDesc;

/* Memory types (values come from the UEFI spec). Only CONVENTIONAL is treated
 * as free: the firmware stack and our own image/boot info live in the other
 * types, so we must not hand them out. */
#define BOOT_MEM_LOADER_CODE 1
#define BOOT_MEM_LOADER_DATA 2
#define BOOT_MEM_BS_CODE 3
#define BOOT_MEM_BS_DATA 4
#define BOOT_MEM_CONVENTIONAL 7

#define BOOT_RESET_COLD 0     /* restart */
#define BOOT_RESET_SHUTDOWN 2 /* power off */

typedef struct
{
    /* Memory map. Walk it with: (const char *)memmap + i * desc_size.
     * desc_size can be larger than sizeof(BootMemDesc)! */
    const void *memmap;
    unsigned long long memmap_size; /* bytes actually used */
    unsigned long long desc_size;   /* stride between entries */
    unsigned int desc_version;

    /* Framebuffer */
    void *framebuffer;
    unsigned int width;
    unsigned int height;
    unsigned int pitch; /* in pixels */
    unsigned int red_mask_mode;

    /* The firmware's reset service (UEFI ResetSystem). Works after boot services are gone.
     * type: BOOT_RESET_COLD (restart) or BOOT_RESET_SHUTDOWN. May be NULL; does not return when it works. */
    void (*reset_system)(unsigned type);

    /* Physical address of the ACPI RSDP, 0 if the firmware did not give one. */
    unsigned long long acpi_rsdp;
} BootInfo;

#endif
