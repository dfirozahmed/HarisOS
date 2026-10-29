#include "gfx.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static void expect_rgb(GfxColor c, unsigned char er, unsigned char eg, unsigned char eb)
{
    unsigned char r = 0, g = 0, b = 0;
    gfx_unpack(c, &r, &g, &b);
    assert(r == er && g == eg && b == eb);
}

/* a pretend GPU for the overlay test: writes layer + the picture below it (from its own buffer) onto the screen */
static const unsigned int *OV_LAYER;
static unsigned int *OV_FB;
static unsigned int OV_BELOW[20 * 16];
static int OV_LW, OV_PITCH, OV_UPLOADS, OV_DRAWS, OV_FAIL, OV_BOX[4];
static unsigned int OV_SENTINEL;
static int OV_UNTOUCHED; /* at the paste, the screen inside the box still held OV_SENTINEL */
static unsigned int ov_px(unsigned int p, unsigned int below)
{
    unsigned int t = p >> 24, v = 0;
    for (int s = 0; s < 24; s += 8) {
        unsigned int c = ((p >> s) & 0xFF) + (((below >> s) & 0xFF) * t + 127) / 255;
        v |= (c > 255 ? 255 : c) << s;
    }
    return v;
}
static int ov_upload(const void *layer, int lw, int lh)
{
    assert(layer == OV_LAYER && lw == OV_LW && lw * lh <= 20 * 16);
    OV_UPLOADS++;
    return 0;
}
static volatile unsigned int *ov_below(int *pitch)
{
    *pitch = OV_LW;
    return OV_FAIL == 2 ? 0 : OV_BELOW;
}
static int ov_draw(int x, int y, int x0, int y0, int x1, int y1)
{
    if (OV_FAIL)
        return -1;
    OV_DRAWS++;
    OV_BOX[0] = x0; OV_BOX[1] = y0; OV_BOX[2] = x1; OV_BOX[3] = y1;
    OV_UNTOUCHED = 1;
    for (int yy = y0; yy < y1; ++yy)
        for (int xx = x0; xx < x1; ++xx) {
            int i = (yy - y) * OV_LW + (xx - x);
            if (OV_FB[yy * OV_PITCH + xx] != OV_SENTINEL) OV_UNTOUCHED = 0;
            OV_FB[yy * OV_PITCH + xx] = ov_px(OV_LAYER[i], OV_BELOW[i]);
        }
    return 0;
}

/* a pretend compositor driver: compose fails when CMP_FAIL is set */
static int CMP_CALLS, CMP_FAIL;
static int c_new(int w, int h){ (void)w; (void)h; return 1; }
static void c_free(int id){ (void)id; }
static int c_up(int id, const void *p, int pitch, int x, int y, int w, int h, int o){ (void)id; (void)p; (void)pitch; (void)x; (void)y; (void)w; (void)h; (void)o; return 0; }
static int c_compose(const GfxCompItem *it, int n, const int *r, int nr){ (void)it; (void)n; (void)r; (void)nr; CMP_CALLS++; return CMP_FAIL ? -1 : 0; }

/* the pointer drawn by a pretend compositor: the boxes it is told about */
static int PTR_CALLS, PTR_BOX[8][4];
static void ptr_changed(int x, int y, int w, int h)
{
    if (PTR_CALLS < 8) { PTR_BOX[PTR_CALLS][0] = x; PTR_BOX[PTR_CALLS][1] = y; PTR_BOX[PTR_CALLS][2] = w; PTR_BOX[PTR_CALLS][3] = h; }
    PTR_CALLS++;
}

int main(void)
{
    enum { WIDTH = 4, HEIGHT = 3, PITCH = 6 };
    unsigned int screen[PITCH * HEIGHT];
    for (int i = 0; i < PITCH * HEIGHT; ++i) screen[i] = 0xDEADBEEFu;

    gfx_init(screen, WIDTH, HEIGHT, PITCH, 1);
    assert(gfx_ready());
    assert(gfx_width() == WIDTH && gfx_height() == HEIGHT);

    GfxColor red = gfx_rgb(255, 0, 0);
    GfxColor blue = gfx_rgb(0, 0, 255);
    expect_rgb(red, 255, 0, 0);
    gfx_clear(red);
    for (int y = 0; y < HEIGHT; ++y) {
        for (int x = 0; x < WIDTH; ++x) assert(screen[y * PITCH + x] == red);
        assert(screen[y * PITCH + WIDTH] == 0xDEADBEEFu);
        assert(screen[y * PITCH + WIDTH + 1] == 0xDEADBEEFu);
    }

    gfx_set_clip(1, 1, 2, 1);
    gfx_fill_rect(0, 0, WIDTH, HEIGHT, blue);
    assert(screen[0] == red);
    assert(screen[PITCH + 1] == blue && screen[PITCH + 2] == blue);
    assert(screen[PITCH] == red && screen[PITCH + 3] == red);
    gfx_reset_clip();

    gfx_pixel(-1, 0, blue); /* off-screen writes must be ignored */
    gfx_pixel(WIDTH, 0, blue);
    assert(screen[0] == red && screen[WIDTH] == 0xDEADBEEFu);

    gfx_fill_rect(0, 0, 0, HEIGHT, blue); /* empty geometry */
    assert(screen[0] == red);

    /* Alpha endpoints: 0 preserves the destination, 255 replaces it. */
    gfx_blend_pixel(0, 0, blue, 0);
    assert(screen[0] == red);
    gfx_blend_pixel(0, 0, blue, 255);
    assert(screen[0] == blue);

    /* Backbuffer writes stay hidden until present, then copy to pitched FB. */
    unsigned int back[WIDTH * HEIGHT];
    assert(gfx_enable_backbuffer(back) == 0);
    assert(gfx_double_buffered());
    gfx_fill_rect(1, 1, 1, 1, red);
    assert(screen[PITCH + 1] == blue);
    gfx_present();
    assert(screen[PITCH + 1] == red);
    assert(gfx_double_buffered());

    /* Brightness scales only what goes to the screen; the back buffer keeps the true picture. */
    GfxColor white = gfx_rgb(255, 255, 255);
    gfx_fill_rect(0, 0, WIDTH, HEIGHT, white);
    gfx_present();
    assert(gfx_brightness() == GFX_BRIGHTNESS_MAX);
    assert(gfx_set_brightness(50) == 0 && gfx_brightness() == 50);
    expect_rgb(screen[0], 127, 127, 127);
    assert(back[0] == white);
    assert(gfx_set_brightness(0) == 0 && gfx_brightness() == GFX_BRIGHTNESS_MIN); /* never fully black */
    expect_rgb(screen[0], 24, 24, 24);
    assert(gfx_set_brightness(500) == 0 && gfx_brightness() == GFX_BRIGHTNESS_MAX);
    assert(screen[0] == white);
    gfx_disable_backbuffer();
    assert(gfx_set_brightness(50) == -1); /* needs the back buffer */
    assert(!gfx_double_buffered());

    {   /* layers: a shape drawn into a layer and pasted must look like the shape drawn directly */
        enum { SW = 120, SH = 90, LW = 60, LH = 50, LX = 30, LY = 20 };
        static unsigned int scr[SW * SH], want[SW * SH], black[LW * LH], white[LW * LH];
        gfx_init(scr, SW, SH, SW, 1);
        for (int pass = 0; pass < 2; ++pass) {
            if (pass == 0) {
                gfx_layer_begin(black, LW, LH, 0x000000u);
            } else {
                gfx_layer_begin(white, LW, LH, 0xFFFFFFu);
            }
            gfx_fill_rect_alpha(4, 4, 40, 30, 200, 100, 50, 128);   /* translucent, like a shadow */
            gfx_fill_rect(14, 14, 20, 20, gfx_rgb(10, 200, 30));     /* opaque */
            gfx_layer_end();
        }
        assert(gfx_width() == SW && gfx_height() == SH);            /* the real screen is back */
        gfx_layer_pack(black, white, LW, LH);
        assert((black[0] >> 24) == 255);                             /* nothing drawn: fully see-through */
        assert((black[20 * LW + 20] >> 24) == 0);                    /* opaque pixel */
        for (int i = 0; i < SW * SH; ++i) scr[i] = 0x00405060u;
        for (int i = 0; i < SW * SH; ++i) want[i] = scr[i];
        gfx_layer_blit(black, LW, LH, LX, LY);
        gfx_init(want, SW, SH, SW, 1);
        gfx_fill_rect_alpha(LX + 4, LY + 4, 40, 30, 200, 100, 50, 128);
        gfx_fill_rect(LX + 14, LY + 14, 20, 20, gfx_rgb(10, 200, 30));
        for (int i = 0; i < SW * SH; ++i) {
            int d0 = (int)(scr[i] & 0xFF) - (int)(want[i] & 0xFF);
            int d1 = (int)((scr[i] >> 8) & 0xFF) - (int)((want[i] >> 8) & 0xFF);
            int d2 = (int)((scr[i] >> 16) & 0xFF) - (int)((want[i] >> 16) & 0xFF);
            assert(d0 >= -2 && d0 <= 2 && d1 >= -2 && d1 <= 2 && d2 >= -2 && d2 <= 2);   /* rounding only */
        }
    }

    {   /* scrolling a rectangle of the back buffer: rows move, everything outside stays */
        enum { SW = 20, SH = 12 };
        static unsigned int fb[SW * SH], back[SW * SH];
        gfx_init(fb, SW, SH, SW, 1);
        assert(gfx_scroll_rect(0, 0, SW, SH, 1) == -1);                 /* no back buffer yet */
        assert(gfx_enable_backbuffer(back) == 0);
        for (int y = 0; y < SH; ++y) gfx_fill_rect(0, y, SW, 1, gfx_rgb((unsigned char)(y * 10), 0, 0));
        assert(gfx_scroll_rect(4, 2, 10, 8, 3) == 0);                   /* rows 2..9 move down by 3 */
        for (int x = 0; x < SW; ++x) {
            for (int y = 0; y < SH; ++y) {
                int inside = x >= 4 && x < 14 && y >= 2 && y < 10;
                int from = inside && y >= 5 ? y - 3 : y;                /* rows 2..4 keep their old pixels */
                assert(back[y * SW + x] == gfx_rgb((unsigned char)(from * 10), 0, 0));
            }
        }
        assert(gfx_scroll_rect(0, 0, SW, SH, SH) == -1);                /* everything would scroll out */
        assert(gfx_scroll_rect(0, 0, SW, SH, 0) == -1);
        for (int y = 0; y < SH; ++y) gfx_fill_rect(0, y, SW, 1, gfx_rgb((unsigned char)(y * 10), 0, 0));
        assert(gfx_scroll_rect(0, 0, SW, SH, -2) == 0);                 /* up by 2 */
        gfx_backbuffer_detach(1);                                       /* a compositor keeps the pictures elsewhere */
        assert(gfx_scroll_rect(0, 0, SW, SH, 1) == -1 && back[5] == gfx_rgb(20, 0, 0)); /* refused, nothing moved */
        gfx_backbuffer_detach(0);
        for (int y = 0; y < SH - 2; ++y) assert(back[y * SW + 5] == gfx_rgb((unsigned char)((y + 2) * 10), 0, 0));
    }

    /* gfx_put_pixels: 0x00RRGGBB in, the screen's own order out, clipped on both sides */
    {
        unsigned int scr[8 * 2];
        for (int i = 0; i < 16; ++i) scr[i] = 0xDEADBEEFu;
        gfx_init(scr, 8, 2, 8, 1);
        gfx_reset_clip();
        const unsigned int px[4] = {0x00112233u, 0xFF445566u, 0x00778899u, 0x00AABBCCu};
        gfx_put_pixels(2, 0, px, 4);
        assert(scr[1] == 0xDEADBEEFu && scr[6] == 0xDEADBEEFu);
        assert(scr[2] == gfx_rgb(0x11, 0x22, 0x33) && scr[3] == gfx_rgb(0x44, 0x55, 0x66));   /* the top byte is dropped */
        assert(scr[4] == gfx_rgb(0x77, 0x88, 0x99) && scr[5] == gfx_rgb(0xAA, 0xBB, 0xCC));
        gfx_put_pixels(-2, 1, px, 4);                    /* the part left of the screen is cut off */
        assert(scr[8] == gfx_rgb(0x77, 0x88, 0x99) && scr[9] == gfx_rgb(0xAA, 0xBB, 0xCC) && scr[10] == 0xDEADBEEFu);
        gfx_put_pixels(6, 1, px, 4);                     /* ... and the part right of it */
        assert(scr[14] == gfx_rgb(0x11, 0x22, 0x33) && scr[15] == gfx_rgb(0x44, 0x55, 0x66));
        gfx_put_pixels(0, 2, px, 4);                     /* below the screen: nothing */
        gfx_put_pixels(0, 0, px, 0);
        gfx_set_clip(3, 0, 1, 2);
        gfx_put_pixels(0, 0, px, 4);                     /* the clip rectangle applies */
        assert(scr[3] == gfx_rgb(0xAA, 0xBB, 0xCC) && scr[0] == 0xDEADBEEFu && scr[2] == gfx_rgb(0x11, 0x22, 0x33));
        gfx_reset_clip();
    }

    /* overlay: a driver pastes a layer at gfx_present(), after the copy and without copying the layer's opaque inside */
    {
        enum { SW = 60, SH = 40, LW = 20, LH = 16, LX = 10, LY = 8, IN = 4 };
        static unsigned int fb[SW * SH], back[SW * SH], lay[LW * LH];
        static const GfxOverlayOps ops = {ov_upload, ov_below, ov_draw};
        for (int y = 0; y < LH; ++y)
            for (int x = 0; x < LW; ++x) {
                int edge = x < IN || y < IN || x >= LW - IN || y >= LH - IN;
                lay[y * LW + x] = edge ? (0x80000000u | 0x00101010u) : 0x00AA3300u; /* half see-through edge, opaque inside */
            }
        OV_LAYER = lay;
        OV_LW = LW;
        OV_FB = fb;
        OV_PITCH = SW;
        gfx_init(fb, SW, SH, SW, 1);
        assert(gfx_enable_backbuffer(back) == 0);
        gfx_fill_rect(0, 0, SW, SH, gfx_rgb(0x20, 0x40, 0x60));
        gfx_present();
        unsigned int below = fb[0];
        assert(gfx_overlay_show(lay, LW, LH, LX, LY, 1, IN) == 0);   /* no driver: the caller blits */

        gfx_overlay_driver(&ops);
        OV_SENTINEL = 0x00777777u;
        for (int i = 0; i < SW * SH; ++i) fb[i] = OV_SENTINEL;           /* to see what the copy wrote */
        gfx_fill_rect(0, 0, SW, SH, gfx_rgb(0x20, 0x40, 0x60));
        assert(gfx_overlay_show(lay, LW, LH, LX, LY, 1, IN) == 1 && OV_UPLOADS == 1);
        gfx_present();
        assert(OV_DRAWS == 1 && OV_BOX[0] == LX && OV_BOX[1] == LY && OV_BOX[2] == LX + LW && OV_BOX[3] == LY + LH);
        assert(OV_UNTOUCHED);                                           /* nothing under the layer was copied to the screen */
        assert(OV_BELOW[0] == below && OV_BELOW[8 * LW + 10] == 0);    /* the picture below went to the driver, not the inside */
        OV_SENTINEL = 0xFFFFFFFFu;
        assert(fb[0] == below && fb[(LY + LH) * SW + LX] == below);     /* around the layer: the back buffer */
        assert(fb[(LY + 8) * SW + LX + 10] == 0x00AA3300u);             /* the inside: the layer */
        assert(fb[LY * SW + LX] == ov_px(lay[0], below));               /* the edge: blended over the back buffer */
        assert(back[(LY + 8) * SW + LX + 10] == below);                 /* the layer never went into the back buffer */

        assert(gfx_overlay_show(lay, LW, LH, LX + 5, LY, 0, IN) == 1 && OV_UPLOADS == 1);  /* moved: no new upload */
        gfx_overlay_area(0, 0, SW, LY + 4);                             /* a "taskbar" below row LY + 4 */
        gfx_present();
        assert(OV_DRAWS == 2 && OV_BOX[0] == LX + 5 && OV_BOX[3] == LY + 4);
        assert(gfx_set_brightness(50) == 0);
        assert(gfx_overlay_show(lay, LW, LH, LX, LY, 0, IN) == 0);      /* dimmed: the GPU picture would not be */
        assert(gfx_set_brightness(GFX_BRIGHTNESS_MAX) == 0);
        gfx_overlay_area(0, 0, 0, 0);
        assert(gfx_overlay_show(lay, LW, LH, LX, LY, 0, IN) == 0);      /* nowhere to show it */
        gfx_overlay_area(0, 0, SW, SH);
        assert(gfx_overlay_show(lay, LW, LH, LX, LY, 0, IN) == 1 && OV_UPLOADS == 2);   /* shown again: uploaded again */
        gfx_overlay_hide();
        gfx_fill_rect(0, 0, SW, SH, gfx_rgb(0x20, 0x40, 0x60));
        gfx_present();
        assert(OV_DRAWS == 2 && fb[(LY + 8) * SW + LX + 10] == below);  /* hidden: not pasted */

        OV_FAIL = 1;                                                    /* the driver fails: the CPU pastes it, and from then on */
        assert(gfx_overlay_show(lay, LW, LH, LX, LY, 1, IN) == 1);
        gfx_present();
        assert(fb[(LY + 8) * SW + LX + 10] == 0x00AA3300u && back[(LY + 8) * SW + LX + 10] == 0x00AA3300u);
        assert(fb[LY * SW + LX] != below);                              /* the edge is blended */
        assert(gfx_overlay_show(lay, LW, LH, LX, LY, 1, IN) == 0);      /* no driver any more */
        gfx_overlay_driver(&ops);                                      /* again, and now its buffer cannot be had */
        OV_FAIL = 2;
        assert(gfx_overlay_show(lay, LW, LH, LX, LY, 1, IN) == 1);
        gfx_present();
        assert(fb[(LY + 8) * SW + LX + 10] == 0x00AA3300u && gfx_overlay_show(lay, LW, LH, LX, LY, 1, IN) == 0);
        gfx_disable_backbuffer();
    }

    {   /* compositor: present calls the driver; a failure drops it */
        static const GfxCompOps cops = {c_new, c_free, c_up, c_compose, 0, 0};
        static unsigned int scr[16 * 8];
        GfxCompItem it = {1, 0, 0, 16, 8, 0};
        const int r[4] = {0, 0, 16, 8};
        gfx_init(scr, 16, 8, 16, 1);
        assert(gfx_comp() == 0 && gfx_comp_present(&it, 1, r, 1) == -1);
        gfx_comp_driver(&cops);
        assert(gfx_comp() == &cops && gfx_comp_present(&it, 1, r, 1) == 0 && CMP_CALLS == 1);
        CMP_FAIL = 1;
        assert(gfx_comp_present(&it, 1, r, 1) == -1 && CMP_CALLS == 2 && gfx_comp() == 0);
    }

    {   /* a layer placed on the screen: drawing in screen coordinates lands in the buffer, cut to it and to the screen's left / top */
        static unsigned int scr[40 * 30], buf[10 * 8];
        gfx_init(scr, 40, 30, 40, 1);
        assert(gfx_layer_begin_at(buf, 12, 6, 10, 8, 0x00FFFFFFu) == 0);
        assert(gfx_width() == 40 && gfx_height() == 30);              /* the screen is still the screen */
        gfx_fill_rect(0, 0, 40, 30, gfx_rgb(1, 2, 3));               /* everything: only the buffer gets it */
        gfx_layer_end();
        for (int i = 0; i < 10 * 8; ++i) assert(buf[i] == gfx_rgb(1, 2, 3));
        for (int i = 0; i < 40 * 30; ++i) assert(scr[i] == 0);
        assert(gfx_layer_begin_at(buf, 12, 6, 10, 8, 0) == 0);
        gfx_fill_rect(14, 7, 2, 1, gfx_rgb(9, 9, 9));                /* screen (14, 7) is buffer (2, 1) */
        gfx_layer_end();
        assert(buf[1 * 10 + 2] == gfx_rgb(9, 9, 9) && buf[1 * 10 + 4] == 0 && buf[0] == 0);
        assert(gfx_layer_begin_at(buf, -4, -2, 10, 8, 0) == 0);        /* reaching past the left and top edges */
        gfx_fill_rect(-4, -2, 10, 8, gfx_rgb(5, 5, 5));
        gfx_layer_end();
        assert(buf[0] == 0 && buf[2 * 10 + 4] == gfx_rgb(5, 5, 5) && buf[1 * 10 + 9] == 0);
        assert(gfx_layer_begin_at(buf, 35, 26, 10, 8, 0) == 0);        /* and past the right and bottom ones: kept */
        gfx_fill_rect(35, 26, 10, 8, gfx_rgb(6, 6, 6));
        gfx_layer_end();
        assert(buf[0] == gfx_rgb(6, 6, 6) && buf[7 * 10 + 9] == gfx_rgb(6, 6, 6));
        assert(gfx_width() == 40 && gfx_height() == 30);
    }

    {   /* a compositor draws the pointer: the CPU leaves the screen alone and reports the boxes */
        enum { SW = 120, SH = 90 };
        static unsigned int fb[SW * SH], back[SW * SH], pic[64 * 64];
        gfx_init(fb, SW, SH, SW, 1);
        assert(gfx_enable_backbuffer(back) == 0);
        gfx_cursor_move(30, 20);
        gfx_cursor_show(1);
        int painted = 0;
        for (int i = 0; i < SW * SH; ++i) painted |= fb[i] != 0;
        assert(painted);                                       /* the CPU drew it */
        gfx_cursor_external(ptr_changed);
        for (int i = 0; i < SW * SH; ++i) assert(fb[i] == 0); /* taken off the screen ... */
        assert(PTR_CALLS == 1);                                /* ... and its box given to the compositor */
        int x, y, w, h; unsigned g1, g2;
        assert(gfx_cursor_layer(pic, 64 * 64, &x, &y, &w, &h, &g1) == 1);
        assert(x == PTR_BOX[0][0] && y == PTR_BOX[0][1] && w == PTR_BOX[0][2] && h == PTR_BOX[0][3]);
        int see_through = 0, solid = 0;
        for (int i = 0; i < w * h; ++i) { see_through |= (pic[i] >> 24) == 255 && (pic[i] & 0xFFFFFFu) == 0; solid |= (pic[i] >> 24) == 0; }
        assert(see_through && solid);                         /* a packed layer: empty corners, an opaque arrow */
        gfx_cursor_move(50, 40);
        assert(PTR_CALLS == 3 && PTR_BOX[1][0] == x && PTR_BOX[2][0] == x + 20 && PTR_BOX[2][1] == y + 20);
        for (int i = 0; i < SW * SH; ++i) assert(fb[i] == 0); /* still nothing written */
        gfx_cursor_move(50, 40);
        assert(PTR_CALLS == 3);                                /* not moved: nothing to draw */
        gfx_cursor_set_shape(GFX_CURSOR_SIZE_H);
        assert(PTR_CALLS == 5 && gfx_cursor_layer(pic, 64 * 64, &x, &y, &w, &h, &g2) == 1 && g2 != g1 && w == 21);
        gfx_cursor_set_shape(0);
        gfx_cursor_show(0);
        assert(PTR_CALLS == 8 && gfx_cursor_layer(pic, 64 * 64, &x, &y, &w, &h, &g2) == 0); /* hidden: no picture */
        gfx_cursor_show(1);
        gfx_cursor_external(0);                                /* the CPU again: drawn on the screen */
        painted = 0;
        for (int i = 0; i < SW * SH; ++i) painted |= fb[i] != 0;
        assert(painted);
        gfx_disable_backbuffer();
    }

    puts("Graphics regression tests passed.");
    return 0;
}

// MSYS2 UCRT64
// chmod +x tests/run_gfx_tests.sh
// CC=clang ./tests/run_gfx_tests.sh