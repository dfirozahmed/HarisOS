/* Host test seams for amd_native.c (it is compiled unchanged).
 *
 * amd_native.c asks the rest of the kernel for: the PSP state (amd_psp.c), the staged firmware images (amd_firmware.c) and
 * kernel heap blocks (heap.c). The real ones need the serial port, the drives and the page allocator, so the test supplies
 * simple stand-ins. kmalloc has one rule that matters: the GPU is given 32-bit addresses of heap blocks (GPU_DMA_LIMIT in
 * gpu_types.h) and amd_native.c refuses blocks above 4 GiB, so the stand-in hands out memory from a fixed arena BELOW 4 GiB. */
#include "gpu_types.h"
#include "amd_psp.h"
#include "amd_firmware.h"
#include "heap.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

/* ---- PSP: pretend it is active, like the V86 hardware snapshot ---- */
int amd_psp_active(void)
{
    return 1;
}

/* ---- firmware: none staged ---- */
int amd_firmware_count(void)
{
    return 0;
}

int amd_firmware_staged(void)
{
    return 0;
}

const AmdFirmwareEntry *amd_firmware_get(int index)
{
    (void)index;
    return (const AmdFirmwareEntry *)0;
}

/* ---- heap: a bump allocator in an arena below 4 GiB; kfree gives nothing back (the tests are short) ---- */
#define ARENA_BYTES (16u * 1024u * 1024u)
static unsigned char *arena;
static unsigned long long arena_used;

/* 1 if the arena below 4 GiB could be reserved; the tests skip the system-RAM page table cases otherwise */
int sim_heap_ready(void)
{
    if (arena)
        return 1;
#ifdef _WIN32
    /* try fixed low addresses: the first free 16 MiB hole between 256 MiB and 2 GiB */
    for (unsigned long long a = 0x10000000ULL; a < 0x80000000ULL; a += 0x01000000ULL) {
        void *p = VirtualAlloc((void *)a, ARENA_BYTES, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (p) {
            arena = (unsigned char *)p;
            break;
        }
    }
#else
    void *p = mmap(0, ARENA_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (p != MAP_FAILED)
        arena = (unsigned char *)p;
#endif
    arena_used = 0;
    return arena != 0 && (unsigned long long)arena + ARENA_BYTES <= GPU_DMA_LIMIT;
}

/* 0 = normal, 1 = out of memory, 2 = every block lies above 4 GiB (the test must see amd_native.c refuse it) */
static int heap_mode;

void sim_heap_reset(int mode)
{
    heap_mode = mode;
    arena_used = 0;
}

void *kmalloc(unsigned long long size)
{
    unsigned long long n = (size + 15u) & ~15ULL;
    if (heap_mode == 1)
        return (void *)0;
    if (heap_mode == 2)
        return (void *)(unsigned long long)(0x100000000ULL + (arena_used += n)); /* never dereferenced */
    if (!sim_heap_ready() || arena_used + n > ARENA_BYTES)
        return (void *)0;
    void *p = arena + arena_used;
    arena_used += n;
    return p;
}

void kfree(void *p)
{
    (void)p;
}
