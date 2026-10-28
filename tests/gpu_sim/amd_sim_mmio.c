#include "gpu_types.h"
#include "gpu_mmio.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define MMIO_SIZE 0x80000u
#define VRAM_SIZE (512u * 1024u * 1024u)
static uint8_t mmio[MMIO_SIZE];
static uint8_t *vram;
static int fail_writes;
static int fail_after = -1;
static int successful_mmio_writes;
static int fail_after_once;

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static void wr32(uint8_t *p, uint32_t v) {
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
}
int gpu_mmio_can_access(uint64_t base) { return base == 0xFE400000ULL || base == 0x10000000ULL; }
volatile uint8_t *gpu_mmio_ptr(uint64_t base) { return (volatile uint8_t *)(base == 0xFE400000ULL ? mmio : vram); }
uint32_t gpu_mmio_read32(uint64_t base, uint32_t off) {
    uint8_t *b = (uint8_t *)(base == 0xFE400000ULL ? mmio : vram);
    if (base == 0xFE400000ULL && off + 4 > MMIO_SIZE) return 0xFFFFFFFFu;
    if (base == 0x10000000ULL && off + 4 > VRAM_SIZE) return 0xFFFFFFFFu;
    return rd32(b + off);
}
uint16_t gpu_mmio_read16(uint64_t base, uint32_t off) { return (uint16_t)gpu_mmio_read32(base, off); }
uint8_t gpu_mmio_read8(uint64_t base, uint32_t off) { return (uint8_t)gpu_mmio_read32(base, off); }
void gpu_mmio_write32(uint64_t base, uint32_t off, uint32_t value) {
    if (fail_writes) return;
    if (fail_after >= 0 && successful_mmio_writes >= fail_after && !fail_after_once) { fail_after_once = 1; return; }
    uint8_t *b = (uint8_t *)(base == 0xFE400000ULL ? mmio : vram);
    if (base == 0xFE400000ULL && off + 4 <= MMIO_SIZE) { wr32(b + off, value); successful_mmio_writes++; }
    else if (base == 0x10000000ULL && off + 4 <= VRAM_SIZE) { wr32(b + off, value); }
}

static void putreg(uint32_t off, uint32_t v) { wr32(mmio + off, v); }
void sim_init(void) {
    if (vram) free(vram);
    vram = (uint8_t *)calloc(1, VRAM_SIZE);
    fail_writes = 0;
    fail_after = -1;
    successful_mmio_writes = 0;
    fail_after_once = 0;
    memset(mmio, 0, sizeof(mmio));
    /* V86-like Renoir state: 512 MiB VRAM, aperture covers it, VMID0 off.
       Register locations below match the offsets used by amd_native.c. */
    putreg((0x1A000u + 0x0680u) << 2, 0x00080602u); /* MM VM L2 */
    putreg((0x1A000u + 0x06C0u) << 2, 0x007FFE80u); /* MM VM CTX0 */
    putreg((0x1A000u + 0x072Bu) << 2, 0x00000000u); /* MM PT base LO */
    putreg((0x1A000u + 0x072Cu) << 2, 0x00000000u); /* MM PT base HI */

    /* System aperture: 0 .. 512 MiB-1, using the same 256 KiB units
       that amd_native_gart_enable() checks. */
    putreg((0x1A000u + 0x0831u) << 2, 0x00000000u); /* MM SYS AP LOW */
    putreg((0x1A000u + 0x0832u) << 2, 0x000007FFu); /* MM SYS AP HIGH */

    /* 512 MiB VRAM: FB base/top are in 16 MiB units. */
    putreg((0x1A000u + 0x082Cu) << 2, 0x00000000u); /* MM FB LOCATION BASE */
    putreg((0x1A000u + 0x082Du) << 2, 0x0000001Fu); /* MM FB LOCATION TOP */
    putreg((0x1A000u + 0x0817u) << 2, 0x00000410u); /* MM FB OFFSET / MC_VM_FB_OFFSET */

    /* GC mirror of the framebuffer range/offset. */
    putreg((0x2000u + 0x0980u) << 2, 0x00000000u); /* GC FB LOCATION BASE */
    putreg((0x2000u + 0x0981u) << 2, 0x0000001Fu); /* GC FB LOCATION TOP */
    putreg((0x2000u + 0x096Bu) << 2, 0x00000410u); /* GC FB OFFSET / MC_VM_FB_OFFSET */
    putreg((0x2000u + 0x08ECu) << 2, 0x00000000u); /* GC PT BASE HI */

    /* invalidate ACKs start clear; model hardware ACK immediately on request */
    putreg((0x2000u + 0x08C6u) << 2, 1u);
    putreg((0x1A000u + 0x0706u) << 2, 1u);
}
void sim_fail_writes(int on) { fail_writes = on; }
void sim_fail_after(int n) { fail_after = n; }
