/* Host test of apps/imageviewer/imgview.c: fit, zoom steps, centring / clamping, zooming about a point, and the rows of
 * a zoomed picture. */
#include <stdio.h>
#include "imgview.h"

static int ok = 1;
static void check(int cond, const char *name)
{
    printf("%s: %s\n", name, cond ? "PASS" : "FAIL");
    if (!cond)
        ok = 0;
}

int main(void)
{
    check(imgview_fit_pct(200, 100, 100, 100) == 50, "fit: wide picture limited by width");
    check(imgview_fit_pct(100, 400, 100, 100) == 25, "fit: tall picture limited by height");
    check(imgview_fit_pct(10, 10, 100, 100) == 100, "fit: a small picture is not enlarged");
    check(imgview_fit_pct(100000, 10, 10, 10) == IMGV_MIN_PCT, "fit: never below the smallest zoom");
    check(imgview_fit_pct(0, 10, 10, 10) == 100, "fit: bad size");

    check(imgview_step_pct(100, 1) == 125 && imgview_step_pct(100, -1) == 75, "steps: around 100%");
    check(imgview_step_pct(3200, 1) == IMGV_MAX_PCT && imgview_step_pct(5, -1) == IMGV_MIN_PCT, "steps: ends");
    check(imgview_step_pct(110, 1) == 125 && imgview_step_pct(110, -1) == 100, "steps: between two steps");

    check(imgview_scaled(100, 50) == 50 && imgview_scaled(1, 5) == 1 && imgview_scaled(7, 200) == 14, "scaled");

    int ox = 999, oy = -999;
    imgview_clamp(&ox, &oy, 50, 50, 100, 80);
    check(ox == 25 && oy == 15, "clamp: a small picture is centred");
    ox = 40; oy = -500;
    imgview_clamp(&ox, &oy, 300, 200, 100, 80);
    check(ox == 0 && oy == -120, "clamp: a big picture keeps its edges inside");
    ox = -150; oy = -30;
    imgview_clamp(&ox, &oy, 300, 200, 100, 80);
    check(ox == -150 && oy == -30, "clamp: a position that is fine stays");

    ox = 10; oy = 20;
    imgview_zoom_at(&ox, &oy, 100, 200, 60, 70); /* the point at (50, 50) of the picture stays under (60, 70) */
    check(ox == -40 && oy == -30, "zoom_at: the point under the pointer stays");

    unsigned img[4] = {0x000000, 0xFF0000, 0x00FF00, 0x0000FF}; /* 2 x 2 */
    unsigned line[8];
    imgview_row(img, 2, 2, 200, 1, 0, 6, 0, 0x777777, line); /* shown 4 x 4, one column in */
    check(line[0] == 0x777777 && line[1] == 0 && line[2] == 0 && line[3] == 0xFF0000 && line[4] == 0xFF0000 && line[5] == 0x777777,
          "row: zoomed in, nearest, background beside it");
    imgview_row(img, 2, 2, 200, 1, 0, 6, 3, 0x777777, line);
    check(line[1] == 0x00FF00 && line[4] == 0x0000FF, "row: second picture row");
    imgview_row(img, 2, 2, 200, 1, 0, 6, 4, 0x777777, line);
    check(line[1] == 0x777777 && line[3] == 0x777777, "row: below the picture is background");
    imgview_row(img, 2, 2, 200, -1, -1, 6, 0, 0x777777, line); /* picture moved up-left: half of its first column is cut off */
    check(line[0] == 0 && line[1] == 0xFF0000 && line[2] == 0xFF0000 && line[3] == 0x777777, "row: moved so a part is outside");

    unsigned big[16];
    for (int i = 0; i < 16; i++)
        big[i] = 0x808080;
    unsigned small_[2];
    imgview_row(big, 4, 4, 50, 0, 0, 2, 0, 0, small_);
    check(small_[0] == 0x808080 && small_[1] == 0x808080, "row: zoomed out, mean of a flat colour");
    unsigned one[1];
    unsigned chk[4] = {0xFFFFFF, 0, 0, 0xFFFFFF};
    imgview_row(chk, 2, 2, 50, 0, 0, 1, 0, 0, one);
    check(one[0] == 0x808080, "row: zoomed out, a checker mixes to grey");

    return ok ? 0 : 1;
}
