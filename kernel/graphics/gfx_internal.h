/* Internal shared state for HarisOS graphics modules.
 * Not a public application API; include/gfx.h remains the public interface.
 */
#ifndef HARIS_GFX_INTERNAL_H
#define HARIS_GFX_INTERNAL_H

#include "gfx.h"
#if defined(__SSE2__)
#include <emmintrin.h>
#endif

/* Shared graphics state, private to kernel/graphics. */
extern volatile unsigned int *FB;
extern volatile unsigned int *DRAW;
extern int W, H, PITCH, DPITCH;
extern int BACK;
extern volatile unsigned int *CACHE;
extern int CACHE_OK;
extern int DX0, DY0, DX1, DY1;
void gfx_internal_dirty_reset(void);
extern int CUR_DRAWN, CUR_Y_SHARED;
extern int RED_FIRST;
void gfx_internal_cursor_reset_state(void);
void gfx_internal_buffer_init(void *framebuffer, int width, int height, int pitch);
void gfx_internal_mark_dirty(int x0, int y0, int x1, int y1);
void gfx_internal_blend_ptr(volatile unsigned int *p, int r, int g, int b, int alpha);
void gfx_internal_cursor_restore(void);
void gfx_internal_cursor_y_range(int *y0, int *y1); /* screen rows covered by the pointer (when drawn) */
void gfx_internal_cursor_paint(void);

/* Copies n 32-bit pixels. "rep movsl" moves whole blocks in one instruction (fast strings on real CPUs, one
 * helper call in an emulator), where a C loop with volatile pointers handles one pixel per few instructions. */
static inline void gfx_internal_copy32(volatile unsigned int *dst, const volatile unsigned int *src, int n)
{
    if (n <= 0)
        return;
#if defined(__x86_64__)
    __asm__ volatile("rep movsl" : "+D"(dst), "+S"(src), "+c"(n) : : "memory"); /* direction flag is clear */
#else
    for (int i = 0; i < n; i++)
        dst[i] = src[i];
#endif
}

/* Copies n pixels to the screen's memory. Video memory is slow per write (on many PCs every 4-byte store is its
 * own bus transaction), so this stores 16 bytes at a time with streaming stores, which the CPU can combine
 * into whole cache lines. Call gfx_internal_copy_fb_done() after the last copy. */
static inline void gfx_internal_copy_fb(volatile unsigned int *dst, const volatile unsigned int *src, int n)
{
#if defined(__SSE2__)
    while (n > 0 && ((unsigned long long)dst & 15))
    {
        *dst++ = *src++;
        n--;
    }
    __m128i *d = (__m128i *)(void *)dst;
    const __m128i *s = (const __m128i *)(const void *)src;
    for (; n >= 16; n -= 16, d += 4, s += 4)
    {
        __m128i a = _mm_loadu_si128(s), b = _mm_loadu_si128(s + 1);
        __m128i c = _mm_loadu_si128(s + 2), e = _mm_loadu_si128(s + 3);
        _mm_stream_si128(d, a);
        _mm_stream_si128(d + 1, b);
        _mm_stream_si128(d + 2, c);
        _mm_stream_si128(d + 3, e);
    }
    for (; n >= 4; n -= 4, d++, s++)
        _mm_stream_si128(d, _mm_loadu_si128(s));
    dst = (volatile unsigned int *)(void *)d;
    src = (const volatile unsigned int *)(const void *)s;
    while (n-- > 0)
        *dst++ = *src++;
#else
    gfx_internal_copy32(dst, src, n);
#endif
}

static inline void gfx_internal_copy_fb_done(void)
{
#if defined(__SSE2__)
    _mm_sfence(); /* streaming stores are weakly ordered: make them all visible */
#endif
}

int gfx_internal_brightness_scale(void); /* 0..256: factor for pixels written straight to the screen */

extern int CX0, CY0, CX1, CY1;

/* Clips [x0,x1) x [y0,y1) to the active clip rectangle.
 * Returns non-zero only when a drawable area remains.
 */
int gfx_internal_clip_box(int *x0, int *y0, int *x1, int *y1);

#endif
