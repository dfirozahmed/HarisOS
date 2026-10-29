/* AMD Renoir GPU compositor, M4a: a frame of many draws in one submission, with fixed shaders that take their parameters in user
 * SGPRs (see include/amd_comp.h). The command stream comes from the same pieces as the 3D tests (amd_3d_internal.h: head, draws,
 * tail), so every register value and where it comes from is in amd_3d.c.
 *
 * GFX9 shader rule kept in every shader here: at most ONE scalar operand (an SGPR, a literal, or the VCC that V_CNDMASK reads) per
 * VALU instruction (Mesa aco_validate.cpp, const_bus_limit 1); an SGPR needed next to VCC is copied into a VGPR first. */
#include "amd_comp.h"
#include "amd_3d.h"
#include "amd_3d_isa.h"
#include "amd_3d_internal.h"
#include "amd_native.h"

#define SGPR(n) ((uint32_t)(n)) /* source operand code of SGPR n */

/* ---- the fixed shaders ---- */

/* The rectangle from s0..s3 (x0, y0, x1, y1 as floats), a triangle strip of 4 vertices: bit 0 of the vertex number picks x1, bit 1
 * picks y1. Window space position (pixels), like the 3D tests' vertex shader. */
uint32_t amd_comp_build_vs(uint32_t *code)
{
    uint32_t n = 0;
    code[n++] = a3d_vop2(A3D_OP2_AND_B32, 1, 0, A3D_SRC_INT(1));                 /* v_and_b32 v1, 1, v0        */
    code[n++] = a3d_vopc(A3D_OPC_CMP_NE_U32, 1, A3D_SRC_ZERO);                   /* v_cmp_ne_u32 vcc, 0, v1    */
    code[n++] = a3d_vop1(A3D_OP1_MOV_B32, 2, SGPR(2));                           /* v_mov_b32 v2, s2  (x1)     */
    code[n++] = a3d_vop1(A3D_OP1_MOV_B32, 7, SGPR(0));                           /* v_mov_b32 v7, s0  (x0)     */
    code[n++] = a3d_vop2(A3D_OP2_CNDMASK_B32, 3, 2, A3D_SRC_VGPR(7));            /* v_cndmask_b32 v3, v7, v2, vcc */
    code[n++] = a3d_vop2(A3D_OP2_LSHRREV_B32, 1, 0, A3D_SRC_INT(1));             /* v_lshrrev_b32 v1, 1, v0    */
    code[n++] = a3d_vop2(A3D_OP2_AND_B32, 1, 1, A3D_SRC_INT(1));                 /* v_and_b32 v1, 1, v1        */
    code[n++] = a3d_vopc(A3D_OPC_CMP_NE_U32, 1, A3D_SRC_ZERO);                   /* v_cmp_ne_u32 vcc, 0, v1    */
    code[n++] = a3d_vop1(A3D_OP1_MOV_B32, 2, SGPR(3));                           /* v_mov_b32 v2, s3  (y1)     */
    code[n++] = a3d_vop1(A3D_OP1_MOV_B32, 7, SGPR(1));                           /* v_mov_b32 v7, s1  (y0)     */
    code[n++] = a3d_vop2(A3D_OP2_CNDMASK_B32, 4, 2, A3D_SRC_VGPR(7));            /* v_cndmask_b32 v4, v7, v2, vcc */
    code[n++] = a3d_vop1(A3D_OP1_MOV_B32, 5, A3D_SRC_ZERO);                      /* v_mov_b32 v5, 0.0   (z)    */
    code[n++] = a3d_vop1(A3D_OP1_MOV_B32, 6, A3D_SRC_ONE_F);                     /* v_mov_b32 v6, 1.0   (w)    */
    code[n++] = a3d_exp0(0xF, A3D_EXP_TARGET_PARAM0, 0, 0, 0);                   /* exp param0 v3, v4, v5, v6 (one is required) */
    code[n++] = a3d_exp1(3, 4, 5, 6);
    code[n++] = a3d_exp0(0xF, A3D_EXP_TARGET_POS0, 0, 1, 0);                     /* exp pos0 v3, v4, v5, v6 done */
    code[n++] = a3d_exp1(3, 4, 5, 6);
    code[n++] = A3D_S_ENDPGM;
    return n;
}

/* A solid colour: s0 = (r, g), s1 = (b, a) as halves (FP16_ABGR export, RB+). */
uint32_t amd_comp_build_ps_fill(uint32_t *code)
{
    uint32_t n = 0;
    code[n++] = a3d_vop1(A3D_OP1_MOV_B32, 0, SGPR(0));                           /* v_mov_b32 v0, s0           */
    code[n++] = a3d_vop1(A3D_OP1_MOV_B32, 1, SGPR(1));                           /* v_mov_b32 v1, s1           */
    code[n++] = a3d_exp0(0xF, A3D_EXP_TARGET_MRT0, 1, 1, 1);                     /* exp mrt0 v0, v1 compr done vm */
    code[n++] = a3d_exp1(0, 1, 0, 0);
    code[n++] = A3D_S_ENDPGM;
    return n;
}

/* A surface: texel = pixel position + (s8, s9) (integers, wrap-around arithmetic), read with IMAGE_LOAD through the resource in
 * s0..s7; the alpha is the opacity in s10 (the surface has no alpha of its own). */
uint32_t amd_comp_build_ps_image(uint32_t *code)
{
    uint32_t n = 0;
    code[n++] = a3d_vop1(A3D_OP1_CVT_U32_F32, 4, A3D_SRC_VGPR(2));               /* v_cvt_u32_f32 v4, v2 (x)   */
    code[n++] = a3d_vop1(A3D_OP1_CVT_U32_F32, 5, A3D_SRC_VGPR(3));               /* v_cvt_u32_f32 v5, v3 (y)   */
    code[n++] = a3d_vop2(A3D_OP2_ADD_U32, 4, 4, SGPR(8));                        /* v_add_u32 v4, s8, v4       */
    code[n++] = a3d_vop2(A3D_OP2_ADD_U32, 5, 5, SGPR(9));                        /* v_add_u32 v5, s9, v5       */
    code[n++] = a3d_mimg_0(A3D_MIMG_IMAGE_LOAD, 0xF);                            /* image_load v[6:9], v[4:5], s[0:7] dmask:0xf */
    code[n++] = a3d_mimg_1(4, 6, 0, 0);
    code[n++] = A3D_S_WAITCNT_VM0;                                               /* s_waitcnt vmcnt(0)         */
    code[n++] = a3d_vop1(A3D_OP1_MOV_B32, 9, SGPR(10));                          /* v_mov_b32 v9, s10 (alpha)  */
    code[n++] = a3d_vop3a_0(A3D_OP3_CVT_PKRTZ_F16_F32, 10);                      /* v_cvt_pkrtz_f16_f32 v10, v6, v7 */
    code[n++] = a3d_vop3a_1(A3D_SRC_VGPR(6), A3D_SRC_VGPR(7), 0);
    code[n++] = a3d_vop3a_0(A3D_OP3_CVT_PKRTZ_F16_F32, 11);                      /* v_cvt_pkrtz_f16_f32 v11, v8, v9 */
    code[n++] = a3d_vop3a_1(A3D_SRC_VGPR(8), A3D_SRC_VGPR(9), 0);
    code[n++] = a3d_exp0(0xF, A3D_EXP_TARGET_MRT0, 1, 1, 1);                     /* exp mrt0 v10, v11 compr done vm */
    code[n++] = a3d_exp1(10, 11, 0, 0);
    code[n++] = A3D_S_ENDPGM;
    return n;
}

/* One axis of the soft shadow: vout = clamp(min(pos - lo, hi - pos) * (1 / r), 0, 1); lo / hi / 1/r in SGPRs, v5 is scratch. */
static void shadow_axis(uint32_t *code, uint32_t *n, uint32_t vpos, uint32_t vout, uint32_t s_lo, uint32_t s_hi)
{
    code[(*n)++] = a3d_vop2(A3D_OP2_SUBREV_F32, vout, vpos, SGPR(s_lo));         /* vout = pos - lo            */
    code[(*n)++] = a3d_vop2(A3D_OP2_SUB_F32, 5, vpos, SGPR(s_hi));               /* v5 = hi - pos              */
    code[(*n)++] = a3d_vop2(A3D_OP2_MIN_F32, vout, 5, A3D_SRC_VGPR(vout));       /* vout = min(vout, v5)       */
    code[(*n)++] = a3d_vop2(A3D_OP2_MUL_F32, vout, vout, SGPR(4));               /* vout *= 1 / r              */
    code[(*n)++] = a3d_vop2(A3D_OP2_MIN_F32, vout, vout, A3D_SRC_ONE_F);         /* vout = min(1.0, vout)      */
    code[(*n)++] = a3d_vop2(A3D_OP2_MAX_F32, vout, vout, A3D_SRC_ZERO);          /* vout = max(0.0, vout)      */
}

/* A soft black shadow: alpha = s5 * fx * fy (the same formula as the M3 shadow shader, which amd_3d_shadow_alpha() checks). */
uint32_t amd_comp_build_ps_shadow(uint32_t *code)
{
    uint32_t n = 0;
    shadow_axis(code, &n, 2, 4, 0, 2);                                           /* v4 = fx                    */
    shadow_axis(code, &n, 3, 6, 1, 3);                                           /* v6 = fy                    */
    code[n++] = a3d_vop2(A3D_OP2_MUL_F32, 4, 6, A3D_SRC_VGPR(4));                /* v4 = fx * fy               */
    code[n++] = a3d_vop2(A3D_OP2_MUL_F32, 4, 4, SGPR(5));                        /* v4 *= alpha                */
    code[n++] = a3d_vop1(A3D_OP1_MOV_B32, 0, A3D_SRC_ZERO);                      /* v0 = (r, g) = (0, 0)       */
    code[n++] = a3d_vop1(A3D_OP1_MOV_B32, 5, A3D_SRC_ZERO);                      /* v5 = b = 0.0               */
    code[n++] = a3d_vop3a_0(A3D_OP3_CVT_PKRTZ_F16_F32, 1);                       /* v1 = (b, alpha) as halves  */
    code[n++] = a3d_vop3a_1(A3D_SRC_VGPR(5), A3D_SRC_VGPR(4), 0);
    code[n++] = a3d_exp0(0xF, A3D_EXP_TARGET_MRT0, 1, 1, 1);                     /* exp mrt0 v0, v1 compr done vm */
    code[n++] = a3d_exp1(0, 1, 0, 0);
    code[n++] = A3D_S_ENDPGM;
    return n;
}

/* ---- building a frame ---- */

void amd_comp_begin(AmdCompFrame *f, uint32_t *ib, uint32_t cap, uint64_t rt_mc, uint32_t pitch_px, uint32_t width, uint32_t height,
                    int bgr, uint64_t shader_mc)
{
    Ib b = { ib, 0, cap, 0 };
    f->ib = ib; f->cap = cap;
    f->rt_mc = rt_mc; f->pitch_px = pitch_px; f->width = width; f->height = height; f->bgr = bgr;
    f->vs_mc = shader_mc + AMD_COMP_VS_OFF;
    f->ps_fill_mc = shader_mc + AMD_COMP_PS_FILL_OFF;
    f->ps_image_mc = shader_mc + AMD_COMP_PS_IMAGE_OFF;
    f->ps_shadow_mc = shader_mc + AMD_COMP_PS_SHADOW_OFF;
    f->draws = 0;
    a3d_emit_head(&b, rt_mc, pitch_px, width, height, bgr);
    f->n = b.n; f->over = b.over;
}

/* One draw of the rectangle with the given pixel shader and its user SGPRs; the vertex shader gets the rectangle as floats. */
static void comp_draw(AmdCompFrame *f, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, int blend, uint64_t ps_mc, int ps_pos,
                      uint32_t ps_vgprs, const uint32_t *ps_user, uint32_t ps_user_n)
{
    Ib b = { f->ib, f->n, f->cap, f->over };
    uint32_t vs_user[4];
    A3dDraw d;
    if (x1 > f->width) x1 = f->width;
    if (y1 > f->height) y1 = f->height;
    if (x0 >= x1 || y0 >= y1)
        return;
    vs_user[0] = a3d_f32_bits(x0); vs_user[1] = a3d_f32_bits(y0); vs_user[2] = a3d_f32_bits(x1); vs_user[3] = a3d_f32_bits(y1);
    d.x0 = x0; d.y0 = y0; d.x1 = x1; d.y1 = y1;
    d.blend = blend;
    d.ps_mc = ps_mc; d.ps_pos = ps_pos;
    d.ps_vgprs = ps_vgprs;
    d.ps_sgprs = 2;                 /* 24 SGPRs: up to 11 user SGPRs + the system ones + VCC */
    d.ps_user_n = ps_user_n; d.ps_user = ps_user;
    d.vs_mc = f->vs_mc; d.vs_vgprs = 1; d.vs_user_n = 4; d.vs_user = vs_user;
    a3d_emit_draw(&b, &d);
    f->n = b.n; f->over = b.over;
    f->draws++;
}

void amd_comp_fill(AmdCompFrame *f, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t r, uint32_t g, uint32_t b)
{
    uint32_t u[2];
    u[0] = a3d_f16_unorm8(r) | (a3d_f16_unorm8(g) << 16);
    u[1] = a3d_f16_unorm8(b) | (a3d_f16_unorm8(255) << 16);
    comp_draw(f, x0, y0, x1, y1, 0, f->ps_fill_mc, 0, 0, u, 2);
}

void amd_comp_image(AmdCompFrame *f, const AmdSurface *s, uint32_t sx, uint32_t sy, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1,
                    uint32_t opacity, int bgr)
{
    Amd3dJob t = { 0 };
    uint32_t u[11];
    t.tex_mc = s->mc; t.tex_w = s->w; t.tex_h = s->h; t.tex_pitch_px = s->pitch_px; t.bgr = bgr;
    amd_3d_build_tex_desc(u, &t);
    u[8] = sx - x0;                 /* texel = pixel + (sx - x0, sy - y0), as wrap-around integers */
    u[9] = sy - y0;
    u[10] = opacity >= 255 ? 0x3F800000u : a3d_f32_ratio(opacity, 255);
    comp_draw(f, x0, y0, x1, y1, opacity < 255, f->ps_image_mc, 1, 2, u, 11);
}

void amd_comp_shadow(AmdCompFrame *f, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t r, uint32_t alpha)
{
    uint32_t u[6];
    u[0] = a3d_f32_bits(x0); u[1] = a3d_f32_bits(y0); u[2] = a3d_f32_bits(x1); u[3] = a3d_f32_bits(y1);
    u[4] = a3d_f32_ratio(1, r ? r : 1);
    u[5] = a3d_f32_ratio(alpha, 255);
    comp_draw(f, x0, y0, x1, y1, 1, f->ps_shadow_mc, 1, 1, u, 6);
}

uint32_t amd_comp_end(AmdCompFrame *f, uint64_t sync_mc)
{
    Ib b = { f->ib, f->n, f->cap, f->over };
    a3d_emit_tail(&b, sync_mc);
    f->n = b.n; f->over = b.over;
    return f->over ? 0 : f->n;
}

/* ---- running it, and the M4a test ---- */
#define VRAM_COMP_SHADER_OFF 0x0B004000u   /* 4 KiB: the four shaders at AMD_COMP_*_OFF */
#define VRAM_COMP_SYNC_OFF 0x0B300040u     /* 4 bytes for the end-of-frame sync */
/* Below 256 MiB: the CPU reaches VRAM only through BAR0, and on this laptop BAR0 covers the first 256 MiB of the 512 MiB (reads at
 * 256 MiB and up gave 0xFFFFFFFF, 2026-10-29). 208 MiB = after the PSP TMR (192 MiB, 4 MiB). */
#define VRAM_COMP_RT_OFF 0x0D000000u       /* the 512 x 256 offscreen target (512 KiB) */
#define VRAM_COMP_SURF_OFF 0x0D100000u     /* the test windows, 256 KiB apart */
#define COMP_RT_W 512u
#define COMP_RT_H 256u

static const char *status = "compositor test not run";
static uint32_t val[AMD_COMP_VALUES];
const char *amd_comp_status(void) { return status; }
uint32_t amd_comp_value(int what) { return (what >= 0 && what < AMD_COMP_VALUES) ? val[what] : 0; }

static unsigned long long comp_tsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((unsigned long long)hi << 32) | lo;
}

/* Writes the four shaders to VRAM, each padded with zeros (the shader fetch reads ahead). */
static void upload_shaders(void)
{
    static const uint32_t off[4] = { AMD_COMP_VS_OFF, AMD_COMP_PS_FILL_OFF, AMD_COMP_PS_IMAGE_OFF, AMD_COMP_PS_SHADOW_OFF };
    uint32_t code[AMD_COMP_SHADER_MAX], k, i, n;
    for (k = 0; k < 4; k++) {
        n = k == 0 ? amd_comp_build_vs(code) : k == 1 ? amd_comp_build_ps_fill(code) : k == 2 ? amd_comp_build_ps_image(code)
                                                                                      : amd_comp_build_ps_shadow(code);
        for (i = 0; i < 0x200u / 4u; i++)
            amd_gpu_vram_write32(VRAM_COMP_SHADER_OFF + off[k] + 4u * i, i < n ? code[i] : 0u);
    }
}

/* The test scene, in scene coordinates (0, 0) .. (480, 224); the caller places it at (ox, oy). Three windows painted in order, each
 * after its shadow (offset 6, 8; r 12; alpha 150; the shadow rectangle is the window moved by the offset and grown by r / 2). */
#define SCENE_W 480u
#define SCENE_H 224u
static const struct { uint32_t x0, y0, x1, y1, opacity; } win[3] = {
    { 24, 24, 184, 124, 255 },   /* 160 x 100 */
    { 134, 74, 314, 194, 255 },  /* 180 x 120, over the first one and its shadow */
    { 284, 14, 444, 114, 160 },  /* 160 x 100, partly transparent, over the second one */
};
#define SHADOW_DX 6u
#define SHADOW_DY 8u
#define SHADOW_R 12u
#define SHADOW_A 150u
#define BG_R 0x30u
#define BG_G 0x50u
#define BG_B 0x70u

static uint32_t rgb_px(uint32_t r, uint32_t g, uint32_t b, int bgr) { return bgr ? ((r << 16) | (g << 8) | b) : ((b << 16) | (g << 8) | r); }

/* Pixel (x, y) of window k's picture: a 16-row title bar, a 1-pixel darker border, and a body with a gradient. */
static uint32_t win_texel(uint32_t k, uint32_t x, uint32_t y, int bgr)
{
    uint32_t w = win[k].x1 - win[k].x0, h = win[k].y1 - win[k].y0;
    if (x == 0 || y == 0 || x == w - 1 || y == h - 1)
        return rgb_px(0x18, 0x18, 0x20, bgr);
    if (y < 16)
        return rgb_px(k == 0 ? 0x40u : k == 1 ? 0xC0u : 0x40u, k == 1 ? 0x60u : 0x90u, k == 2 ? 0x30u : 0xE0u, bgr);
    return rgb_px(0xF0u - (x * 0x40u) / w, 0xF0u - (y * 0x60u) / h, 0xE8u - k * 0x30u, bgr);
}

static void shadow_rect(uint32_t k, uint32_t *x0, uint32_t *y0, uint32_t *x1, uint32_t *y1)
{
    *x0 = win[k].x0 + SHADOW_DX - SHADOW_R / 2u; *y0 = win[k].y0 + SHADOW_DY - SHADOW_R / 2u;
    *x1 = win[k].x1 + SHADOW_DX + SHADOW_R / 2u; *y1 = win[k].y1 + SHADOW_DY + SHADOW_R / 2u;
}

/* What the scene should be at scene pixel (x, y): the background, then every shadow and window "over" it in the painting order. */
static uint32_t scene_px(uint32_t x, uint32_t y, int bgr, int stage)
{
    uint32_t v = rgb_px(BG_R, BG_G, BG_B, bgr), k;
    for (k = 0; k < 3; k++) {
        Amd3dJob s = { 0 };
        shadow_rect(k, &s.x0, &s.y0, &s.x1, &s.y1);
        s.shadow_r = SHADOW_R; s.a = SHADOW_A;
        if (stage >= 2 && x >= s.x0 && x < s.x1 && y >= s.y0 && y < s.y1)
            v = amd_3d_blend_px(0, v, amd_3d_shadow_alpha(&s, x, y));
        if (stage >= 3 && x >= win[k].x0 && x < win[k].x1 && y >= win[k].y0 && y < win[k].y1) {
            uint32_t t = win_texel(k, x - win[k].x0, y - win[k].y0, bgr);
            v = win[k].opacity >= 255 ? t : amd_3d_blend_px(t, v, win[k].opacity);
        }
    }
    return v & 0x00FFFFFFu;
}

static int near3(uint32_t a, uint32_t b)
{
    int s;
    for (s = 0; s < 24; s += 8) {
        int d = (int)((a >> s) & 0xFFu) - (int)((b >> s) & 0xFFu);
        if (d > 3 || d < -3) return 0;
    }
    return 1;
}

/* Composites the scene at (ox, oy) of the target (VRAM offset base_off, GPU address rt_mc) in ONE submission and checks it, with a
 * margin of 8 pixels around it that must stay as the CPU wrote it. stage 1: the background only, 2: and the shadows, 3: and the
 * windows (the whole scene). via_frame_ib 0: the stream goes through the 4 KiB IB page the 3D tests use (it must fit in 1024
 * dwords), 1: through the frame IB (128 KiB through the GART). */
static int comp_run(uint32_t base_off, uint64_t rt_mc, uint32_t pitch_px, uint32_t width, uint32_t height, int bgr, uint32_t ox, uint32_t oy,
                    int stage, int via_frame_ib)
{
    static uint32_t ib[4096];
    static int shaders_up;
    AmdSurface surf[3];
    AmdCompFrame f;
    uint32_t k, x, y, n, seq, cap;
    uint32_t cx0 = ox >= 8u ? ox - 8u : 0, cy0 = oy >= 8u ? oy - 8u : 0;
    uint32_t cx1 = ox + SCENE_W + 8u > width ? width : ox + SCENE_W + 8u, cy1 = oy + SCENE_H + 8u > height ? height : oy + SCENE_H + 8u;
    uint64_t shader_mc = amd_native_vram_mc_addr(VRAM_COMP_SHADER_OFF, 0x1000u), sync_mc = amd_native_vram_mc_addr(VRAM_COMP_SYNC_OFF, 4u);
    uint64_t frame_va = via_frame_ib ? amd_gpu_frame_ib_va() : amd_gpu_ib_va();
    unsigned long long t0;
    const uint32_t pattern = 0x00112233u;

    for (k = 0; k < AMD_COMP_VALUES; k++) val[k] = 0;
    if (!amd_gpu_ring_running()) { status = "compositor test skipped: the persistent ring is not running"; return 0; }
    if (!shader_mc || !sync_mc || !rt_mc) { status = "compositor test skipped: no MC address"; return 0; }
    if (!frame_va) { status = "compositor test FAILED: the frame IB could not be mapped through the GART"; return 0; }
    if (ox + SCENE_W > width || oy + SCENE_H > height) { status = "compositor test skipped: the target is too small"; return 0; }

    /* the CPU must reach the target and the surfaces (BAR0 may be smaller than the VRAM): a word written must read back */
    amd_gpu_vram_write32(base_off, 0x5AA5C33Cu);
    amd_gpu_vram_write32(VRAM_COMP_SURF_OFF + 2u * 0x40000u, 0xC33C5AA5u);
    amd_gpu_wc_flush();
    if (amd_gpu_vram_read32(base_off) != 0x5AA5C33Cu || amd_gpu_vram_read32(VRAM_COMP_SURF_OFF + 2u * 0x40000u) != 0xC33C5AA5u) {
        status = "compositor test skipped: the CPU cannot read back what it wrote to the target / surfaces (outside BAR0?)";
        return 0;
    }

    if (!shaders_up) { upload_shaders(); shaders_up = 1; }
    for (k = 0; k < 3; k++) {               /* the windows' pictures */
        surf[k].off = VRAM_COMP_SURF_OFF + k * 0x40000u;
        surf[k].w = win[k].x1 - win[k].x0; surf[k].h = win[k].y1 - win[k].y0; surf[k].pitch_px = 256;
        surf[k].mc = amd_native_vram_mc_addr(surf[k].off, surf[k].pitch_px * surf[k].h * 4u);
        if (!surf[k].mc) { status = "compositor test skipped: no MC address for a surface"; return 0; }
        for (y = 0; y < surf[k].h; y++)
            for (x = 0; x < surf[k].w; x++)
                amd_gpu_vram_write32(surf[k].off + (y * surf[k].pitch_px + x) * 4u, win_texel(k, x, y, bgr));
    }
    for (y = cy0; y < cy1; y++)              /* what must stay around the scene (and is painted over inside it) */
        for (x = cx0; x < cx1; x++)
            amd_gpu_vram_write32(base_off + (y * pitch_px + x) * 4u, pattern);
    amd_gpu_vram_write32(VRAM_COMP_SYNC_OFF, 0);
    amd_gpu_wc_flush();

    /* the frame: background, then per window its shadow and the window */
    cap = via_frame_ib ? (amd_gpu_frame_ib_max_dwords() < 4096u ? amd_gpu_frame_ib_max_dwords() : 4096u) : 1024u;
    amd_comp_begin(&f, ib, cap, rt_mc, pitch_px, width, height, bgr, shader_mc);
    amd_comp_fill(&f, ox, oy, ox + SCENE_W, oy + SCENE_H, BG_R, BG_G, BG_B);
    for (k = 0; k < 3; k++) {
        uint32_t sx0, sy0, sx1, sy1;
        shadow_rect(k, &sx0, &sy0, &sx1, &sy1);
        if (stage >= 2)
            amd_comp_shadow(&f, ox + sx0, oy + sy0, ox + sx1, oy + sy1, SHADOW_R, SHADOW_A);
        if (stage >= 3)
            amd_comp_image(&f, &surf[k], 0, 0, ox + win[k].x0, oy + win[k].y0, ox + win[k].x1, oy + win[k].y1, win[k].opacity, bgr);
    }
    n = amd_comp_end(&f, sync_mc);
    val[AMD_COMP_V_DWORDS] = n; val[AMD_COMP_V_DRAWS] = f.draws;
    if (!n) { status = "compositor test skipped: the frame does not fit"; return 0; }

    if (a3d_default_state()) { status = "compositor test FAILED: the default state stream did not run"; return 0; }
    for (k = 0; k < n; k++) {
        if (via_frame_ib) amd_gpu_frame_ib_write32(k, ib[k]);
        else amd_gpu_ib_write32(k, ib[k]);
    }
    amd_gpu_wc_flush();
    amd_gpu_ib_flush();
    amd_gpu_fault_clear();
    t0 = comp_tsc();
    seq = amd_gpu_submit_ib(frame_va, n);
    if (!seq) { status = "compositor test FAILED: the ring did not take the frame"; return 0; }
    if (!amd_gpu_fence_wait(seq, 8000000u)) {
        amd_gpu_fault_snapshot();
        val[AMD_COMP_V_FAULT_GC] = amd_native_fault_gc_status(); val[AMD_COMP_V_FAULT_GC + 1] = amd_native_fault_gc_lo();
        val[AMD_COMP_V_FAULT_GC + 2] = amd_native_fault_gc_hi();
        val[AMD_COMP_V_GRBM] = amd_gpu_reg_read32(0x2004u);               /* GRBM_STATUS: which blocks are still busy */
        status = "compositor test FAILED: fence timeout (the GPU did not finish the frame)";
        return 0;
    }
    val[AMD_COMP_V_CYCLES] = (uint32_t)(comp_tsc() - t0);

    for (y = cy0; y < cy1; y++)
        for (x = cx0; x < cx1; x++) {
            uint32_t v = amd_gpu_vram_read32(base_off + (y * pitch_px + x) * 4u) & 0x00FFFFFFu;
            if (x >= ox && x < ox + SCENE_W && y >= oy && y < oy + SCENE_H) {
                uint32_t want = scene_px(x - ox, y - oy, bgr, stage);
                val[AMD_COMP_V_IN_TOTAL]++;
                if (near3(v, want)) val[AMD_COMP_V_IN_GOOD]++;
                else if (!val[AMD_COMP_V_FIRST_IN]) {
                    val[AMD_COMP_V_FIRST_IN] = v | 0x01000000u; val[AMD_COMP_V_FIRST_IN + 1] = want;
                    val[AMD_COMP_V_FIRST_IN + 2] = (x - ox) | ((y - oy) << 16);
                }
            } else {
                val[AMD_COMP_V_OUT_TOTAL]++;
                if (v != (pattern & 0x00FFFFFFu)) val[AMD_COMP_V_OUT_BAD]++;
            }
        }
    if (val[AMD_COMP_V_IN_GOOD] == val[AMD_COMP_V_IN_TOTAL] && !val[AMD_COMP_V_OUT_BAD]) {
        status = stage == 1 ? "compositor PASS: the background fill, every pixel right"
               : stage == 2 ? "compositor PASS: background and 3 soft shadows in one submission, every pixel right"
                            : "compositor PASS: background, 3 shadows and 3 windows (one 63% opaque) in one submission, every pixel right";
        return 1;
    }
    amd_gpu_fault_snapshot();
    val[AMD_COMP_V_FAULT_GC] = amd_native_fault_gc_status(); val[AMD_COMP_V_FAULT_GC + 1] = amd_native_fault_gc_lo();
    val[AMD_COMP_V_FAULT_GC + 2] = amd_native_fault_gc_hi();
    status = val[AMD_COMP_V_IN_GOOD] ? "compositor FAILED: some pixels are wrong" : "compositor FAILED: nothing of the scene is there";
    return 0;
}

int amd_comp_test_offscreen(int stage, int via_frame_ib)
{
    uint64_t rt_mc = amd_native_vram_mc_addr(VRAM_COMP_RT_OFF, COMP_RT_W * COMP_RT_H * 4u);
    return comp_run(VRAM_COMP_RT_OFF, rt_mc, COMP_RT_W, COMP_RT_W, COMP_RT_H, 1, 16, 16, stage, via_frame_ib);
}

/* On the screen: the scene in the top right corner, 20 pixels in. */
int amd_comp_test_screen(uint64_t fb_phys, uint32_t pitch_bytes, uint32_t width, uint32_t height, uint64_t vram_cpu, int bgr,
                         int via_frame_ib)
{
    uint64_t off64;
    if (!pitch_bytes || (pitch_bytes & 3u) || width < SCENE_W + 40u || height < SCENE_H + 40u || fb_phys < vram_cpu) {
        status = "compositor screen test skipped: bad framebuffer info"; return 0; }
    off64 = fb_phys - vram_cpu;
    if (off64 + (uint64_t)pitch_bytes * height > (uint64_t)VRAM_COMP_SHADER_OFF) {
        status = "compositor screen test skipped: the framebuffer reaches the test memory"; return 0; }
    return comp_run((uint32_t)off64, amd_native_vram_mc_addr((uint32_t)off64, pitch_bytes * height), pitch_bytes / 4u, width, height, bgr,
                    width - SCENE_W - 20u, 20u, 3, via_frame_ib);
}
