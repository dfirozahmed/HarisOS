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

/* The screen itself (the firmware framebuffer, not the back buffer): for a driver that draws into it with hardware (AMD GPU).
 * gfx_screen_pitch_bytes() is the distance between two rows in bytes. gfx_repaint_all() sends the whole back buffer to the screen
 * again (after something else painted over it); without a back buffer it does nothing - redraw the desktop instead. */
volatile unsigned int *gfx_screen_base(void);
unsigned int gfx_screen_pitch_bytes(void);
void gfx_repaint_all(void);
/* 1: a pixel is 0x00RRGGBB (bytes B, G, R, X in memory), 0: 0x00BBGGRR. */
int gfx_screen_red_first(void);

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
const unsigned int *gfx_cache_pixels(void); /* the saved picture (W x H, W pixels per row), 0 when not valid */
void gfx_cache_invalidate(void);
void gfx_cache_save(void); /* copy the whole back buffer into the cache */
void gfx_cache_restore(int x, int y, int w, int h);

/* ---- scrolling -----------------------------------------------
 * Move the pixels of a rectangle of the back buffer up (dy < 0) or down (dy > 0) by |dy| rows, so a list that
 * scrolled only has to draw the rows that came into view. Returns 0 if done, -1 if there is no back buffer
 * or nothing can be kept (then redraw the whole rectangle). The rows that came into view hold old pixels. */
int gfx_scroll_rect(int x, int y, int w, int h, int dy);
/* A compositor that keeps the windows' pictures elsewhere (gfx.h "compositor") says so: while on, the back buffer does not
 * hold what is on the screen, and gfx_scroll_rect() returns -1. */
void gfx_backbuffer_detach(int on);

/* ---- layers ---------------------------------------------------
 * Draw something once into its own w*h pixel buffer, then paste it often (a window being dragged).
 *   gfx_layer_begin(buf, w, h, fill)  all drawing goes to buf; the "screen" is w*h with (0,0) at its corner
 *   ... draw as usual ...
 *   gfx_layer_end()                   back to the real screen
 * Drawing is done twice, once over black (fill 0) and once over white (fill 0xFFFFFF) into two buffers.
 * gfx_layer_pack() then turns the pair into one premultiplied layer: the colour is what was drawn over
 * black, the top byte is how much of the background still shows through (255 = nothing drawn there).
 * gfx_layer_blit() draws the layer at x,y (clipped), over what is already in the picture. */
int gfx_layer_begin(void *buf, int w, int h, unsigned fill);
/* The same, but the buffer's pixel (0, 0) is (x, y) of the screen: drawing uses screen coordinates as usual (so what a window
 * remembers while drawing, such as where its buttons and text boxes are, is right), and only [x, x+w) x [y, y+h) is drawn,
 * nothing left of or above the screen. _keep: the buffer is not filled first. */
int gfx_layer_begin_at(void *buf, int x, int y, int w, int h, unsigned fill);
int gfx_layer_begin_at_keep(void *buf, int x, int y, int w, int h);
void gfx_layer_end(void);
void gfx_layer_pack(void *over_black, const void *over_white, int w, int h); /* w*h pixels each; the layer is w wide */
void gfx_layer_blit(const void *layer, int lw, int lh, int x, int y);

/* ---- overlay: a GPU pastes a layer ----------------------------
 * A driver that can paste a packed layer onto the screen registers itself with gfx_overlay_driver(). While an
 * overlay is shown, every gfx_present() copies the back buffer to the screen except where the layer is: there
 * the picture goes to the driver's buffer (below), and the driver writes layer + picture, finished, onto the
 * screen, before the pointer is drawn. So the screen never shows the picture without the layer (it would
 * flicker at the edges while dragging), the layer is never in the back buffer, and the CPU never blends it.
 * The layer's opaque inside is left out of the copy. If the driver fails once, the CPU pastes the layer
 * itself from then on.
 *   upload(layer, lw, lh)        a new picture of the layer: copy it; 0 = done
 *   below(&pitch)                where the picture below the layer goes: lw x lh pixels in layer coordinates,
 *                                pitch pixels per row; 0 = failed
 *   draw(x, y, x0, y0, x1, y1)   with the layer's corner at x,y, write layer + below onto the screen, only
 *                                inside the box [x0,x1) x [y0,y1); 0 = done and finished
 * gfx_overlay_area(): where an overlay may be seen; things drawn later (a taskbar, menus) are outside it.
 * gfx_overlay_show() returns 1 when the layer will be pasted, 0 when the caller must gfx_layer_blit() it
 * (no driver, no back buffer, dimmed screen, or the upload failed). changed: the picture is new. inset: the
 * layer is opaque everywhere at least this far from its edges. */
typedef struct
{
    int (*upload)(const void *layer, int lw, int lh);
    volatile unsigned int *(*below)(int *pitch);
    int (*draw)(int x, int y, int x0, int y0, int x1, int y1);
} GfxOverlayOps;
void gfx_overlay_driver(const GfxOverlayOps *ops); /* 0: none */
void gfx_overlay_area(int x, int y, int w, int h);   /* w or h <= 0: nowhere */
int gfx_overlay_show(const void *layer, int lw, int lh, int x, int y, int changed, int inset);
void gfx_overlay_hide(void);

/* ---- compositor: a GPU puts the whole scene together ----------
 * Every part of the scene lives in its own picture in the GPU's memory (a "surface"): the wallpaper, each window
 * (a packed layer with its shadow), the taskbar. For the parts of the screen that changed, the driver puts the
 * listed items together, back to front, and writes the result onto the screen in one go (no half-finished
 * picture is ever shown). The back buffer is not used for them; the CPU only draws what changed inside a surface.
 *   surface_new(w, h)             a new surface; its id (> 0), 0 = none
 *   surface_free(id)
 *   upload(id, px, pitch, x, y, w, h, opaque)  w x h pixels (pitch pixels per row) to (x, y) of the surface;
 *                                 opaque: they cover what is below (the top byte is ignored); else a packed layer
 *   compose(items, n, rects, nr)  put items[0..n-1] together inside each of the nr rectangles (x0, y0, x1, y1
 *                                 each, screen pixels) and write that onto the screen; 0 = done and finished
 * gfx_comp_present() wraps compose with the pointer (taken off the screen before, drawn again after). */
typedef struct
{
    int surface;  /* id */
    int x, y;     /* where its pixel (0, 0) is on the screen */
    int w, h;
    int layer;    /* 1 = packed layer (blended), 0 = opaque picture */
} GfxCompItem;
/* What the desktop measured over a number of composed frames (for GfxCompOps.report). Times in microseconds. */
typedef struct
{
    unsigned frames;
    unsigned long long draw_us, draw_us_max;       /* the CPU bringing surfaces up to date (drawing, packing, uploading) */
    unsigned long long present_us, present_us_max; /* the compose: building it, the GPU doing it, waiting for it */
    unsigned windows_full, windows_part, tops;     /* surfaces drawn again: whole windows, parts of windows, menus / taskbar */
    /* where the requests to draw window content came from (names from the window manager), and the busiest windows */
    int nwhy;
    unsigned why[12];
    const char *why_name[12];
    int nbusy;
    struct
    {
        char title[24];
        unsigned redraws;
        unsigned long long pixels;
    } busy[3];
} GfxCompStats;
typedef struct
{
    int (*surface_new)(int w, int h);
    void (*surface_free)(int id);
    int (*upload)(int id, const void *px, int pitch, int x, int y, int w, int h, int opaque);
    int (*compose)(const GfxCompItem *items, int n, const int *rects, int nr);
    void (*report)(const GfxCompStats *s); /* may be 0: every few hundred frames, what the desktop measured */
} GfxCompOps;
void gfx_comp_driver(const GfxCompOps *ops); /* 0: none */
const GfxCompOps *gfx_comp(void);            /* the driver, 0 when there is none (or it failed) */
int gfx_comp_present(const GfxCompItem *items, int n, const int *rects, int nr); /* 0 = done; on a failure the driver is dropped */

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

/* Copy n pixels of 0x00RRGGBB (whatever the screen's own order is) to row y from x on, clipped. The picture of a window
 * that a program draws itself (gui.h) is shown with this. */
void gfx_put_pixels(int x, int y, const unsigned int *src, int n);

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
