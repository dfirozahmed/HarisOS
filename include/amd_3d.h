/* AMD Renoir (GFX9) 3D pipeline: rectangles drawn by the graphics pipeline (vertex shader, rasteriser, pixel shader, colour buffer)
 * instead of by a CP DMA fill. M1 a solid colour, M2 an image (IMAGE_LOAD), M3 alpha blending, a soft shadow and a box blur.
 * Needs the persistent ring of the CP tests (amd_native.h) and VMID0.
 *
 * The command stream and the shaders are built by pure functions (no hardware access), so the host test (tests/test_amd_3d.c) can
 * walk them packet by packet and instruction by instruction. Where every register value comes from is written next to it in amd_3d.c. */
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
    /* M2: a textured rectangle. The pixel shader reads texel (x - x0, y - y0) of this image (linear, 8-8-8-8, same byte order as the
     * colour buffer) and writes it; the colour fields above are then not used. */
    uint32_t textured;
    uint64_t tex_mc;       /* the image, 256-byte aligned */
    uint32_t tex_off;      /* the same place as a VRAM byte offset (for the CPU) */
    uint32_t tex_pitch_px, tex_w, tex_h;
    uint64_t sync_mc;      /* 0, or 4 bytes the end-of-draw colour buffer sync writes and waits for (see cb_sync in amd_3d.c) */
    /* M3: 1 = "over" alpha blending: result = colour * a / 255 + what is in the colour buffer * (255 - a) / 255 */
    uint32_t blend;
    /* M3: a soft shadow (needs blend = 1): the rectangle is the whole shadow, its alpha a / 255 inside, falling linearly to 0 over
     * the outer shadow_r pixels of every edge (both axes multiplied, so the corners are round-ish). 0 = no shadow. */
    uint32_t shadow_r;
    /* M3: a box blur (needs textured = 1): every pixel is the average of the (2 blur_r + 1)^2 texels around its own one. The image is
     * 2 blur_r bigger than the rectangle each way (tex_w = x1 - x0 + 2 blur_r ...), so no read falls outside it. 0 = no blur. */
    uint32_t blur_r;
} Amd3dJob;

#define AMD_3D_IB_MAX 600 /* dwords amd_3d_build_ib() can write at most */
#define AMD_3D_VS_MAX 48  /* dwords of the vertex / pixel shader the builders write at most */
#define AMD_3D_PS_MAX 128 /* the blur shader: 9 texel reads */

/* Shader code. Return the number of dwords written. */
uint32_t amd_3d_build_vs(uint32_t *code, const Amd3dJob *job);
uint32_t amd_3d_build_ps(uint32_t *code, const Amd3dJob *job);
/* The 8 dwords of the image resource (T#) for job->tex_*; they go to the pixel shader in its user SGPRs s0 .. s7. */
void amd_3d_build_tex_desc(uint32_t desc[8], const Amd3dJob *job);
/* The colour of texel (x, y) of the test image, as a memory dword of the colour buffer format (0x00RRGGBB if job->bgr). */
uint32_t amd_3d_texel(const Amd3dJob *job, uint32_t x, uint32_t y);
/* The default context state of the GPU (Linux clear-state table), a stream of its own that has to run once before the draw. Returns the
 * number of dwords, 0 if it does not fit in cap. */
#define AMD_3D_DEFAULT_IB_MAX 1000
uint32_t amd_3d_build_default_ib(uint32_t *ib, uint32_t cap);
/* The packets Linux gfx_v9_0_cp_gfx_start() writes into the ring once (PREAMBLE_CNTL, clear state, CLEAR_STATE, SET_BASE,
 * VGT_INDEX_TYPE). Returns the number of dwords, 0 if cap is too small. amd_3d_ring_start() sends them through the ring: 1 = done. */
uint32_t amd_3d_build_ring_start(uint32_t *p, uint32_t cap);
int amd_3d_ring_start(void);

/* The whole command stream (state, shaders' addresses, the draw). Returns the number of dwords, 0 if it does not fit in cap. */
uint32_t amd_3d_build_ib(uint32_t *ib, uint32_t cap, const Amd3dJob *job);

/* On the GPU. offscreen: a 256 x 64 buffer in VRAM that nothing shows; screen: a rectangle in the real framebuffer (fb_phys is its CPU
 * address, vram_cpu the CPU address of the VRAM aperture). Every pixel is checked afterwards; 1 = all right.
 *   M1 rect     a solid rectangle                         M2 tex     the rectangle textured from an image
 *   M3 blend    "over" alpha blending on a background     M3 shadow  a soft black shadow (alpha from the distance to the edges)
 *   M3 blur     a 3 x 3 box blur of a checkerboard image */
int amd_3d_rect_offscreen(void);
int amd_3d_rect_screen(uint64_t fb_phys, uint32_t pitch_bytes, uint32_t width, uint32_t height, uint64_t vram_cpu, int bgr);
int amd_3d_tex_offscreen(void);
int amd_3d_tex_screen(uint64_t fb_phys, uint32_t pitch_bytes, uint32_t width, uint32_t height, uint64_t vram_cpu, int bgr);
int amd_3d_blend_offscreen(void);
int amd_3d_blend_screen(uint64_t fb_phys, uint32_t pitch_bytes, uint32_t width, uint32_t height, uint64_t vram_cpu, int bgr);
int amd_3d_shadow_offscreen(void);
int amd_3d_shadow_screen(uint64_t fb_phys, uint32_t pitch_bytes, uint32_t width, uint32_t height, uint64_t vram_cpu, int bgr);
int amd_3d_blur_offscreen(void);
int amd_3d_blur_screen(uint64_t fb_phys, uint32_t pitch_bytes, uint32_t width, uint32_t height, uint64_t vram_cpu, int bgr);

/* What the checks expect (also used by the host test). "Over" blending of one pixel byte by byte (0x00RRGGBB): src * a / 255 +
 * dst * (255 - a) / 255, rounded. The shadow alpha (0 .. 255) of pixel (x, y), an integer version of the shader formula. The blur
 * of pixel (x, y): per byte the rounded average of the (2 blur_r + 1)^2 texels around it. */
uint32_t amd_3d_blend_px(uint32_t src, uint32_t dst, uint32_t a);
uint32_t amd_3d_shadow_alpha(const Amd3dJob *job, uint32_t x, uint32_t y);
uint32_t amd_3d_blur_px(const Amd3dJob *job, uint32_t x, uint32_t y);

const char *amd_3d_status(void);
/* After an offscreen test: row 0 .. 15 of a 32 x 16 picture of the 256 x 64 buffer (# drawn, . untouched, ? other), 0 beyond. */
const char *amd_3d_map_row(int row);
/* What the last test measured: */
#define AMD_3D_V_IN_GOOD 0      /* pixels inside that are right */
#define AMD_3D_V_IN_TOTAL 1     /* pixels inside */
#define AMD_3D_V_OUT_BAD 2      /* pixels outside that changed */
#define AMD_3D_V_OUT_TOTAL 3    /* pixels outside looked at */
#define AMD_3D_V_FIRST_IN 4     /* the first wrong pixel inside, | 0x01000000 (0: none) */
#define AMD_3D_V_FIRST_OUT 5    /* the first changed pixel outside, | 0x01000000 (0: none) */
#define AMD_3D_V_DWORDS 6       /* dwords of the draw stream */
#define AMD_3D_V_FENCE 7        /* the last fence number */
#define AMD_3D_V_STAGE 8        /* how far it got: 1 pattern, 2 shaders, 3 default state, 4 draw, 5 checked */
#define AMD_3D_V_GRBM 9         /* GRBM_STATUS after the draw */
#define AMD_3D_V_FAULT_GC 10    /* after a failure: VM fault GC status, GC address lo, hi (10 .. 12), MM status (13) */
#define AMD_3D_VALUES 14
uint32_t amd_3d_value(int what);

#endif
