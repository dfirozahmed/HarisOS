/* ============================================================
 * HarisOS graphics layer (gfx)
 *
 * A small, reusable 2D drawing library on top of a linear 32-bit
 * framebuffer. It knows nothing about UEFI or the desktop: any code
 * (desktop, window manager, boot screen, panic screen) can call it.
 *
 *   gfx_init(...)            once, with the framebuffer from the bootloader
 *   gfx_fill_rect(...) ...   drawing primitives
 *   gfx_draw_text(...)       built-in 5x7 bitmap font
 *
 * All coordinates are in pixels, (0,0) is the top-left corner.
 * Every call is clipped to the screen and to the optional clip rectangle,
 * so drawing outside the visible area is always safe.
 * ============================================================ */
#ifndef HARIS_GFX_H
#define HARIS_GFX_H

/* Packed pixel in the framebuffer's native format. Create it with gfx_rgb(). */
typedef unsigned int GfxColor;

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

/* ---- colors ------------------------------------------------- */
GfxColor gfx_rgb(unsigned char r, unsigned char g, unsigned char b);

/* ---- clipping ----------------------------------------------- */
/* Restrict all drawing to a rectangle (e.g. the inside of a window). */
void gfx_set_clip(int x, int y, int w, int h);
void gfx_reset_clip(void);

/* ---- primitives --------------------------------------------- */
void gfx_clear(GfxColor c);
void gfx_pixel(int x, int y, GfxColor c);
void gfx_hline(int x, int y, int len, GfxColor c);
void gfx_vline(int x, int y, int len, GfxColor c);
void gfx_line(int x0, int y0, int x1, int y1, GfxColor c);
void gfx_rect(int x, int y, int w, int h, GfxColor c); /* 1px outline */
void gfx_fill_rect(int x, int y, int w, int h, GfxColor c);
void gfx_fill_round_rect(int x, int y, int w, int h, int radius, GfxColor c);
void gfx_fill_circle(int cx, int cy, int radius, GfxColor c);

/* Semi-transparent fill: alpha 0 = invisible, 255 = solid. */
void gfx_fill_rect_alpha(int x, int y, int w, int h,
                         unsigned char r, unsigned char g, unsigned char b,
                         unsigned char alpha);

/* Vertical gradient from color 1 (top) to color 2 (bottom). */
void gfx_gradient_v(int x, int y, int w, int h,
                    unsigned char r1, unsigned char g1, unsigned char b1,
                    unsigned char r2, unsigned char g2, unsigned char b2);

/* ---- text (5x7 font; A-Z, 0-9 and . : - / _ ! +; lowercase is drawn uppercase) */
#define GFX_FONT_W 5
#define GFX_FONT_H 7
void gfx_draw_char(int x, int y, char ch, GfxColor c, int scale);
void gfx_draw_text(int x, int y, const char *s, GfxColor c, int scale); /* '\n' starts a new line */
int gfx_text_width(const char *s, int scale);                           /* width of the longest line, in pixels */
int gfx_text_height(const char *s, int scale);                          /* height of all lines, in pixels */

#endif
