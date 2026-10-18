#include "font.h"
#include <assert.h>
#include <stdio.h>

GfxColor gfx_rgb(unsigned char r, unsigned char g, unsigned char b)
{
    return 0xFF000000u | ((unsigned int)r << 16) | ((unsigned int)g << 8) | b;
}

static int blend_calls;
static unsigned char min_alpha = 255;
static int min_x = 2147483647;

void gfx_blend_pixel(int x, int y, GfxColor color, unsigned char alpha)
{
    (void)y;
    (void)color;
    ++blend_calls;
    if (alpha < min_alpha) min_alpha = alpha;
    if (x < min_x) min_x = x;
}

int main(void)
{
    assert(font_line_height(NULL) == 0);
    assert(font_line_height(&font_ui_14) > 0);
    assert(gfx_text_w(NULL, "Hello") == 0);
    assert(gfx_text_w(&font_ui_14, NULL) == 0);
    assert(gfx_text_w(&font_ui_14, "Hello") > 0);
    assert(gfx_text_w(&font_ui_14, "Hello\nHi") == gfx_text_w(&font_ui_14, "Hello"));
    assert(gfx_text_h(&font_ui_14, "") == 0);
    assert(gfx_text_h(&font_ui_14, "A") == font_line_height(&font_ui_14));
    assert(gfx_text_h(&font_ui_14, "A\nB") > gfx_text_h(&font_ui_14, "A"));
    assert(gfx_text_fit(&font_ui_14, "Hello", 0) == 0);
    assert(gfx_text_fit(&font_ui_14, "Hello", -1) == 0);
    assert(gfx_text_fit(&font_ui_14, "Hello", gfx_text_w(&font_ui_14, "Hello")) == 5);

    gfx_text_alpha(&font_ui_14, 10, 5, "A", gfx_rgb(255, 255, 255), 255);
    assert(blend_calls > 0);
    assert(min_x >= 0 && min_x < 100); /* glyph pixels stay near the requested origin */

    int calls_before = blend_calls;
    gfx_text_alpha(&font_ui_14, 10, 5, "A", gfx_rgb(255, 255, 255), 0);
    assert(blend_calls == calls_before); /* zero alpha is a no-op */

    puts("Font regression tests passed.");
    return 0;
}
