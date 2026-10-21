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

    puts("Graphics regression tests passed.");
    return 0;
}

// MSYS2 UCRT64
// chmod +x tests/run_gfx_tests.sh
// CC=clang ./tests/run_gfx_tests.sh