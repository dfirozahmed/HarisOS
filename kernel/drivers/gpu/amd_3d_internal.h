/* Shared between amd_3d.c (the 3D tests M1..M3) and amd_comp.c (the M4 compositor): the command stream writer and the three pieces a
 * graphics stream is made of. amd_3d_build_ib() is head + one draw + tail; a compositor frame is head + many draws + tail. */
#ifndef HARIS_AMD_3D_INTERNAL_H
#define HARIS_AMD_3D_INTERNAL_H

#include "gpu_types.h"

/* A command stream being written: p[0 .. cap-1]; n counts on past cap, and over is set then (the caller checks it). */
typedef struct
{
    uint32_t *p;
    uint32_t n, cap;
    int over;
} Ib;

/* One draw: a rectangle (window space pixels, x0 <= x < x1) that the vertex shader turns into 4 vertices and the scissor clips to.
 * The shaders' programs and their user SGPR values (SET_SH_REG to SPI_SHADER_USER_DATA_PS_0 / _VS_0). */
typedef struct
{
    uint32_t x0, y0, x1, y1;
    int blend;                  /* 1: "over" alpha blending (SRC_ALPHA / ONE_MINUS_SRC_ALPHA) */
    uint64_t ps_mc;
    int ps_pos;                 /* 1: the pixel shader gets the pixel position in v2 / v3 (POS_X/Y_FLOAT_ENA) */
    uint32_t ps_vgprs, ps_sgprs; /* SPI_SHADER_PGM_RSRC1_PS fields: (VGPRs / 4) - 1, (SGPRs / 8) - 1 */
    uint32_t ps_user_n;         /* 0 .. 16 */
    const uint32_t *ps_user;
    uint64_t vs_mc;
    uint32_t vs_vgprs;          /* SPI_SHADER_PGM_RSRC1_VS VGPRS field */
    uint32_t vs_user_n;         /* 0 .. 16 */
    const uint32_t *vs_user;
} A3dDraw;

/* Everything that depends only on the colour buffer (rt_mc, pitch, size, byte order): cache sync, GFX9 start-up registers, the colour
 * buffer, guard band, colour output, the fixed vertex shader registers. */
void a3d_emit_head(Ib *b, uint64_t rt_mc, uint32_t pitch_px, uint32_t width, uint32_t height, int bgr);
/* One draw (blend state, scissor, shaders, user data, DRAW_INDEX_AUTO). */
void a3d_emit_draw(Ib *b, const A3dDraw *d);
/* The end: with sync_mc, flush the colour buffer and wait for it (so the fence after the stream means "pixels in memory"). */
void a3d_emit_tail(Ib *b, uint64_t sync_mc);

/* Runs the default context state stream (the Linux clear-state table) through the ring. 0 = done. */
int a3d_default_state(void);

#endif
