/* AMD Renoir GPU compositor, M4a: many draws in one submission. A frame is built from a few fixed shaders that take their parameters
 * in user SGPRs (so the shader code is written to VRAM once, not per draw):
 *   fill    a solid colour rectangle
 *   image   a rectangle copied from a surface (an 8-8-8-8 picture in VRAM), with an opacity
 *   shadow  a soft black shadow (alpha falling to 0 over the outer r pixels), blended "over" what is there
 * The frame builders are pure functions (no hardware access), so tests/test_amd_3d.c walks the stream and the shaders. */
#ifndef HARIS_AMD_COMP_H
#define HARIS_AMD_COMP_H

#include "gpu_types.h"

/* A picture in VRAM: linear, 8-8-8-8, the same byte order as the target. */
typedef struct
{
    uint64_t mc;        /* GPU (MC) address, 256-byte aligned */
    uint32_t off;       /* the same place as a VRAM byte offset (for the CPU) */
    uint32_t w, h, pitch_px;
} AmdSurface;

/* A frame being built. */
typedef struct
{
    uint32_t *ib;
    uint32_t cap, n;
    int over;
    uint64_t rt_mc;
    uint32_t pitch_px, width, height;
    int bgr;
    uint64_t vs_mc, ps_fill_mc, ps_image_mc, ps_shadow_mc;  /* the shaders (amd_comp_build_*), 256-byte aligned */
    uint32_t draws;
} AmdCompFrame;

/* The fixed shaders. Each returns its number of dwords; code must hold AMD_COMP_SHADER_MAX. */
#define AMD_COMP_SHADER_MAX 48
uint32_t amd_comp_build_vs(uint32_t *code);         /* rectangle from user SGPRs s0..s3 = x0, y0, x1, y1 (floats) */
uint32_t amd_comp_build_ps_fill(uint32_t *code);    /* s0 = (r, g), s1 = (b, a) as halves */
uint32_t amd_comp_build_ps_image(uint32_t *code);   /* s0..s7 = image resource, s8 / s9 = surface x / y minus screen x / y, s10 = opacity */
uint32_t amd_comp_build_ps_shadow(uint32_t *code);  /* s0..s3 = x0, y0, x1, y1, s4 = 1 / r, s5 = alpha (floats) */

/* Building a frame: begin (the target), any number of fills / images / shadows (painted in that order), end. end returns the number
 * of dwords, 0 if the stream did not fit. sync_mc: 4 bytes in VRAM for the end-of-frame colour buffer sync. */
void amd_comp_begin(AmdCompFrame *f, uint32_t *ib, uint32_t cap, uint64_t rt_mc, uint32_t pitch_px, uint32_t width, uint32_t height,
                    int bgr, uint64_t shader_mc);
void amd_comp_fill(AmdCompFrame *f, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t r, uint32_t g, uint32_t b);
/* the surface's pixel (sx, sy) lands on (x0, y0); opacity 0 .. 255 (255: plain copy) */
void amd_comp_image(AmdCompFrame *f, const AmdSurface *s, uint32_t sx, uint32_t sy, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1,
                    uint32_t opacity, int bgr);
void amd_comp_shadow(AmdCompFrame *f, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t r, uint32_t alpha);
uint32_t amd_comp_end(AmdCompFrame *f, uint64_t sync_mc);
/* The shader offsets inside the 4 KiB at shader_mc that amd_comp_begin() assumes. */
#define AMD_COMP_VS_OFF 0x000u
#define AMD_COMP_PS_FILL_OFF 0x200u
#define AMD_COMP_PS_IMAGE_OFF 0x400u
#define AMD_COMP_PS_SHADOW_OFF 0x600u

/* M4a test: three "windows" (surfaces the CPU fills) with soft shadows over a background, the third half transparent, composited in
 * ONE submission into a 512 x 256 buffer in VRAM / into the screen, every pixel checked against a CPU-made reference. 1 = right.
 * stage 1: only the background, 2: and the shadows, 3: and the windows. via_frame_ib 0: through the 4 KiB IB page of the 3D tests,
 * 1: through the frame IB by its GART address (hung the CP on the laptop), 2: the frame IB by its VRAM MC address. */
int amd_comp_test_offscreen(int stage, int via_frame_ib);
int amd_comp_test_screen(uint64_t fb_phys, uint32_t pitch_bytes, uint32_t width, uint32_t height, uint64_t vram_cpu, int bgr,
                         int via_frame_ib);
const char *amd_comp_status(void);
#define AMD_COMP_V_IN_GOOD 0     /* pixels of the scene that are right */
#define AMD_COMP_V_IN_TOTAL 1
#define AMD_COMP_V_OUT_BAD 2     /* pixels around the scene that changed */
#define AMD_COMP_V_OUT_TOTAL 3
#define AMD_COMP_V_FIRST_IN 4    /* the first wrong pixel, | 0x01000000; then its expected value (5) and its x | y << 16 (6) */
#define AMD_COMP_V_DWORDS 7      /* dwords of the frame stream */
#define AMD_COMP_V_DRAWS 8       /* draws in it */
#define AMD_COMP_V_CYCLES 9      /* CPU cycles from the submission to the fence (the GPU's frame time) */
#define AMD_COMP_V_FAULT_GC 10   /* after a failure: VM fault GC status, address lo, hi (10 .. 12) */
#define AMD_COMP_V_GRBM 13       /* after a fence timeout: GRBM_STATUS */
#define AMD_COMP_VALUES 14
uint32_t amd_comp_value(int what);

#endif
