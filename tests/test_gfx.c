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

    puts("Graphics regression tests passed.");
    return 0;
}

// MSYS2 UCRT64
// chmod +x tests/run_gfx_tests.sh
// CC=clang ./tests/run_gfx_tests.sh