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
    uint64_t vs_mc, ps_fill_mc, ps_image_mc, ps_shadow_mc, ps_layer_mc, ps_under_mc, ps_dim_mc;  /* the shaders (amd_comp_build_*), 256-byte aligned */
    uint32_t draws;
} AmdCompFrame;

/* The fixed shaders. Each returns its number of dwords; code must hold AMD_COMP_SHADER_MAX. */
#define AMD_COMP_SHADER_MAX 48
uint32_t amd_comp_build_vs(uint32_t *code);         /* rectangle from user SGPRs s0..s3 = x0, y0, x1, y1 (floats) */
uint32_t amd_comp_build_ps_fill(uint32_t *code);    /* s0 = (r, g), s1 = (b, a) as halves */
uint32_t amd_comp_build_ps_image(uint32_t *code);   /* s0..s7 = image resource, s8 / s9 = surface x / y minus screen x / y, s10 = opacity */
uint32_t amd_comp_build_ps_shadow(uint32_t *code);  /* s0..s3 = x0, y0, x1, y1, s4 = 1 / r, s5 = alpha (floats) */
uint32_t amd_comp_build_ps_layer(uint32_t *code);   /* s0..s7 = image resource (alpha from the 4th byte), s8 / s9 as for the image */
uint32_t amd_comp_build_ps_under(uint32_t *code);   /* as layer, plus the picture below it s10 rows further down: out = layer + below * alpha */
uint32_t amd_comp_build_ps_dim(uint32_t *code);     /* as image, colour times s10 (the screen's brightness) */

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
/* The shader offsets inside the 4 KiB at shader_mc that amd_comp_begin() assumes (0x200 bytes each). */
#define AMD_COMP_VS_OFF 0x000u
#define AMD_COMP_PS_FILL_OFF 0x200u
#define AMD_COMP_PS_IMAGE_OFF 0x400u
#define AMD_COMP_PS_SHADOW_OFF 0x600u
#define AMD_COMP_PS_LAYER_OFF 0x800u
#define AMD_COMP_PS_UNDER_OFF 0xA00u
#define AMD_COMP_PS_DIM_OFF 0xC00u

/* M4a test: three "windows" (surfaces the CPU fills) with soft shadows over a background, the third half transparent, composited in
 * ONE submission into a 512 x 256 buffer in VRAM / into the screen, every pixel checked against a CPU-made reference. 1 = right.
 * The frame goes through the frame IB (128 KiB of VRAM the CP fetches by its MC address). */
int amd_comp_test_offscreen(void);
int amd_comp_test_screen(uint64_t fb_phys, uint32_t pitch_bytes, uint32_t width, uint32_t height, uint64_t vram_cpu, int bgr);
const char *amd_comp_status(void);

/* M4b: VRAM for the windows' pictures. A first-fit allocator over a range of VRAM byte offsets, in 64 KiB granules (pure: no
 * hardware access, so tests/test_amd_3d.c checks it). Freed neighbours merge again. */
#define AMD_VPOOL_BLOCKS 64
#define AMD_VPOOL_GRAIN 0x10000u
typedef struct { uint32_t off, size, used; } AmdVBlock;
typedef struct { uint32_t n; AmdVBlock b[AMD_VPOOL_BLOCKS]; } AmdVPool;
void amd_vpool_init(AmdVPool *p, uint32_t off, uint32_t size);  /* off is rounded up to a granule and is never 0 */
uint32_t amd_vpool_alloc(AmdVPool *p, uint32_t bytes);           /* the VRAM offset, 0 = no room */
int amd_vpool_free(AmdVPool *p, uint32_t off);                    /* 1 freed, 0 not the start of a used block */
uint32_t amd_vpool_free_bytes(const AmdVPool *p);

/* Surfaces from the compositor's pool (VRAM 16 MiB .. 127 MiB). The pitch is rounded up to 64 pixels (256 bytes). */
int amd_surface_alloc(AmdSurface *s, uint32_t w, uint32_t h);    /* 1 = done */
void amd_surface_free(AmdSurface *s);
/* Copies a w x h rectangle of a RAM picture (src points at its pixel (0, 0), src_pitch_px pixels per row) to (x, y) of the surface.
 * Then amd_gpu_wc_flush() before the GPU reads it. */
void amd_surface_upload(const AmdSurface *s, const uint32_t *src, uint32_t src_pitch_px, uint32_t x, uint32_t y, uint32_t w, uint32_t h);

/* M4b-1 test: a 512 x 512 picture from RAM into a pool surface, timed, then read back word by word. 1 = every word right. */
int amd_comp_upload_test(void);
#define AMD_COMP_U_CYCLES 0      /* CPU cycles of the upload (with the flush) */
#define AMD_COMP_U_BYTES 1
#define AMD_COMP_U_BAD 2         /* words that read back wrong */
#define AMD_COMP_U_OFF 3         /* the surface's VRAM offset */
#define AMD_COMP_U_POOL_FREE 4   /* free pool bytes after it was freed again */
#define AMD_COMP_U_VALUES 5
uint32_t amd_comp_upload_value(int what);

/* A packed gfx layer (gfx_layer_pack: colour premultiplied, 4th byte = how much of what is below shows through): result = colour +
 * below * that / 255 (blend mode 2, ONE / SRC_ALPHA). The surface's pixel (sx, sy) lands on (x0, y0). */
void amd_comp_layer(AmdCompFrame *f, const AmdSurface *s, uint32_t sx, uint32_t sy, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1,
                    int bgr);
/* The same finished in one pass, no blending: the picture below is in the same surface below_dy rows down (amd_comp_build_ps_under). */
/* An image (as amd_comp_image, opaque) with its colour times scale / 256: the screen's brightness. */
void amd_comp_dim(AmdCompFrame *f, const AmdSurface *s, uint32_t sx, uint32_t sy, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1,
                  uint32_t scale, int bgr);
void amd_comp_layer_under(AmdCompFrame *f, const AmdSurface *s, uint32_t sx, uint32_t sy, uint32_t below_dy, uint32_t x0, uint32_t y0,
                          uint32_t x1, uint32_t y1, int bgr);

/* M4b-2: the gfx overlay driver (GfxOverlayOps in gfx.h) that pastes the window being dragged onto the screen. init after the
 * compositor worked on the screen; 1 = ready. upload / draw return 0 when done (draw waits for the GPU), -1 on a failure. */
int amd_comp_overlay_init(uint64_t fb_phys, uint32_t pitch_bytes, uint32_t width, uint32_t height, uint64_t vram_cpu, int bgr);
int amd_comp_overlay_upload(const void *layer, int lw, int lh);
volatile unsigned int *amd_comp_overlay_below(int *pitch);
int amd_comp_overlay_draw(int x, int y, int x0, int y0, int x1, int y1);
#define AMD_COMP_OV_UPLOADS 0
#define AMD_COMP_OV_DRAWS 1
#define AMD_COMP_OV_FAILS 2
#define AMD_COMP_OV_UPLOAD_CYCLES 3  /* the last one */
#define AMD_COMP_OV_DRAW_CYCLES 4    /* the last one, from building the frame to the fence */
#define AMD_COMP_OV_VALUES 5
uint32_t amd_comp_overlay_value(int what);
/* M4b-2 check before the overlay is used (offscreen, amd_comp_value() / amd_comp_status() like the M4a test): 1 = right */
int amd_comp_layer_test(void);
uint32_t amd_comp_layer_px(uint32_t layer, uint32_t below);  /* what blend mode 2 gives for one pixel */
/* The overlay's path as a test (8 rounds of the picture below written fast, then layer + below in one pass): wrong pixels, ~0u = did not run */
uint32_t amd_comp_drag_test(void);
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

/* M4c: the scene compositor, the gfx compositor driver (GfxCompOps in gfx.h). init after the compositor and the overlay worked on
 * the screen (1 = ready); then amd_comp_scene_test() (offscreen, amd_comp_value() / amd_comp_status() as above) before it is used. */
#include "gfx.h"
int amd_comp_scene_init(uint64_t fb_phys, uint32_t pitch_bytes, uint32_t width, uint32_t height, uint64_t vram_cpu, int bgr);
int amd_comp_scene_test(void);
int amd_comp_surface_new(int w, int h);
void amd_comp_surface_free(int id);
int amd_comp_surface_upload(int id, const void *px, int pitch, int x, int y, int w, int h, int opaque);
int amd_comp_compose(const GfxCompItem *items, int n, const int *rects, int nr);
int amd_comp_surface_scroll(int id, int x, int y, int w, int h, int dy); /* GfxCompOps.scroll */
void amd_comp_set_dim(int scale);    /* GfxCompOps.dim: the screen's brightness, 0 .. 256 */
int amd_comp_scroll_test(void);       /* offscreen check before scroll is used: 1 = right */
#define AMD_COMP_SC_FRAMES 0     /* composes done */
#define AMD_COMP_SC_FAILS 1
#define AMD_COMP_SC_UPLOADS 2
#define AMD_COMP_SC_DRAWS 3      /* the last compose: draws into the back buffer */
#define AMD_COMP_SC_DWORDS 4     /* its stream */
#define AMD_COMP_SC_CYCLES 5     /* CPU cycles from building it to the fence */
#define AMD_COMP_SC_SCROLLS 6    /* rows moved inside surfaces (amd_comp_surface_scroll) */
#define AMD_COMP_SC_VALUES 7
uint32_t amd_comp_scene_value(int what);
/* Sums over the composes since the last call (then they start again from 0): for the desktop's report. */
typedef struct
{
    uint32_t composes;
    uint64_t cycles, cycles_max; /* CPU cycles from building a compose to its fence */
    uint64_t pixels;             /* of the rectangles put together */
    uint64_t draws;
    uint64_t upload_bytes;
} AmdSceneSums;
void amd_comp_scene_sums(AmdSceneSums *out);

#endif
