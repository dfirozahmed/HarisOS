/* AMD Renoir (GFX9) 3D pipeline, first step (M1): one coloured rectangle drawn by the graphics pipeline.
 * See include/amd_3d.h.
 *
 * Where the values come from (the user's copy of the sources is in D:\temp\mesa-main and D:\temp\New folder (2)):
 *   - the order of a graphics command stream and the GFX9 start-up registers: Mesa src/amd/common/ac_cmdbuf.c
 *     gfx6_init_graphics_preamble_state() and radeonsi si_state.c gfx6_init_gfx_preamble_state(): CONTEXT_CONTROL, CLEAR_STATE
 *     (radeonsi relies on it for GFX9: has_clear_state), then the registers CLEAR_STATE does not set right;
 *   - the colour buffer: ac_descriptors.c ac_init_gfx6_cb_surface() + ac_set_mutable_cb_surface_fields() (the GFX9 branches) and
 *     si_state.c gfx6_emit_framebuffer_state(): 15 consecutive registers from CB_COLOR0_BASE, then CB_MRT0_EPITCH;
 *   - RB+ (Renoir has it): ac_formats.c ac_set_sx_downconvert_state_for_mrt() and ac_shader_util.c ac_choose_spi_color_formats():
 *     an 8-8-8-8 UNORM target is exported as FP16_ABGR (compressed export) with SX_PS_DOWNCONVERT = SX_RT_EXPORT_8_8_8_8;
 *   - blending off: si_state.c si_create_blend_state_mode(): CB_COLOR_CONTROL mode NORMAL, ROP3 0xCC, SX_MRT0_BLEND_OPT disabled;
 *   - shader registers: si_state_shaders.cpp si_shader_vs_legacy() / si_shader_ps(): PGM_LO/HI, RSRC1/2/3, SPI_PS_INPUT_ENA, the
 *     VS "window space position" mode (PA_CL_VTE_CNTL VTX_XY_FMT / VTX_Z_FMT: positions are pixel coordinates, no viewport);
 *   - the draw: si_state_draw.cpp: VGT_SHADER_STAGES_EN = MAX_PRIMGRP_IN_WAVE(2), IA_MULTI_VGT_PARAM (GFX9: a UCONFIG
 *     register), VGT_PRIMITIVE_TYPE, NUM_INSTANCES, DRAW_INDEX_AUTO with DI_SRC_SEL_AUTO_INDEX;
 *   - Renoir numbers (ac_gpu_info.c): pc_lines 1024, one shader engine, so pbb_max_alloc_count = MIN(128, 1024 / 4) = 128.
 * Register numbers and bit positions: include/amd_3d_regs.h (generated from gc_9_1_offset.h / gc_9_1_sh_mask.h). Enum values
 * (the numbers the register fields take) are from Mesa src/amd/registers/gfx9.json and listed below with that name.
 * The shaders are hand assembled with the encoders of include/amd_3d_isa.h. */
#include "amd_3d.h"
#include "amd_3d_isa.h"
#include "amd_3d_regs.h"
#include "clearstate_defs.h"
#include "clearstate_gfx9.h"
#include "amd_native.h"
#include "gpu_log.h"

/* ---- enum values (gfx9.json) ---- */
#define E_COLOR_8_8_8_8 10u
#define E_NUMBER_UNORM 0u
#define E_SWAP_STD 0u
#define E_SWAP_ALT 1u              /* memory order B, G, R, A for a (R, G, B, A) export */
#define E_SPI_SHADER_FP16_ABGR 4u
#define E_SPI_SHADER_4COMP 4u
#define E_SX_RT_EXPORT_8_8_8_8 5u
#define E_8BIT_FORMAT_0_5 6u
#define E_OPT_COMB_BLEND_DISABLED 6u
#define E_CB_NORMAL 1u
#define E_EARLY_Z_THEN_LATE_Z 1u
#define E_PUNCHOUT_FORCE_OFF 2u
#define E_X_ROUND_TO_EVEN 2u
#define E_X_16_8_FIXED_POINT_1_256TH 5u
#define E_DI_PT_TRISTRIP 6u
#define E_DI_SRC_SEL_AUTO_INDEX 2u
#define E_MAX_BLOCK_SIZE_256B 2u
#define E_RESOURCE_2D 1u           /* RADEON_RESOURCE_2D */

/* ---- PM4 ---- */
#define PKT3(op, n) (0xC0000000u | ((uint32_t)(n) << 16) | ((uint32_t)(op) << 8))
#define OP_CLEAR_STATE 0x12u
#define OP_CONTEXT_CONTROL 0x28u
#define OP_DRAW_INDEX_AUTO 0x2Du
#define OP_NUM_INSTANCES 0x2Fu
#define OP_ACQUIRE_MEM 0x58u
#define OP_EVENT_WRITE 0x46u
#define OP_RELEASE_MEM 0x49u
#define OP_WAIT_REG_MEM 0x3Cu
#define E_EVENT_FLUSH_AND_INV_CB_DATA_TS 45u /* gfx9.json V_028A90 */
#define E_EVENT_FLUSH_AND_INV_CB_META 46u
#define OP_SET_CONTEXT_REG 0x69u
#define OP_SET_SH_REG 0x76u
#define OP_SET_UCONFIG_REG 0x79u

/* CP_COHER_CNTL bits (gc_9_1_sh_mask.h): the shader code and constants are fetched through the instruction / scalar caches. */
#define COHER_TCL1 (1u << 22)
#define COHER_TC (1u << 23)
#define COHER_KCACHE (1u << 27)
#define COHER_ICACHE (1u << 29)
#define COHER_NO_PFP_SYNC (1u << 31) /* Mesa: "don't sync pfp, i.e. execute the sync in ME" */

typedef struct
{
    uint32_t *p;
    uint32_t n, cap;
    int over;
} Ib;

static void put(Ib *b, uint32_t v)
{
    if (b->n < b->cap)
        b->p[b->n] = v;
    else
        b->over = 1;
    b->n++;
}

static void ctx1(Ib *b, uint32_t reg, uint32_t v)
{
    put(b, PKT3(OP_SET_CONTEXT_REG, 1));
    put(b, reg);
    put(b, v);
}

static void ctx2(Ib *b, uint32_t reg, uint32_t v0, uint32_t v1)
{
    put(b, PKT3(OP_SET_CONTEXT_REG, 2));
    put(b, reg);
    put(b, v0);
    put(b, v1);
}

static void sh1(Ib *b, uint32_t reg, uint32_t v)
{
    put(b, PKT3(OP_SET_SH_REG, 1));
    put(b, reg);
    put(b, v);
}

/* WRITE_DATA to memory (DST_SEL 5, with a write confirm), as the CP tests do */
static void write_data(Ib *b, uint64_t mc, uint32_t v)
{
    put(b, PKT3(0x37u, 3));
    put(b, (5u << 8) | (1u << 20));
    put(b, (uint32_t)mc);
    put(b, (uint32_t)(mc >> 32));
    put(b, v);
}

static void uc1(Ib *b, uint32_t reg, uint32_t v)
{
    put(b, PKT3(OP_SET_UCONFIG_REG, 1));
    put(b, reg);
    put(b, v);
}

/* ---- shaders ---- */
uint32_t amd_3d_build_vs(uint32_t *code, const Amd3dJob *job)
{
    uint32_t n = 0;
    uint32_t x0 = a3d_f32_bits(job->x0), x1 = a3d_f32_bits(job->x1);
    uint32_t y0 = a3d_f32_bits(job->y0), y1 = a3d_f32_bits(job->y1);

    /* A triangle strip of 4 vertices; v0 = the vertex number. Vertex 0 (x0,y0), 1 (x1,y0), 2 (x0,y1), 3 (x1,y1):
     * bit 0 of the number picks x, bit 1 picks y. The coordinates are literals, the position is in window space (pixels). */
    code[n++] = a3d_vop2(A3D_OP2_AND_B32, 1, 0, A3D_SRC_INT(1));                 /* v_and_b32 v1, 1, v0        */
    code[n++] = a3d_vopc(A3D_OPC_CMP_NE_U32, 1, A3D_SRC_ZERO);                   /* v_cmp_ne_u32 vcc, 0, v1    */
    code[n++] = a3d_vop1(A3D_OP1_MOV_B32, 2, A3D_SRC_LITERAL);                   /* v_mov_b32 v2, x1           */
    code[n++] = x1;
    code[n++] = a3d_vop2(A3D_OP2_CNDMASK_B32, 3, 2, A3D_SRC_LITERAL);            /* v_cndmask_b32 v3, x0, v2, vcc: vcc ? v2 : x0 */
    code[n++] = x0;
    code[n++] = a3d_vop2(A3D_OP2_LSHRREV_B32, 1, 0, A3D_SRC_INT(1));             /* v_lshrrev_b32 v1, 1, v0    */
    code[n++] = a3d_vop2(A3D_OP2_AND_B32, 1, 1, A3D_SRC_INT(1));                 /* v_and_b32 v1, 1, v1        */
    code[n++] = a3d_vopc(A3D_OPC_CMP_NE_U32, 1, A3D_SRC_ZERO);                   /* v_cmp_ne_u32 vcc, 0, v1    */
    code[n++] = a3d_vop1(A3D_OP1_MOV_B32, 2, A3D_SRC_LITERAL);                   /* v_mov_b32 v2, y1           */
    code[n++] = y1;
    code[n++] = a3d_vop2(A3D_OP2_CNDMASK_B32, 4, 2, A3D_SRC_LITERAL);            /* v_cndmask_b32 v4, y0, v2, vcc */
    code[n++] = y0;
    code[n++] = a3d_vop1(A3D_OP1_MOV_B32, 5, A3D_SRC_ZERO);                      /* v_mov_b32 v5, 0.0   (z)    */
    code[n++] = a3d_vop1(A3D_OP1_MOV_B32, 6, A3D_SRC_ONE_F);                     /* v_mov_b32 v6, 1.0   (w)    */
    /* A vertex shader has to export at least one parameter (Mesa si_shader_vs_legacy: "VS is required to export at least one param");
     * nothing reads it, SPI_VS_OUT_CONFIG says there is one. */
    code[n++] = a3d_exp0(0xF, A3D_EXP_TARGET_PARAM0, 0, 0, 0);                   /* exp param0 v3, v4, v5, v6   */
    code[n++] = a3d_exp1(3, 4, 5, 6);
    code[n++] = a3d_exp0(0xF, A3D_EXP_TARGET_POS0, 0, 1, 0);                     /* exp pos0 v3, v4, v5, v6 done */
    code[n++] = a3d_exp1(3, 4, 5, 6);
    code[n++] = A3D_S_ENDPGM;
    return n;
}

/* ---- the textured rectangle ---- */
#define E_IMG_DATA_FORMAT_8_8_8_8 10u
#define E_IMG_NUM_FORMAT_UNORM 0u
#define E_SQ_RSRC_IMG_2D 9u
#define E_SQ_SEL_0 0u
#define E_SQ_SEL_1 1u
#define E_SQ_SEL_X 4u
#define E_SQ_SEL_Y 5u
#define E_SQ_SEL_Z 6u

/* The T# of a linear 8-8-8-8 image (ac_descriptors.c ac_build_gfx6_texture_descriptor() with ac_set_mutable_tex_desc_fields(), the
 * GFX9 branches). The destination selects put the colour in R, G, B order: the image bytes are B, G, R, X (bgr) or R, G, B, X. */
void amd_3d_build_tex_desc(uint32_t d[8], const Amd3dJob *job)
{
    uint32_t sx = job->bgr ? E_SQ_SEL_Z : E_SQ_SEL_X, sz = job->bgr ? E_SQ_SEL_X : E_SQ_SEL_Z;

    d[0] = (uint32_t)(job->tex_mc >> 8);
    d[1] = (((uint32_t)(job->tex_mc >> 40) & 0xFFu) << F3D_SQ_IMG_RSRC_WORD1__BASE_ADDRESS_HI) |
           (E_IMG_DATA_FORMAT_8_8_8_8 << F3D_SQ_IMG_RSRC_WORD1__DATA_FORMAT) | (E_IMG_NUM_FORMAT_UNORM << F3D_SQ_IMG_RSRC_WORD1__NUM_FORMAT);
    d[2] = ((job->tex_w - 1) << F3D_SQ_IMG_RSRC_WORD2__WIDTH) | ((job->tex_h - 1) << F3D_SQ_IMG_RSRC_WORD2__HEIGHT) |
           (4u << F3D_SQ_IMG_RSRC_WORD2__PERF_MOD);
    d[3] = (sx << F3D_SQ_IMG_RSRC_WORD3__DST_SEL_X) | (E_SQ_SEL_Y << F3D_SQ_IMG_RSRC_WORD3__DST_SEL_Y) |
           (sz << F3D_SQ_IMG_RSRC_WORD3__DST_SEL_Z) | (E_SQ_SEL_1 << F3D_SQ_IMG_RSRC_WORD3__DST_SEL_W) | /* alpha: 1 (the X byte is no alpha) */
           (0u << F3D_SQ_IMG_RSRC_WORD3__SW_MODE) | (E_SQ_RSRC_IMG_2D << F3D_SQ_IMG_RSRC_WORD3__TYPE);   /* SW_MODE 0 = linear; one mip level */
    d[4] = (0u << F3D_SQ_IMG_RSRC_WORD4__DEPTH) | ((job->tex_pitch_px - 1) << F3D_SQ_IMG_RSRC_WORD4__PITCH); /* EPITCH */
    d[5] = 0;
    d[6] = 0;
    d[7] = 0;
}

/* The test image: red grows with x, green with y, blue is a diagonal ramp. */
uint32_t amd_3d_texel(const Amd3dJob *job, uint32_t x, uint32_t y)
{
    uint32_t r = (x * 255u) / (job->tex_w - 1u), g = (y * 255u) / (job->tex_h - 1u), b = (x * 5u + y * 9u) & 0xFFu;
    return job->bgr ? ((r << 16) | (g << 8) | b) : ((b << 16) | (g << 8) | r);
}

/* Pixel shader of the textured rectangle: the pixel position (v2, v3 = x + 0.5, y + 0.5) minus the rectangle's corner is the texel
 * (IMAGE_LOAD takes integer coordinates and needs no sampler); the 4 floats go out through FP16 packing as in the solid shader. */
static uint32_t build_ps_textured(uint32_t *code, const Amd3dJob *job)
{
    uint32_t n = 0;
    code[n++] = a3d_vop1(A3D_OP1_CVT_U32_F32, 4, A3D_SRC_VGPR(2));                /* v_cvt_u32_f32 v4, v2           */
    code[n++] = a3d_vop1(A3D_OP1_CVT_U32_F32, 5, A3D_SRC_VGPR(3));                /* v_cvt_u32_f32 v5, v3           */
    code[n++] = a3d_vop2(A3D_OP2_SUBREV_CO_U32, 4, 4, A3D_SRC_LITERAL);           /* v_subrev_co_u32 v4, vcc, x0, v4 (v4 - x0) */
    code[n++] = job->x0;
    code[n++] = a3d_vop2(A3D_OP2_SUBREV_CO_U32, 5, 5, A3D_SRC_LITERAL);           /* v_subrev_co_u32 v5, vcc, y0, v5 */
    code[n++] = job->y0;
    code[n++] = a3d_mimg_0(A3D_MIMG_IMAGE_LOAD, 0xF);                             /* image_load v[6:9], v[4:5], s[0:7] dmask:0xf */
    code[n++] = a3d_mimg_1(4, 6, 0, 0);
    code[n++] = A3D_S_WAITCNT_VM0;                                                /* s_waitcnt vmcnt(0)             */
    code[n++] = a3d_vop3a_0(A3D_OP3_CVT_PKRTZ_F16_F32, 10);                       /* v_cvt_pkrtz_f16_f32 v10, v6, v7 */
    code[n++] = a3d_vop3a_1(A3D_SRC_VGPR(6), A3D_SRC_VGPR(7), 0);
    code[n++] = a3d_vop3a_0(A3D_OP3_CVT_PKRTZ_F16_F32, 11);                       /* v_cvt_pkrtz_f16_f32 v11, v8, v9 */
    code[n++] = a3d_vop3a_1(A3D_SRC_VGPR(8), A3D_SRC_VGPR(9), 0);
    code[n++] = a3d_exp0(0xF, A3D_EXP_TARGET_MRT0, 1, 1, 1);                      /* exp mrt0 v10, v11 compr done vm */
    code[n++] = a3d_exp1(10, 11, 0, 0);
    code[n++] = A3D_S_ENDPGM;
    return n;
}

uint32_t amd_3d_build_ps(uint32_t *code, const Amd3dJob *job)
{
    uint32_t n = 0;
    if (job->textured)
        return build_ps_textured(code, job);
    /* RB+ wants FP16_ABGR: the colour goes out as four half floats packed two per VGPR (R low, G high; B low, A high). */
    uint32_t rg = a3d_f16_unorm8(job->r) | (a3d_f16_unorm8(job->g) << 16);
    uint32_t ba = a3d_f16_unorm8(job->b) | (a3d_f16_unorm8(job->a) << 16);

    code[n++] = a3d_vop1(A3D_OP1_MOV_B32, 0, A3D_SRC_LITERAL);                   /* v_mov_b32 v0, (r, g)       */
    code[n++] = rg;
    code[n++] = a3d_vop1(A3D_OP1_MOV_B32, 1, A3D_SRC_LITERAL);                   /* v_mov_b32 v1, (b, a)       */
    code[n++] = ba;
    code[n++] = a3d_exp0(0xF, A3D_EXP_TARGET_MRT0, 1, 1, 1);                     /* exp mrt0 v0, v1 compr done vm */
    code[n++] = a3d_exp1(0, 1, 0, 0);
    code[n++] = A3D_S_ENDPGM;
    return n;
}


/* ---- the default context state ----
 * Linux gfx_v9_0_cp_gfx_start() writes every context register of the GFX9 clear-state table (clearstate_gfx9.h, 879 registers, the
 * "gfx9_cs_data" extents) with SET_CONTEXT_REG when it starts the ring. Mesa's radeonsi then relies on that (has_clear_state;
 * ac_set_tracked_regs_to_clear_state() lists the same values: PA_CL_GB_*_ADJ = 1.0, PA_SU_VTX_CNTL = 5, PA_SC_EDGERULE = 0xAA99AAAA,
 * VGT_VERTEX_REUSE_BLOCK_CNTL = 0x1E ...). Nothing in HarisOS did it, and with the guard band registers at their reset value (0) every
 * triangle is clipped away. This writes the table straight into the context, as one command stream. */
uint32_t amd_3d_build_default_ib(uint32_t *ibp, uint32_t cap)
{
    Ib b = { ibp, 0, cap, 0 };
    const struct cs_section_def *sect;
    const struct cs_extent_def *ext;

    put(&b, PKT3(OP_CONTEXT_CONTROL, 1));
    put(&b, 0x80000000u);
    put(&b, 0x80000000u);
    for (sect = gfx9_cs_data; sect->section; ++sect) {
        if (sect->id != SECT_CONTEXT)
            continue;
        for (ext = sect->section; ext->extent; ++ext) {
            uint32_t i;
            put(&b, PKT3(OP_SET_CONTEXT_REG, ext->reg_count));
            put(&b, ext->reg_index - 0xA000u); /* PACKET3_SET_CONTEXT_REG_START */
            for (i = 0; i < ext->reg_count; i++)
                put(&b, ext->extent[i]);
        }
    }
    return b.over ? 0 : b.n;
}

/* ---- the command stream ---- */
uint32_t amd_3d_build_ib(uint32_t *ibp, uint32_t cap, const Amd3dJob *job)
{
    Ib b = { ibp, 0, cap, 0 };
    uint32_t swap = job->bgr ? E_SWAP_ALT : E_SWAP_STD;
    /* X8 formats have no alpha: Mesa sets FORCE_DST_ALPHA_1 when the format's 4th swizzle is "1" */
    uint32_t force_a1 = 1;

    /* 1. The default context state is NOT set up here: amd_3d_build_default_ib() does it first, as a stream of its own, with the
     * register table of the Linux driver. (CLEAR_STATE would not do: it only restores what the kernel driver put in the clear state
     * snapshot at start-up, and nothing did that on this GPU.) */

    /* 2. the shader code was written by the CPU: invalidate the instruction, scalar and vector caches first */
    put(&b, PKT3(OP_ACQUIRE_MEM, 5));
    put(&b, COHER_ICACHE | COHER_KCACHE | COHER_TCL1 | COHER_TC | COHER_NO_PFP_SYNC); /* CP_COHER_CNTL */
    put(&b, 0xFFFFFFFFu);                                                              /* CP_COHER_SIZE */
    put(&b, 0x000000FFu);                                                              /* CP_COHER_SIZE_HI */
    put(&b, 0);                                                                        /* CP_COHER_BASE */
    put(&b, 0);                                                                        /* CP_COHER_BASE_HI */
    put(&b, 0x0000000Au);                                                              /* POLL_INTERVAL */

    /* 3. GFX9 start-up registers (ac_cmdbuf.c, GFX9 branch). CLEAR_STATE does not restore the generic scissor. */
    ctx2(&b, R3D_PA_SC_GENERIC_SCISSOR_TL, 1u << F3D_PA_SC_GENERIC_SCISSOR_TL__WINDOW_OFFSET_DISABLE,
         (16384u << F3D_PA_SC_GENERIC_SCISSOR_BR__BR_X) | (16384u << F3D_PA_SC_GENERIC_SCISSOR_BR__BR_Y));
    ctx1(&b, R3D_DB_DFSM_CONTROL, E_PUNCHOUT_FORCE_OFF << F3D_DB_DFSM_CONTROL__PUNCHOUT_MODE);
    ctx1(&b, R3D_PA_SC_BINNER_CNTL_1, (127u << F3D_PA_SC_BINNER_CNTL_1__MAX_ALLOC_COUNT) |
                                      (1023u << F3D_PA_SC_BINNER_CNTL_1__MAX_PRIM_PER_BATCH)); /* pbb_max_alloc_count - 1 */
    ctx1(&b, R3D_PA_SC_CONSERVATIVE_RASTERIZATION_CNTL, 1u << F3D_PA_SC_CONSERVATIVE_RASTERIZATION_CNTL__NULL_SQUAD_AA_MASK_ENABLE);
    ctx1(&b, R3D_PA_SU_PRIM_FILTER_CNTL, (1u << F3D_PA_SU_PRIM_FILTER_CNTL__XMAX_RIGHT_EXCLUSION) |
                                         (1u << F3D_PA_SU_PRIM_FILTER_CNTL__YMAX_BOTTOM_EXCLUSION));
    ctx1(&b, R3D_PA_SC_WINDOW_SCISSOR_TL, 1u << F3D_PA_SC_WINDOW_SCISSOR_TL__WINDOW_OFFSET_DISABLE); /* "set to 0,0 in the preamble" */
    uc1(&b, R3D_VGT_MAX_VTX_INDX, 0xFFFFFFFFu); /* without these two the auto-generated indices could be clamped */
    uc1(&b, R3D_VGT_MIN_VTX_INDX, 0);
    uc1(&b, R3D_VGT_INDX_OFFSET, 0);
    uc1(&b, R3D_VGT_INSTANCE_BASE_ID, 0);
    sh1(&b, R3D_SPI_SHADER_PGM_RSRC3_PS, (0xFFFFu << F3D_SPI_SHADER_PGM_RSRC3_PS__CU_EN) |
                                         (0x3Fu << F3D_SPI_SHADER_PGM_RSRC3_PS__WAVE_LIMIT));

    /* 4. the colour buffer (gfx6_emit_framebuffer_state, GFX9 branch): 15 registers from CB_COLOR0_BASE */
    uint32_t info = (E_COLOR_8_8_8_8 << F3D_CB_COLOR0_INFO__FORMAT) | (E_NUMBER_UNORM << F3D_CB_COLOR0_INFO__NUMBER_TYPE) |
                    (swap << F3D_CB_COLOR0_INFO__COMP_SWAP) | (1u << F3D_CB_COLOR0_INFO__BLEND_CLAMP) |
                    (1u << F3D_CB_COLOR0_INFO__SIMPLE_FLOAT);
    uint32_t attrib = (force_a1 << F3D_CB_COLOR0_ATTRIB__FORCE_DST_ALPHA_1) | (E_RESOURCE_2D << F3D_CB_COLOR0_ATTRIB__RESOURCE_TYPE) |
                      (1u << F3D_CB_COLOR0_ATTRIB__RB_ALIGNED) | (1u << F3D_CB_COLOR0_ATTRIB__PIPE_ALIGNED); /* COLOR_SW_MODE 0 = linear */
    uint32_t attrib2 = ((job->width - 1) << F3D_CB_COLOR0_ATTRIB2__MIP0_WIDTH) | ((job->height - 1) << F3D_CB_COLOR0_ATTRIB2__MIP0_HEIGHT);
    uint32_t dcc = (E_MAX_BLOCK_SIZE_256B << F3D_CB_COLOR0_DCC_CONTROL__MAX_UNCOMPRESSED_BLOCK_SIZE) |
                   (1u << F3D_CB_COLOR0_DCC_CONTROL__INDEPENDENT_64B_BLOCKS);
    put(&b, PKT3(OP_SET_CONTEXT_REG, 15));
    put(&b, R3D_CB_COLOR0_BASE);
    put(&b, (uint32_t)(job->rt_mc >> 8));         /* CB_COLOR0_BASE */
    put(&b, (uint32_t)(job->rt_mc >> 40) & 0xFFu); /* CB_COLOR0_BASE_EXT */
    put(&b, attrib2);                              /* CB_COLOR0_ATTRIB2 */
    put(&b, 0);                                    /* CB_COLOR0_VIEW: slice 0, mip 0 */
    put(&b, info);                                 /* CB_COLOR0_INFO */
    put(&b, attrib);                               /* CB_COLOR0_ATTRIB */
    put(&b, dcc);                                  /* CB_COLOR0_DCC_CONTROL */
    put(&b, 0);                                    /* CB_COLOR0_CMASK */
    put(&b, 0);                                    /* CB_COLOR0_CMASK_BASE_EXT */
    put(&b, 0);                                    /* CB_COLOR0_FMASK */
    put(&b, 0);                                    /* CB_COLOR0_FMASK_BASE_EXT */
    put(&b, 0);                                    /* CB_COLOR0_CLEAR_WORD0 */
    put(&b, 0);                                    /* CB_COLOR0_CLEAR_WORD1 */
    put(&b, 0);                                    /* CB_COLOR0_DCC_BASE */
    put(&b, 0);                                    /* CB_COLOR0_DCC_BASE_EXT */
    ctx1(&b, R3D_CB_MRT0_EPITCH, (job->pitch_px - 1) << F3D_CB_MRT0_EPITCH__EPITCH);
    ctx2(&b, R3D_DB_Z_INFO, 0, 0);                 /* no depth / stencil buffer: Z_INVALID, STENCIL_INVALID (both 0) */
    ctx1(&b, R3D_PA_SC_WINDOW_SCISSOR_BR, (job->width << F3D_PA_SC_WINDOW_SCISSOR_BR__BR_X) | (job->height << F3D_PA_SC_WINDOW_SCISSOR_BR__BR_Y));
    ctx2(&b, R3D_PA_SC_VPORT_SCISSOR_0_TL, (job->x0 << F3D_PA_SC_VPORT_SCISSOR_0_TL__TL_X) | (job->y0 << F3D_PA_SC_VPORT_SCISSOR_0_TL__TL_Y) |
                                           (1u << F3D_PA_SC_VPORT_SCISSOR_0_TL__WINDOW_OFFSET_DISABLE),
         (job->x1 << F3D_PA_SC_VPORT_SCISSOR_0_BR__BR_X) | (job->y1 << F3D_PA_SC_VPORT_SCISSOR_0_BR__BR_Y));
    ctx2(&b, R3D_PA_SC_VPORT_ZMIN_0, 0, 0x3F800000u); /* depth range 0.0 .. 1.0 */

    /* Clipping, guard band and sub-pixel precision for a vertex shader that writes window space positions (si_state.c
     * si_emit_clip_regs(): CLIP_DISABLE; si_state_viewport.c si_emit_guardband() with ac_guardband.c ac_compute_guardband(): the
     * "blit" case, quantisation 16.8 fixed point, the viewport taken to be the whole colour buffer; si_state.c: the rasteriser's
     * PA_SC_MODE_CNTL_0 always has VPORT_SCISSOR_ENABLE). With the reset values of the guard band registers (and a 1/16 pixel
     * precision) triangles far from the origin were clipped away. */
    {
        uint32_t hx = ((job->width / 2u) > 8176u ? 8176u : (job->width / 2u)) & ~15u;   /* HW screen offset, aligned to 16 */
        uint32_t hy = ((job->height / 2u) > 8176u ? 8176u : (job->height / 2u)) & ~15u;
        /* guard band = min(-left, right) with max_range = 65536 / 2, as a ratio num / (width or height), at least 1.0 */
        uint32_t ax = 65538u + job->width - 2u * hx, bx = 65536u - job->width + 2u * hx;
        uint32_t ay = 65538u + job->height - 2u * hy, by = 65536u - job->height + 2u * hy;
        uint32_t nx = ax < bx ? ax : bx, ny = ay < by ? ay : by;
        uint32_t gbx = nx < job->width ? 0x3F800000u : a3d_f32_ratio(nx, job->width);
        uint32_t gby = ny < job->height ? 0x3F800000u : a3d_f32_ratio(ny, job->height);

        ctx1(&b, R3D_PA_CL_CLIP_CNTL, (1u << F3D_PA_CL_CLIP_CNTL__CLIP_DISABLE) | (1u << F3D_PA_CL_CLIP_CNTL__DX_LINEAR_ATTR_CLIP_ENA));
        put(&b, PKT3(OP_SET_CONTEXT_REG, 5));
        put(&b, R3D_PA_SU_VTX_CNTL);
        put(&b, (1u << F3D_PA_SU_VTX_CNTL__PIX_CENTER) | (E_X_ROUND_TO_EVEN << F3D_PA_SU_VTX_CNTL__ROUND_MODE) |
                (E_X_16_8_FIXED_POINT_1_256TH << F3D_PA_SU_VTX_CNTL__QUANT_MODE));
        put(&b, gby);                              /* PA_CL_GB_VERT_CLIP_ADJ */
        put(&b, 0x3F800000u);                      /* PA_CL_GB_VERT_DISC_ADJ: discard = 1.0 (+ nothing for the point size) */
        put(&b, gbx);                              /* PA_CL_GB_HORZ_CLIP_ADJ */
        put(&b, 0x3F800000u);                      /* PA_CL_GB_HORZ_DISC_ADJ */
        ctx1(&b, R3D_PA_SU_HARDWARE_SCREEN_OFFSET, ((hx >> 4) << F3D_PA_SU_HARDWARE_SCREEN_OFFSET__HW_SCREEN_OFFSET_X) |
                                                   ((hy >> 4) << F3D_PA_SU_HARDWARE_SCREEN_OFFSET__HW_SCREEN_OFFSET_Y));
        ctx1(&b, R3D_PA_SC_MODE_CNTL_0, (1u << F3D_PA_SC_MODE_CNTL_0__VPORT_SCISSOR_ENABLE) |
                                        (1u << F3D_PA_SC_MODE_CNTL_0__ALTERNATE_RBS_PER_TILE));
    }

    /* 5. colour output: no blending, all four channels, RB+ down-conversion for 8-8-8-8 */
    ctx1(&b, R3D_CB_COLOR_CONTROL, (E_CB_NORMAL << F3D_CB_COLOR_CONTROL__MODE) | (0xCCu << F3D_CB_COLOR_CONTROL__ROP3));
    ctx1(&b, R3D_CB_TARGET_MASK, 0xF);
    ctx1(&b, R3D_CB_SHADER_MASK, 0xF);
    ctx1(&b, R3D_CB_BLEND0_CONTROL, 0);
    ctx1(&b, R3D_CB_DCC_CONTROL, (1u << F3D_CB_DCC_CONTROL__OVERWRITE_COMBINER_MRT_SHARING_DISABLE) |
                                 (4u << F3D_CB_DCC_CONTROL__OVERWRITE_COMBINER_WATERMARK));
    ctx1(&b, R3D_SX_MRT0_BLEND_OPT, (E_OPT_COMB_BLEND_DISABLED << F3D_SX_MRT0_BLEND_OPT__COLOR_COMB_FCN) |
                                    (E_OPT_COMB_BLEND_DISABLED << F3D_SX_MRT0_BLEND_OPT__ALPHA_COMB_FCN));
    put(&b, PKT3(OP_SET_CONTEXT_REG, 3));
    put(&b, R3D_SX_PS_DOWNCONVERT);
    put(&b, E_SX_RT_EXPORT_8_8_8_8);               /* SX_PS_DOWNCONVERT, MRT0 */
    put(&b, E_8BIT_FORMAT_0_5);                    /* SX_BLEND_OPT_EPSILON, MRT0 */
    put(&b, 0);                                    /* SX_BLEND_OPT_CONTROL */
    ctx1(&b, R3D_DB_DEPTH_CONTROL, 0);             /* no depth test */
    ctx1(&b, R3D_DB_SHADER_CONTROL, E_EARLY_Z_THEN_LATE_Z << F3D_DB_SHADER_CONTROL__Z_ORDER);

    /* 6. the pixel shader: no interpolated inputs, but one perspective VGPR input must be enabled (Mesa: "we need to enable at
     * least one of them, otherwise we hang the GPU"); one MRT, exported as FP16_ABGR */
    uint32_t ps_in = 1u << F3D_SPI_PS_INPUT_ENA__PERSP_CENTER_ENA;
    if (job->textured)  /* the pixel position, as floats, goes to the VGPRs after the two perspective ones: v2 = x, v3 = y */
        ps_in |= (1u << F3D_SPI_PS_INPUT_ENA__POS_X_FLOAT_ENA) | (1u << F3D_SPI_PS_INPUT_ENA__POS_Y_FLOAT_ENA);
    ctx2(&b, R3D_SPI_PS_INPUT_ENA, ps_in, ps_in);
    ctx1(&b, R3D_SPI_PS_IN_CONTROL, 0u << F3D_SPI_PS_IN_CONTROL__NUM_INTERP);
    ctx2(&b, R3D_SPI_SHADER_Z_FORMAT, 0, E_SPI_SHADER_FP16_ABGR);      /* Z: none; colour MRT0: FP16_ABGR */
    sh1(&b, R3D_SPI_SHADER_PGM_LO_PS, (uint32_t)(job->ps_mc >> 8));
    sh1(&b, R3D_SPI_SHADER_PGM_HI_PS, (uint32_t)(job->ps_mc >> 40) & 0xFFu);
    /* VGPRS / SGPRS are counted in blocks of 4 / 8, minus 1: the textured shader uses v0 .. v11 (12) and 8 user SGPRs + the system ones */
    sh1(&b, R3D_SPI_SHADER_PGM_RSRC1_PS, ((job->textured ? 2u : 0u) << F3D_SPI_SHADER_PGM_RSRC1_PS__VGPRS) |
                                         ((job->textured ? 2u : 1u) << F3D_SPI_SHADER_PGM_RSRC1_PS__SGPRS) |
                                         (0xC0u << F3D_SPI_SHADER_PGM_RSRC1_PS__FLOAT_MODE) | (1u << F3D_SPI_SHADER_PGM_RSRC1_PS__DX10_CLAMP));
    sh1(&b, R3D_SPI_SHADER_PGM_RSRC2_PS, (job->textured ? 8u : 0u) << F3D_SPI_SHADER_PGM_RSRC2_PS__USER_SGPR);
    if (job->textured) {          /* the image resource goes straight into the pixel shader's user SGPRs s0 .. s7 */
        uint32_t td[8], k;
        amd_3d_build_tex_desc(td, job);
        put(&b, PKT3(OP_SET_SH_REG, 8));
        put(&b, R3D_SPI_SHADER_USER_DATA_PS_0);
        for (k = 0; k < 8; k++)
            put(&b, td[k]);
    }

    /* 7. the vertex shader: window space position (pixels), one position export, no parameters */
    sh1(&b, R3D_SPI_SHADER_PGM_LO_VS, (uint32_t)(job->vs_mc >> 8));
    sh1(&b, R3D_SPI_SHADER_PGM_HI_VS, (uint32_t)(job->vs_mc >> 40) & 0xFFu);
    sh1(&b, R3D_SPI_SHADER_PGM_RSRC1_VS, (1u << F3D_SPI_SHADER_PGM_RSRC1_VS__VGPRS) | (1u << F3D_SPI_SHADER_PGM_RSRC1_VS__SGPRS) |
                                         (0xC0u << F3D_SPI_SHADER_PGM_RSRC1_VS__FLOAT_MODE) | (1u << F3D_SPI_SHADER_PGM_RSRC1_VS__DX10_CLAMP) |
                                         (0u << F3D_SPI_SHADER_PGM_RSRC1_VS__VGPR_COMP_CNT)); /* only the vertex number in v0 */
    sh1(&b, R3D_SPI_SHADER_PGM_RSRC2_VS, 0u << F3D_SPI_SHADER_PGM_RSRC2_VS__USER_SGPR);
    sh1(&b, R3D_SPI_SHADER_PGM_RSRC3_VS, (0xFFFFu << F3D_SPI_SHADER_PGM_RSRC3_VS__CU_EN) | (0x3Fu << F3D_SPI_SHADER_PGM_RSRC3_VS__WAVE_LIMIT));
    sh1(&b, R3D_SPI_SHADER_LATE_ALLOC_VS, 0u << F3D_SPI_SHADER_LATE_ALLOC_VS__LIMIT);
    ctx1(&b, R3D_SPI_VS_OUT_CONFIG, 0u << F3D_SPI_VS_OUT_CONFIG__VS_EXPORT_COUNT);   /* one parameter slot: count - 1 = 0 */
    ctx1(&b, R3D_SPI_SHADER_POS_FORMAT, E_SPI_SHADER_4COMP << F3D_SPI_SHADER_POS_FORMAT__POS0_EXPORT_FORMAT);
    ctx1(&b, R3D_PA_CL_VTE_CNTL, (1u << F3D_PA_CL_VTE_CNTL__VTX_XY_FMT) | (1u << F3D_PA_CL_VTE_CNTL__VTX_Z_FMT));
    ctx1(&b, R3D_PA_CL_VS_OUT_CNTL, 0);
    ctx1(&b, R3D_VGT_GS_MODE, 0);                  /* GS_OFF */
    ctx1(&b, R3D_VGT_SHADER_STAGES_EN, 2u << F3D_VGT_SHADER_STAGES_EN__MAX_PRIMGRP_IN_WAVE);

    /* 8. the draw: 4 vertices, triangle strip, vertex numbers generated by the hardware. With a marker address the stream writes
     * 0x3D3D0001 there just before the draw and 0x3D3D0002 (4 bytes on) just after it: the CPU can then tell a stream that never ran
     * from a draw that ran and drew nothing. */
    if (job->marker_mc)
        write_data(&b, job->marker_mc, 0x3D3D0001u);
    uc1(&b, R3D_IA_MULTI_VGT_PARAM, (127u << F3D_IA_MULTI_VGT_PARAM__PRIMGROUP_SIZE) | (1u << F3D_IA_MULTI_VGT_PARAM__EN_INST_OPT_BASIC) |
                                    (1u << F3D_IA_MULTI_VGT_PARAM__EN_INST_OPT_ADV));
    uc1(&b, R3D_VGT_PRIMITIVE_TYPE, E_DI_PT_TRISTRIP);
    put(&b, PKT3(OP_NUM_INSTANCES, 0));
    put(&b, 1);
    put(&b, PKT3(OP_DRAW_INDEX_AUTO, 1));
    put(&b, 4);                                    /* vertex count */
    put(&b, E_DI_SRC_SEL_AUTO_INDEX);              /* VGT_DRAW_INITIATOR */
    if (job->marker_mc)
        write_data(&b, job->marker_mc + 4u, 0x3D3D0002u);

    /* 9. flush the colour buffer and WAIT for it, as Mesa does (ac_barrier.c, SYNC_AND_INV_CB: EVENT_WRITE FLUSH_AND_INV_CB_META, then
     * a RELEASE_MEM with FLUSH_AND_INV_CB_DATA_TS, then WAIT_REG_MEM on what it wrote). The fence of amd_gpu_submit_ib() comes right
     * after this stream and is only a generic event: a write the colour buffer still holds in its cache, or a fence that fires
     * before the pixels are out, would look exactly like "the draw ran and drew nothing". */
    if (job->marker_mc) {
        uint64_t m3 = job->marker_mc + 8u;
        put(&b, PKT3(OP_EVENT_WRITE, 0));
        put(&b, E_EVENT_FLUSH_AND_INV_CB_META);
        put(&b, PKT3(OP_RELEASE_MEM, 6));
        put(&b, E_EVENT_FLUSH_AND_INV_CB_DATA_TS | (5u << 8) | (1u << 15) | (1u << 19)); /* same cache bits as the fence */
        put(&b, 1u << 29);                             /* DATA_SEL 1: write the 32-bit value below to memory */
        put(&b, (uint32_t)m3);
        put(&b, (uint32_t)(m3 >> 32));
        put(&b, 0x3D3D0003u);
        put(&b, 0);
        put(&b, 0);
        put(&b, PKT3(OP_WAIT_REG_MEM, 5));
        put(&b, 3u | (1u << 4));                       /* function EQUAL, memory space */
        put(&b, (uint32_t)m3);
        put(&b, (uint32_t)(m3 >> 32));
        put(&b, 0x3D3D0003u);
        put(&b, 0xFFFFFFFFu);
        put(&b, 4);                                    /* poll interval */
    }

    return b.over ? 0 : b.n;
}

/* ---- running it ---- */
#define VRAM_SHADER_OFF 0x0B000000u /* 176 MiB: between the CP ring (160 MiB) and the PSP TMR (192 MiB); the CPU already reaches 160 MiB */
#define VRAM_RT_OFF 0x0B100000u
#define VRAM_MARK_OFF 0x0B300000u
#define RT_W 256u
#define RT_H 64u

static const char *status = "3D test not run";
static uint32_t val[AMD_3D_VALUES];

const char *amd_3d_status(void) { return status; }
uint32_t amd_3d_value(int what) { return (what >= 0 && what < AMD_3D_VALUES) ? val[what] : 0; }


/* Submits the command stream now in the IB page and waits for its fence. Returns 0 if the GPU finished it. */
static int submit_and_wait(uint32_t ndw, const char *what_failed_submit, const char *what_failed_fence)
{
    uint32_t seq = amd_gpu_submit_ib(amd_gpu_ib_va(), ndw);
    if (!seq) { status = what_failed_submit; return 2; }
    val[7] = seq;
    if (!amd_gpu_fence_wait(seq, 8000000u)) { status = what_failed_fence; return 3; }
    return 0;
}

/* Switches of the stress test (amd_3d_stress): leave the shader code in VRAM as it is / wait after writing it. */
static int skip_shader_write;
static uint32_t shader_delay_reads;

/* Writes the shaders, runs the default state stream, then the draw stream, waits for the fences and reads some registers back
 * (the CP has run SET_CONTEXT_REG / SET_SH_REG for them, so a read shows whether the packets took effect).
 * Returns 0 if the GPU did run both. */
static int run_job(const Amd3dJob *job_in, uint32_t vs_off, uint32_t ps_off)
{
    Amd3dJob job = *job_in;
    static uint32_t ib[AMD_3D_DEFAULT_IB_MAX];
    uint32_t vs[AMD_3D_VS_MAX], ps[AMD_3D_PS_MAX];
    uint32_t i, nvs, nps, nib;
    int rc;

    nvs = amd_3d_build_vs(vs, &job);
    nps = amd_3d_build_ps(ps, &job);

    if (!skip_shader_write) {
        /* shader code, then zero padding (the shader fetch reads ahead of the end) */
        for (i = 0; i < 64u; i++) amd_gpu_vram_write32(vs_off + 4u * i, i < nvs ? vs[i] : 0u);
        for (i = 0; i < 64u; i++) amd_gpu_vram_write32(ps_off + 4u * i, i < nps ? ps[i] : 0u);

        amd_gpu_wc_flush();   /* the shader code is in memory before any command that may fetch it */
        for (i = 0; i < shader_delay_reads; i++)
            (void)amd_gpu_vram_read32(0);
    }

    /* 1. the default context state */
    nib = amd_3d_build_default_ib(ib, AMD_3D_DEFAULT_IB_MAX);
    if (!nib) { status = "3D test skipped: default state stream does not fit"; return 1; }
    for (i = 0; i < nib; i++) amd_gpu_ib_write32(i, ib[i]);
    amd_gpu_ib_flush();
    val[8] = 3;
    rc = submit_and_wait(nib, "3D test FAILED: the ring did not take the default state stream",
                         "3D test FAILED: fence timeout in the default state stream");
    if (rc) return rc;

    /* 2. the draw */
    nib = amd_3d_build_ib(ib, AMD_3D_IB_MAX, &job);
    if (!nib) { status = "3D test skipped: command stream does not fit"; return 1; }
    val[6] = nib;
    for (i = 0; i < nib; i++) amd_gpu_ib_write32(i, ib[i]);
    amd_gpu_ib_flush();
    val[8] = 4;
    rc = submit_and_wait(nib, "3D test FAILED: the ring did not take the command stream",
                         "3D test FAILED: fence timeout (the GPU did not finish the draw)");
    if (rc) return rc;

    val[9]  = amd_gpu_reg_read32(0xA000u + R3D_CB_COLOR0_BASE);             /* CB_COLOR0_BASE */
    val[10] = amd_gpu_reg_read32(0xA000u + R3D_CB_COLOR0_BASE + 4u);        /* CB_COLOR0_INFO */
    val[11] = amd_gpu_reg_read32(0xA000u + R3D_PA_CL_GB_VERT_CLIP_ADJ);     /* 0x3F800000 once the default state is in */
    val[12] = amd_gpu_reg_read32(0x2C00u + R3D_SPI_SHADER_PGM_LO_PS);
    val[13] = amd_gpu_reg_read32(0x2C00u + R3D_SPI_SHADER_PGM_LO_VS);
    val[14] = amd_gpu_reg_read32(0x2004u);                                  /* GRBM_STATUS */
    val[15] = amd_gpu_reg_read32(0xA000u + R3D_PA_CL_VTE_CNTL);
    for (i = 0; i < 8; i++)                                                  /* the image resource in the pixel shader's user SGPRs */
        val[16 + i] = amd_gpu_reg_read32(0x2C00u + R3D_SPI_SHADER_USER_DATA_PS_0 + i);
    val[24] = amd_gpu_reg_read32(0x2C00u + R3D_SPI_SHADER_PGM_RSRC1_PS);
    val[25] = amd_gpu_reg_read32(0x2C00u + R3D_SPI_SHADER_PGM_RSRC2_PS);
    return 0;
}

/* Fills the colour buffer with `pattern`, runs the job, checks the rectangle (colour) and everything else (unchanged). The area
 * looked at is the rectangle grown by `margin` pixels; the buffer starts at VRAM offset base_off. */
static char map[16][33];  /* a coarse picture of the offscreen buffer after the draw: # colour, . untouched, ? something else */

const char *amd_3d_map_row(int row) { return (row >= 0 && row < 16 && map[row][0]) ? map[row] : 0; }

/* two 8 bit channel triples differ by at most 1 in each channel (the colour goes through half floats and back) */
static int close_px(uint32_t a, uint32_t b)
{
    int s;
    for (s = 0; s < 24; s += 8) {
        int d = (int)((a >> s) & 0xFFu) - (int)((b >> s) & 0xFFu);
        if (d > 1 || d < -1) return 0;
    }
    return 1;
}

static int draw_once(Amd3dJob *job, uint32_t base_off, uint32_t margin, uint32_t pattern, int want_map);

/* How many pixels inside the rectangle are right now (the colour for a solid job, the texel for a textured one). */
static uint32_t inside_good(const Amd3dJob *job, uint32_t base_off)
{
    uint32_t x, y, good = 0;
    uint32_t solid = job->bgr ? ((job->r << 16) | (job->g << 8) | job->b) : ((job->b << 16) | (job->g << 8) | job->r);
    for (y = job->y0; y < job->y1; y++)
        for (x = job->x0; x < job->x1; x++) {
            uint32_t v = amd_gpu_vram_read32(base_off + (y * job->pitch_px + x) * 4u) & 0x00FFFFFFu;
            if (job->textured ? close_px(v, amd_3d_texel(job, x - job->x0, y - job->y0) & 0x00FFFFFFu) : v == solid) good++;
        }
    return good;
}

/* The first textured draw of a boot got no pixels on one run although the same draw works afterwards: a textured job is run a second
 * time when the first try failed (val[27] = number of tries, +0x100 if the last one passed). */
static int draw_and_check(Amd3dJob *job, uint32_t base_off, uint32_t margin, uint32_t pattern, int want_map)
{
    int ok = draw_once(job, base_off, margin, pattern, want_map);
    uint32_t tries = 1, f_gc = 0, f_lo = 0, f_hi = 0, f_mm = 0;
    if (!ok) {
        amd_gpu_fault_snapshot();            /* did the first try end in a VM fault? */
        f_gc = amd_native_fault_gc_status(); f_lo = amd_native_fault_gc_lo(); f_hi = amd_native_fault_gc_hi(); f_mm = amd_native_fault_mm_status();
        ok = draw_once(job, base_off, margin, pattern, want_map);
        tries = 2;
    }
    val[27] = tries | (ok ? 0x100u : 0u);
    val[28] = f_gc; val[29] = f_lo; val[30] = f_hi; val[31] = f_mm;   /* fault registers after a failed first try */
    return ok;
}

static int draw_once(Amd3dJob *job, uint32_t base_off, uint32_t margin, uint32_t pattern, int want_map)
{
    uint32_t cx0 = job->x0 > margin ? job->x0 - margin : 0, cy0 = job->y0 > margin ? job->y0 - margin : 0;
    uint32_t cx1 = job->x1 + margin > job->width ? job->width : job->x1 + margin;
    uint32_t cy1 = job->y1 + margin > job->height ? job->height : job->y1 + margin;
    uint32_t x, y, expect, in_good = 0, in_total = 0, out_bad = 0, out_total = 0;
    uint32_t first_in = 0, first_out = 0;
    uint32_t px_r = job->r, px_g = job->g, px_b = job->b;

    { uint32_t k; for (k = 0; k < AMD_3D_VALUES; k++) val[k] = 0; }
    val[8] = 1;
    for (y = cy0; y < cy1; y++)
        for (x = cx0; x < cx1; x++)
            amd_gpu_vram_write32(base_off + (y * job->pitch_px + x) * 4u, pattern);
    if (job->textured)                                         /* the image the pixel shader will read */
        for (y = 0; y < job->tex_h; y++)
            for (x = 0; x < job->tex_w; x++)
                amd_gpu_vram_write32(job->tex_off + (y * job->tex_pitch_px + x) * 4u, amd_3d_texel(job, x, y));
    amd_gpu_wc_flush();                                        /* drain the CPU's write buffers: pattern and image are in memory */

    val[8] = 2;
    job->marker_mc = amd_native_vram_mc_addr(VRAM_MARK_OFF, 16u);
    amd_gpu_vram_write32(VRAM_MARK_OFF, 0); amd_gpu_vram_write32(VRAM_MARK_OFF + 4u, 0); amd_gpu_vram_write32(VRAM_MARK_OFF + 8u, 0);
    amd_gpu_wc_flush();
    if (run_job(job, VRAM_SHADER_OFF, VRAM_SHADER_OFF + 0x100u))
        return 0;
    val[32] = amd_gpu_vram_read32(VRAM_MARK_OFF);          /* 0x3D3D0001 if the stream got to the draw */
    val[33] = amd_gpu_vram_read32(VRAM_MARK_OFF + 4u);     /* 0x3D3D0002 if it got past it */

    expect = (px_r << 16) | (px_g << 8) | px_b;               /* memory dword 0x00RRGGBB, the X byte is not compared */
    if (!job->bgr)
        expect = (px_b << 16) | (px_g << 8) | px_r;
    for (y = cy0; y < cy1; y++)
        for (x = cx0; x < cx1; x++) {
            uint32_t v = amd_gpu_vram_read32(base_off + (y * job->pitch_px + x) * 4u) & 0x00FFFFFFu;
            if (x >= job->x0 && x < job->x1 && y >= job->y0 && y < job->y1) {
                in_total++;
                if (job->textured ? close_px(v, amd_3d_texel(job, x - job->x0, y - job->y0) & 0x00FFFFFFu) : v == expect) in_good++;
                else if (!first_in) first_in = v | 0x01000000u;
            } else {
                out_total++;
                if (v != (pattern & 0x00FFFFFFu)) {
                    out_bad++;
                    if (!first_out) first_out = v | 0x01000000u;
                }
            }
        }
    if (want_map) {
        uint32_t gx, gy;
        for (gy = 0; gy < 16u; gy++) {
            for (gx = 0; gx < 32u; gx++) {
                uint32_t v = amd_gpu_vram_read32(base_off + ((gy * 4u) * job->pitch_px + gx * 8u) * 4u) & 0x00FFFFFFu;
                map[gy][gx] = (job->textured ? (v != (pattern & 0x00FFFFFFu)) : v == expect) ? '#' : v == (pattern & 0x00FFFFFFu) ? '.' : '?';
            }
            map[gy][32] = 0;
        }
    }
    val[0] = in_good; val[1] = in_total; val[2] = out_bad; val[3] = out_total; val[4] = first_in; val[5] = first_out;
    val[8] = 5;
    if (!(in_total && in_good == in_total && !out_bad)) {
        /* Failed. Wait about 20 ms and count again, with nothing redrawn: if the rectangle is there by now, the GPU's pixels reached
         * memory later than the fence said (val[26] = right inside pixels at that time); if not, they never came or were overwritten. */
        uint32_t k;
        for (k = 0; k < 20000u; k++) (void)amd_gpu_vram_read32(0);
        val[26] = inside_good(job, base_off);
    }
    if (in_total && in_good == in_total && !out_bad) {
        status = job->textured ? "3D PASS: the pixel shader read the image and the rectangle shows it (every texel right, nothing outside touched)"
                               : "3D PASS: the graphics pipeline drew the rectangle (every pixel right, nothing outside touched)";
        return 1;
    }
    status = in_good == 0 ? "3D FAILED: the GPU ran the draw but the rectangle is not there (see the first wrong pixel)"
                          : "3D FAILED: some pixels are wrong";
    return 0;
}

int amd_3d_rect_offscreen(void)
{
    Amd3dJob job = { 0 };
    if (!amd_gpu_ring_running()) { status = "3D test skipped: the persistent ring is not running"; return 0; }
    job.rt_mc = amd_native_vram_mc_addr(VRAM_RT_OFF, RT_W * RT_H * 4u);
    job.vs_mc = amd_native_vram_mc_addr(VRAM_SHADER_OFF, 0x200u);
    job.ps_mc = amd_native_vram_mc_addr(VRAM_SHADER_OFF + 0x100u, 0x100u);
    if (!job.rt_mc || !job.vs_mc || !job.ps_mc) { status = "3D test skipped: no MC address for the test memory"; return 0; }
    job.pitch_px = RT_W; job.width = RT_W; job.height = RT_H;
    job.bgr = 1;
    job.x0 = 32; job.y0 = 16; job.x1 = 224; job.y1 = 48;
    job.r = 255; job.g = 128; job.b = 0; job.a = 255;
    return draw_and_check(&job, VRAM_RT_OFF, 256, 0x00112233u, 1); /* the whole 256 x 64 buffer is looked at */
}

int amd_3d_rect_screen(uint64_t fb_phys, uint32_t pitch_bytes, uint32_t width, uint32_t height, uint64_t vram_cpu, int bgr)
{
    Amd3dJob job = { 0 };
    uint64_t off64;
    if (!amd_gpu_ring_running()) { status = "3D screen test skipped: the persistent ring is not running"; return 0; }
    if (!pitch_bytes || (pitch_bytes & 3u) || width < 600 || height < 200 || fb_phys < vram_cpu) {
        status = "3D screen test skipped: bad framebuffer info"; return 0; }
    off64 = fb_phys - vram_cpu;
    if (off64 + (uint64_t)pitch_bytes * height > (uint64_t)VRAM_SHADER_OFF) {
        status = "3D screen test skipped: the framebuffer reaches the test memory"; return 0; }
    job.rt_mc = amd_native_vram_mc_addr((uint32_t)off64, pitch_bytes * height);
    job.vs_mc = amd_native_vram_mc_addr(VRAM_SHADER_OFF, 0x200u);
    job.ps_mc = amd_native_vram_mc_addr(VRAM_SHADER_OFF + 0x100u, 0x100u);
    if (!job.rt_mc || !job.vs_mc || !job.ps_mc) { status = "3D screen test skipped: no MC address"; return 0; }
    job.pitch_px = pitch_bytes / 4u; job.width = width; job.height = height;
    job.bgr = bgr;
    job.x0 = width / 2 - 200; job.x1 = width / 2 + 200;
    job.y0 = height - 120; job.y1 = height - 88;
    job.r = 255; job.g = 128; job.b = 0; job.a = 255;
    return draw_and_check(&job, (uint32_t)off64, 8, 0x00112233u, 0);
}

/* ---- M2: the rectangle textured from an image in VRAM ---- */
#define VRAM_TEX_OFF 0x0B200000u

int amd_3d_tex_offscreen(void)
{
    Amd3dJob job;
    if (!amd_gpu_ring_running()) { status = "3D texture test skipped: the persistent ring is not running"; return 0; }
    job.rt_mc = amd_native_vram_mc_addr(VRAM_RT_OFF, RT_W * RT_H * 4u);
    job.vs_mc = amd_native_vram_mc_addr(VRAM_SHADER_OFF, 0x200u);
    job.ps_mc = amd_native_vram_mc_addr(VRAM_SHADER_OFF + 0x100u, 0x100u);
    job.tex_off = VRAM_TEX_OFF;
    job.tex_mc = amd_native_vram_mc_addr(VRAM_TEX_OFF, 256u * 32u * 4u);
    if (!job.rt_mc || !job.vs_mc || !job.ps_mc || !job.tex_mc) { status = "3D texture test skipped: no MC address for the test memory"; return 0; }
    job.pitch_px = RT_W; job.width = RT_W; job.height = RT_H;
    job.bgr = 1;
    job.x0 = 32; job.y0 = 16; job.x1 = 224; job.y1 = 48;
    job.r = job.g = job.b = job.a = 0;
    job.textured = 1;
    job.tex_w = job.x1 - job.x0; job.tex_h = job.y1 - job.y0; job.tex_pitch_px = 256;
    return draw_and_check(&job, VRAM_RT_OFF, 256, 0x00112233u, 1);
}

int amd_3d_tex_screen(uint64_t fb_phys, uint32_t pitch_bytes, uint32_t width, uint32_t height, uint64_t vram_cpu, int bgr)
{
    Amd3dJob job;
    uint64_t off64;
    if (!amd_gpu_ring_running()) { status = "3D texture screen test skipped: the persistent ring is not running"; return 0; }
    if (!pitch_bytes || (pitch_bytes & 3u) || width < 600 || height < 200 || fb_phys < vram_cpu) {
        status = "3D texture screen test skipped: bad framebuffer info"; return 0; }
    off64 = fb_phys - vram_cpu;
    if (off64 + (uint64_t)pitch_bytes * height > (uint64_t)VRAM_SHADER_OFF) {
        status = "3D texture screen test skipped: the framebuffer reaches the test memory"; return 0; }
    job.rt_mc = amd_native_vram_mc_addr((uint32_t)off64, pitch_bytes * height);
    job.vs_mc = amd_native_vram_mc_addr(VRAM_SHADER_OFF, 0x200u);
    job.ps_mc = amd_native_vram_mc_addr(VRAM_SHADER_OFF + 0x100u, 0x100u);
    job.tex_off = VRAM_TEX_OFF;
    job.tex_mc = amd_native_vram_mc_addr(VRAM_TEX_OFF, 512u * 64u * 4u);
    if (!job.rt_mc || !job.vs_mc || !job.ps_mc || !job.tex_mc) { status = "3D texture screen test skipped: no MC address"; return 0; }
    job.pitch_px = pitch_bytes / 4u; job.width = width; job.height = height;
    job.bgr = bgr;
    job.x0 = width / 2 - 200; job.x1 = width / 2 + 200;
    job.y0 = height - 120; job.y1 = height - 88;
    job.r = job.g = job.b = job.a = 0;
    job.textured = 1;
    job.tex_w = job.x1 - job.x0; job.tex_h = job.y1 - job.y0; job.tex_pitch_px = 512;
    return draw_and_check(&job, (uint32_t)off64, 8, 0x00112233u, 0);
}

/* A stress test for the failures that came and went between boots (a draw that ends with no pixels at all, no fault): the same solid
 * rectangle 8 times in each of three modes. bits[m] has bit i set if try i drew it right.
 *   mode 0: the shader code is written once, then left alone
 *   mode 1: the shader code is written again before every draw (the normal case)
 *   mode 2: as mode 1, plus a wait of about 20 ms after writing it */
static uint32_t stress_combo[3];
uint32_t amd_3d_stress_combo(int mode) { return mode >= 0 && mode < 3 ? stress_combo[mode] : 0; }

void amd_3d_stress(uint32_t bits[3])
{
    Amd3dJob job = { 0 };
    uint32_t mode, i;
    bits[0] = bits[1] = bits[2] = 0xFFFFFFFFu;
    stress_combo[0] = stress_combo[1] = stress_combo[2] = 0;
    if (!amd_gpu_ring_running()) return;
    job.rt_mc = amd_native_vram_mc_addr(VRAM_RT_OFF, RT_W * RT_H * 4u);
    job.vs_mc = amd_native_vram_mc_addr(VRAM_SHADER_OFF, 0x200u);
    job.ps_mc = amd_native_vram_mc_addr(VRAM_SHADER_OFF + 0x100u, 0x100u);
    if (!job.rt_mc || !job.vs_mc || !job.ps_mc) return;
    job.pitch_px = RT_W; job.width = RT_W; job.height = RT_H;
    job.bgr = 1;
    job.x0 = 32; job.y0 = 16; job.x1 = 224; job.y1 = 48;
    job.r = 255; job.g = 128; job.b = 0; job.a = 255;
    for (mode = 0; mode < 3; mode++) {
        bits[mode] = 0;
        shader_delay_reads = mode == 2 ? 20000u : 0u;
        for (i = 0; i < 8; i++) {
            Amd3dJob j = job;
            skip_shader_write = (mode == 0 && i > 0);
            /* one try only: the retry would hide exactly what is being measured */
            if (draw_once(&j, VRAM_RT_OFF, 8, 0x00112233u, 0))
                bits[mode] |= 1u << i;
            else {
                if (val[26] == val[1])         /* failed, but the whole rectangle was there 20 ms later */
                    bits[mode] |= 0x10000u << i;
                /* what the stream did: 0 neither marker, 1 only before the draw, 2 only after, 3 both */
                stress_combo[mode] += 1u << (4u * ((val[32] == 0x3D3D0001u ? 1u : 0u) | (val[33] == 0x3D3D0002u ? 2u : 0u)));
            }
        }
    }
    skip_shader_write = 0;
    shader_delay_reads = 0;
}
