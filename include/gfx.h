/* ============================================================
 * HarisOS graphics layer (gfx)
 *
 * A small, reusable 2D drawing library on top of a linear 32-bit
 * framebuffer. It knows nothing about UEFI, the desktop or any widgets: any
 * code (desktop, window manager, boot screen, panic screen) can call it.
 *
 *   gfx_init(...)            once, with the framebuffer from the bootloader
 *   gfx_fill_rect(...) ...   drawing primitives
 *   font.h                   smooth text (gfx_text)
 *   ui.h                     ready-made widgets built on top of this layer
 *
 * Everything curved (rounded rectangles, circles, lines, rings) is
 * ANTI-ALIASED: edges are blended with the background so they look smooth
 * instead of jagged. All shapes accept an alpha value for transparency.
 *
 * Coordinates are pixels, (0,0) is the top-left corner. Every call is clipped
 * to the screen and to the optional clip rectangle, so drawing outside the
 * visible area is always safe.
 *
 * Functions ending in _q take "Q4" coordinates: 1/16 of a pixel
 * (16 = one pixel). Use them for sub-pixel accurate shapes such as icons.
 * ============================================================ */
#ifndef HARIS_GFX_H
#define HARIS_GFX_H

/* Packed pixel in the framebuffer's native format. Create it with gfx_rgb(). */
typedef unsigned int GfxColor;

#define GFX_Q 16 /* Q4 fixed point: GFX_Q units = 1 pixel */

/* ---- setup -------------------------------------------------- */
/* red_mask_mode != 0: pixel layout is 0x00RRGGBB, otherwise 0x00BBGGRR.
 * pitch is the number of pixels per scanline (may be larger than width). */
void gfx_init(void *framebuffer, unsigned int width, unsigned int height,
              unsigned int pitch, unsigned int red_mask_mode);
int gfx_ready(void);
unsigned int gfx_width(void);
unsigned int gfx_height(void);

/* ---- double buffering ---------------------------------------
 * Draw into a back buffer in ordinary RAM and copy the finished picture to
 * the screen in one go with gfx_present(). Screen memory is slow and you
 * see half-drawn frames when drawing straight to it; the back buffer avoids
 * both. gfx tracks the changed area, so gfx_present() copies only that.
 *
 *   unsigned long long bytes = gfx_backbuffer_size();
 *   void *mem = ...;                 // at least `bytes` bytes of RAM
 *   gfx_enable_backbuffer(mem);      // from now on drawing goes to mem
 *   ...draw...
 *   gfx_present();                   // make it visible
 *
 * Without a back buffer everything still works (drawing goes straight to
 * the screen) and gfx_present() does nothing. */
unsigned long long gfx_backbuffer_size(void); /* width * height * 4 bytes */
int gfx_enable_backbuffer(void *memory);      /* 0 = ok; copies the current screen in */
void gfx_present(void);                       /* copy changed area to the screen */
void gfx_disable_backbuffer(void);            /* present, then draw directly again */
int gfx_double_buffered(void);

/* ---- brightness ---------------------------------------------
 * Software dimming of what gfx_present() sends to the screen (needs the back buffer; returns -1
 * without it). percent is clamped to GFX_BRIGHTNESS_MIN..GFX_BRIGHTNESS_MAX so the screen never
 * goes fully black. The picture is redrawn at once. */
#define GFX_BRIGHTNESS_MIN 10
#define GFX_BRIGHTNESS_MAX 100
int gfx_set_brightness(int percent);
int gfx_brightness(void);

/* ---- background cache ---------------------------------------
 */
int gfx_cache_enable(void *memory); /* 0 = ok, -1 = no back buffer */
int gfx_cache_valid(void);          /* 1 = saved and usable */
void gfx_cache_invalidate(void);
void gfx_cache_save(void); /* copy the whole back buffer into the cache */
void gfx_cache_restore(int x, int y, int w, int h);

/* ---- mouse pointer ------------------------------------------
 * A smooth arrow drawn on top of everything, directly on the screen (never in
 * the back buffer), so moving it does not need a redraw of the scene.
 * gfx_present() keeps it on top automatically. When NOT double buffering,
 * wrap scene drawing in gfx_cursor_show(0) ... gfx_cursor_show(1). */
void gfx_cursor_show(int on);
void gfx_cursor_move(int x, int y); /* x,y = position of the arrow tip */

/* Pointer shape: the normal arrow, or a double arrow for resizing windows.
 * For the resize shapes (x, y) is the center of the cursor. */
#define GFX_CURSOR_ARROW 0
#define GFX_CURSOR_SIZE_H 1    /* left-right */
#define GFX_CURSOR_SIZE_V 2    /* up-down */
#define GFX_CURSOR_SIZE_NWSE 3 /* top-left <-> bottom-right */
#define GFX_CURSOR_SIZE_NESW 4 /* top-right <-> bottom-left */
void gfx_cursor_set_shape(int shape);

/* ---- colors ------------------------------------------------- */
GfxColor gfx_rgb(unsigned char r, unsigned char g, unsigned char b);
void gfx_unpack(GfxColor c, unsigned char *r, unsigned char *g, unsigned char *b);
GfxColor gfx_mix(GfxColor a, GfxColor b, unsigned char t); /* t: 0 = a, 255 = b */

/* ---- clipping ----------------------------------------------- */
/* Restrict all drawing to a rectangle (e.g. the inside of a window). */
void gfx_set_clip(int x, int y, int w, int h);
void gfx_reset_clip(void);
/* Shrink the clip to the overlap of the current clip and this rectangle. */
void gfx_clip_intersect(int x, int y, int w, int h);
/* Remember / restore the current clip (use around nested widgets). */
typedef struct
{
    int x0, y0, x1, y1;
} GfxClip;
GfxClip gfx_get_clip(void);
void gfx_restore_clip(GfxClip c);

/* ---- plain (pixel exact) primitives ------------------------- */
void gfx_clear(GfxColor c);
void gfx_pixel(int x, int y, GfxColor c);
void gfx_hline(int x, int y, int len, GfxColor c);
void gfx_vline(int x, int y, int len, GfxColor c);
void gfx_rect(int x, int y, int w, int h, GfxColor c); /* 1px outline */
void gfx_fill_rect(int x, int y, int w, int h, GfxColor c);

/* Blend one pixel: alpha 0 = nothing, 255 = solid. */
void gfx_blend_pixel(int x, int y, GfxColor c, unsigned char alpha);

/* Semi-transparent rectangle (alpha 0 = invisible, 255 = solid). */
void gfx_fill_rect_alpha(int x, int y, int w, int h,
                         unsigned char r, unsigned char g, unsigned char b,
                         unsigned char alpha);

/* ---- smooth (anti-aliased) shapes --------------------------- */
/* Rounded rectangle. radius is clamped to half the shorter side. */
void gfx_fill_round_rect(int x, int y, int w, int h, int radius, GfxColor c);
void gfx_fill_round_rect_alpha(int x, int y, int w, int h, int radius, GfxColor c, unsigned char alpha);
/* Outline of thickness `thick` pixels, drawn inside the given rectangle. */
void gfx_round_rect(int x, int y, int w, int h, int radius, int thick, GfxColor c);
void gfx_round_rect_alpha(int x, int y, int w, int h, int radius, int thick, GfxColor c, unsigned char alpha);

void gfx_fill_circle(int cx, int cy, int radius, GfxColor c);
void gfx_circle(int cx, int cy, int radius, GfxColor c); /* 1px ring */
/* Sub-pixel versions: center and radius in Q4. thick_q = ring thickness. */
void gfx_fill_circle_q(int cx_q, int cy_q, int r_q, GfxColor c, unsigned char alpha);
void gfx_ring_q(int cx_q, int cy_q, int r_q, int thick_q, GfxColor c, unsigned char alpha);

/* Line with round caps. width_q is the thickness in Q4 (e.g. 24 = 1.5 px). */
void gfx_line_q(int x0_q, int y0_q, int x1_q, int y1_q, int width_q, GfxColor c, unsigned char alpha);
void gfx_line(int x0, int y0, int x1, int y1, GfxColor c); /* smooth 1px line */

/* Filled convex polygon (triangle, quad, arrow...), up to 8 corners, Q4 coordinates:
 * pts = {x0,y0, x1,y1, ...}. Either winding direction works. */
void gfx_fill_poly_q(const int *pts, int count, GfxColor c, unsigned char alpha);

/* Soft drop shadow under a rounded rectangle (x,y,w,h = the shape that casts it).
 * Draw it BEFORE the shape: the area under the shape itself is left unshaded, so
 * the shape is expected to cover it. blur = pixels the shadow fades out over;
 * (dx,dy) = offset; strength = 0..255 darkness at the edge. */
void gfx_shadow(int x, int y, int w, int h, int radius, int blur, int dx, int dy, unsigned char strength);

/* Soft round light/glow: color fades from `alpha` at the center to 0 at the edge. */
void gfx_glow(int cx, int cy, int radius, GfxColor c, unsigned char alpha);

/* ---- gradients ---------------------------------------------- */
/* Vertical gradient from color 1 (top) to color 2 (bottom). */
void gfx_gradient_v(int x, int y, int w, int h,
                    unsigned char r1, unsigned char g1, unsigned char b1,
                    unsigned char r2, unsigned char g2, unsigned char b2);
/* Horizontal gradient from color 1 (left) to color 2 (right). */
void gfx_gradient_h(int x, int y, int w, int h,
                    unsigned char r1, unsigned char g1, unsigned char b1,
                    unsigned char r2, unsigned char g2, unsigned char b2);
/* Vertical gradient inside a rounded rectangle (buttons, bars). */
void gfx_fill_round_rect_gradient(int x, int y, int w, int h, int radius,
                                  GfxColor top, GfxColor bottom);

#endif
