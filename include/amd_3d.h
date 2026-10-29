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
    /* M2: a textured rectangle. The pixel shader reads texel (x - x0, y - y0) of this image (linear, 8-8-8-8, same byte order as the
     * colour buffer) and writes it; the colour fields above are then not used. */
    uint32_t textured;
    uint64_t tex_mc;       /* the image, 256-byte aligned */
    uint32_t tex_off;      /* the same place as a VRAM byte offset (for the CPU) */
    uint32_t tex_pitch_px, tex_w, tex_h;
    uint64_t marker_mc;    /* 0, or the address of 8 bytes the stream writes a marker to before and after the draw (see amd_3d.c) */
    /* M3: 1 = "over" alpha blending: result = colour * a / 255 + what is in the colour buffer * (255 - a) / 255 */
    uint32_t blend;
} Amd3dJob;

#define AMD_3D_IB_MAX 600 /* dwords amd_3d_build_ib() can write at most */
#define AMD_3D_VS_MAX 48  /* dwords of the vertex / pixel shader the builders write at most */
#define AMD_3D_PS_MAX 32

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

/* On the GPU. offscreen: a 256 x 64 buffer in VRAM that nothing shows; checks every pixel afterwards. screen: a rectangle in the
 * real framebuffer (fb_phys is its CPU address, vram_cpu the CPU address of the VRAM aperture). 1 = the pixels are right. */
/* n tiny throw-away solid draws (8 x 8, a new colour each) right after bring-up: the first ~20-24 draws of a boot were randomly lost on
 * the real laptop. Returns how many were right. bits[k] bit i = draw 32k + i right; info[0] = index of the last failed draw + 1
 * (0: none), info[1] = failed draws, info[2] = CPU cycles / 2^20 for all of them, info[3] = CPU cycles / 2^20 to the last failed one. */
#define AMD_3D_WARMUP_WORDS 12
/* vs_store = 1: the vertex shader also stores what it computed (see amd_3d_warmup_vs); it hung the GPU once, so run it last. */
int amd_3d_warmup(int n, uint32_t bits[AMD_3D_WARMUP_WORDS], uint32_t info[4], int vs_store);
/* Pipeline statistics of the warm-up draws (IA vertices, IA primitives, VS, C invocations, C primitives, PS invocations, per draw):
 * 0 .. 5 the first right draw, 6 .. 11 the first lost one, 12 .. 17 lost draws with that counter 0, 18 draws whose samples were not
 * written, 19 .. 24 right draws with that counter 0. */
uint32_t amd_3d_warmup_stat(int k);
/* What the warm-up's vertex shader stored per vertex (diagnostic): 0 .. 11 the first right draw (slot 0 .. 3: v0, x, y), 12 .. 23 the
 * first lost one, 24 .. 26 lost draws with a slot not written / a wrong v0 / a wrong position, 27 .. 29 the same for right draws,
 * 30 .. 33 VM fault GC status / GC address lo / hi / MM status after the last draw, 34 = 1 if the ring still ran afterwards. */
uint32_t amd_3d_warmup_vs(int k);
/* One tiny draw whose vertex shader stores what it computed: target 0 = system-RAM scratch through the GART, 1 = VRAM. out[0 .. 11]
 * slot 0 .. 3: v0, x, y; [12] draw right; [13 .. 16] VM fault GC status / lo / hi / MM status; [17] ring alive; [18], [19] address. */
int amd_3d_vs_store_probe(int target, uint32_t out[20]);
/* M3: the rectangle "over"-blended (alpha 128) on a background; the screen one white, alpha 64, just above the solid one. */
int amd_3d_blend_offscreen(void);
int amd_3d_blend_screen(uint64_t fb_phys, uint32_t pitch_bytes, uint32_t width, uint32_t height, uint64_t vram_cpu, int bgr);
/* "Over" blending of one pixel byte by byte (0x00RRGGBB): src * a / 255 + dst * (255 - a) / 255, rounded. */
uint32_t amd_3d_blend_px(uint32_t src, uint32_t dst, uint32_t a);
int amd_3d_rect_offscreen(void);
int amd_3d_rect_screen(uint64_t fb_phys, uint32_t pitch_bytes, uint32_t width, uint32_t height, uint64_t vram_cpu, int bgr);
/* The same two, with the rectangle textured from an image in VRAM (M2). */
int amd_3d_tex_offscreen(void);
int amd_3d_tex_screen(uint64_t fb_phys, uint32_t pitch_bytes, uint32_t width, uint32_t height, uint64_t vram_cpu, int bgr);

/* Eight solid draws in each of three modes (shader code written once / every time / every time with a wait), bit i of bits[m] = try i was right; bit 16+i = it failed but the rectangle was complete 20 ms later. */
void amd_3d_stress(uint32_t bits[3]);
/* After amd_3d_stress(): of the failed draws of a mode, how many had which marker state; nibble n counts state n (0 neither marker,
 * 1 only the one before the draw, 2 only the one after, 3 both). */
uint32_t amd_3d_stress_combo(int mode); /* mode 3 = D, 4 = E */
/* After amd_3d_stress(), mode D (pattern written once, colour changes every draw; k = 0 .. 8) and E (same, pattern before every draw; k = 9 .. 17):
 * k = 0 / 9 bit i = try i right; k = 1 + i / 10 + i the first wrong inside pixel of try i (bit 24 set; 0 if right). */
uint32_t amd_3d_stress_d(int k);
/* After amd_3d_stress(), modes 0..2: low byte = failed draws that showed the whole rectangle after the CPU read 1 MiB of other video memory, next byte = after 16 MiB. */
uint32_t amd_3d_stress_fix(int mode);
/* After amd_3d_stress(): k = 0 .. 2 bit n = draw n (run order, 96 draws) was right; 3 = TSC/2^20 at the start; 4 / 5 = TSC/2^20 from the
 * start to the first / last failed draw (0xFFFFFFFF: none); 6 = TSC/2^20 for the whole test; 7 .. 11, 12 .. 16, 17 .. 21 = RLC_STAT, RLC_GPM_STAT,
 * RLC_PG_CNTL, RLC_CGCG_CGLS_CTRL, GRBM_STATUS2 at the start, at the first failed draw and at the end; 22 = TSC/2^20 of the idle wait;
 * 23 = index of the last failed draw. */
uint32_t amd_3d_stress_info(int k);
const char *amd_3d_status(void);
/* After amd_3d_rect_offscreen(): row 0 .. 15 of a 32 x 16 picture of the 256 x 64 buffer (# colour, . untouched, ? other), 0 beyond. */
const char *amd_3d_map_row(int row);
/* 0 pixels inside right, 1 pixels inside, 2 pixels outside changed, 3 pixels outside, 4 first wrong inside, 5 first wrong outside,
 * 6 draw IB dwords, 7 fence, 8 stage reached, 9 .. 15 registers read back after the draw (see amd_3d.c), 16 .. 23 the pixel shader's user SGPR registers, 24/25 its RSRC1/RSRC2, 26 right inside pixels 20 ms after a failed draw, 27 tries (+0x100 if the last passed), 32/33 the markers before/after the draw (0x3D3D0001 / 0x3D3D0002 if reached), 28 .. 31 VM fault registers read after a failed first try (GC status, address lo, hi; MM status) */
uint32_t amd_3d_value(int what);
#define AMD_3D_VALUES 40

#endif
