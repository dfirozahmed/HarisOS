/* HarisOS taskbar. All drawing uses the gfx layer (include/gfx.h). */
#include "taskbar.h"
#include "gfx.h"

#define SLOT 40 /* size of one clickable slot */
#define GAP 4

/* ---- Haris logo */
#define HLOGO 26
static void draw_h_logo(int x, int y)
{
    GfxColor white = gfx_rgb(255, 255, 255);
    gfx_fill_round_rect(x, y, HLOGO, HLOGO, 7, gfx_rgb(70, 150, 255));
    gfx_fill_rect(x + 7, y + 6, 4, 14, white);  /* left stem */
    gfx_fill_rect(x + 15, y + 6, 4, 14, white); /* right stem */
    gfx_fill_rect(x + 7, y + 11, 12, 4, white); /* crossbar */
}

/* ---- the taskbar ------------------------------------------- */
void taskbar_draw(void)
{
    if (!gfx_ready())
        return;

    int W = (int)gfx_width(), H = (int)gfx_height();
    int by = H - TASKBAR_HEIGHT;
    int cy = by + TASKBAR_HEIGHT / 2 - 2; /* leave room for the indicators below */
    GfxColor text = gfx_rgb(236, 242, 252);

    /* translucent bar with a soft highlight along the top edge */
    gfx_fill_rect_alpha(0, by, W, TASKBAR_HEIGHT, 14, 22, 38, 228);
    gfx_fill_rect_alpha(0, by, W, 1, 255, 255, 255, 45);

    /* left side: start button (H logo + "HARIS") */
    int x = 10;

    int name_w = gfx_text_width("HARIS", 2);
    int start_w = 8 + HLOGO + 8 + name_w + 12;
    gfx_fill_round_rect(x, cy - SLOT / 2 + 2, start_w, SLOT - 4, 8, gfx_rgb(36, 48, 72));
    draw_h_logo(x + 8, cy - HLOGO / 2);
    gfx_draw_text(x + 8 + HLOGO + 8, cy - 7, "HARIS", text, 2);
}
