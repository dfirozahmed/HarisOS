/* HarisOS graphics backend initialization and framebuffer properties.
 */
#include "gfx_internal.h"

/* 1: 0x00RRGGBB, 0: 0x00BBGGRR; consumed by color packing in gfx.c. */
int RED_FIRST;

void gfx_init(void *framebuffer, unsigned int width, unsigned int height,
              unsigned int pitch, unsigned int red_mask_mode)
{
    gfx_internal_buffer_init(framebuffer, (int)width, (int)height, (int)pitch);
    RED_FIRST = red_mask_mode ? 1 : 0;
    gfx_internal_cursor_reset_state();
    gfx_reset_clip();
}

int gfx_ready(void)
{
    return FB != 0 && W > 0 && H > 0;
}

unsigned int gfx_width(void)
{
    return (unsigned int)W;
}

unsigned int gfx_height(void)
{
    return (unsigned int)H;
}

volatile unsigned int *gfx_screen_base(void)
{
    return FB;
}

unsigned int gfx_screen_pitch_bytes(void)
{
    return (unsigned int)PITCH * 4u;
}

void gfx_repaint_all(void)
{
    if (!BACK)
        return;
    gfx_internal_mark_dirty(0, 0, W, H);
    gfx_present();
}
