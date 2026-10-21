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

    puts("Graphics regression tests passed.");
    return 0;
}

// MSYS2 UCRT64
// chmod +x tests/run_gfx_tests.sh
// CC=clang ./tests/run_gfx_tests.sh