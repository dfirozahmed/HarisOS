/* AMD Renoir (GFX9) 3D pipeline, first step (M1): one coloured rectangle drawn by the graphics pipeline (vertex shader, rasteriser,
 * pixel shader, colour buffer) instead of by a CP DMA fill. Needs the persistent ring of the CP tests (amd_native.h) and VMID0.
 *
 * The command stream is built by amd_3d_build_ib() with no hardware access, so the host test can walk it packet by packet.
 * Where every register value comes from is written next to it in amd_3d.c. */
#ifndef HARIS_AMD_3D_H
#define HARIS_AMD_3D_H

#include "gpu_types.h"

/* What to draw and where. All addresses are GPU (MC) addresses. */
typedef struct
{
    uint64_t rt_mc;       /* colour buffer, 256-byte aligned, linear, 8-8-8-8 */
    uint32_t pitch_px;    /* pixels per row of the colour buffer */
    uint32_t width, height; /* size of the colour buffer in pixels */
    int bgr;              /* 1: memory order B,G,R,X (UEFI 0x00RRGGBB), 0: R,G,B,X */
    uint32_t x0, y0, x1, y1; /* the rectangle, pixels: x0 <= x < x1, y0 <= y < y1 */
    uint32_t r, g, b, a;  /* colour, 0 .. 255 */
    uint64_t vs_mc, ps_mc; /* the shader code, 256-byte aligned */
} Amd3dJob;

#define AMD_3D_IB_MAX 600 /* dwords amd_3d_build_ib() can write at most */
#define AMD_3D_VS_MAX 32  /* dwords of the vertex / pixel shader the builders write at most */
#define AMD_3D_PS_MAX 16

/* Shader code. Return the number of dwords written. */
uint32_t amd_3d_build_vs(uint32_t *code, const Amd3dJob *job);
uint32_t amd_3d_build_ps(uint32_t *code, const Amd3dJob *job);
/* The default context state of the GPU (Linux clear-state table), a stream of its own that has to run once before the draw. Returns the
 * number of dwords, 0 if it does not fit in cap. */
#define AMD_3D_DEFAULT_IB_MAX 1000
uint32_t amd_3d_build_default_ib(uint32_t *ib, uint32_t cap);

/* The whole command stream (state, shaders' addresses, the draw). Returns the number of dwords, 0 if it does not fit in cap. */
uint32_t amd_3d_build_ib(uint32_t *ib, uint32_t cap, const Amd3dJob *job);

/* On the GPU. offscreen: a 256 x 64 buffer in VRAM that nothing shows; checks every pixel afterwards. screen: a rectangle in the
 * real framebuffer (fb_phys is its CPU address, vram_cpu the CPU address of the VRAM aperture). 1 = the pixels are right. */
int amd_3d_rect_offscreen(void);
int amd_3d_rect_screen(uint64_t fb_phys, uint32_t pitch_bytes, uint32_t width, uint32_t height, uint64_t vram_cpu, int bgr);

const char *amd_3d_status(void);
/* 0 pixels inside right, 1 pixels inside, 2 pixels outside changed, 3 pixels outside, 4 first wrong inside, 5 first wrong outside,
 * 6 draw IB dwords, 7 fence, 8 stage reached, 9 .. 15 registers read back after the draw (see amd_3d.c) */
uint32_t amd_3d_value(int what);
#define AMD_3D_VALUES 16

#endif
