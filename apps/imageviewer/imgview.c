/* Image viewer maths: see imgview.h. */
#include "imgview.h"

static const int STEPS[] = {5, 10, 15, 25, 33, 50, 67, 75, 100, 125, 150, 200, 300, 400, 600, 800, 1200, 1600, 2400, 3200};
#define NSTEPS ((int)(sizeof STEPS / sizeof STEPS[0]))

int imgview_clamp_pct(int pct)
{
    return pct < IMGV_MIN_PCT ? IMGV_MIN_PCT : (pct > IMGV_MAX_PCT ? IMGV_MAX_PCT : pct);
}

int imgview_fit_pct(int iw, int ih, int vw, int vh)
{
    if (iw <= 0 || ih <= 0 || vw <= 0 || vh <= 0)
        return 100;
    long long a = (long long)vw * 100 / iw, b = (long long)vh * 100 / ih;
    long long p = a < b ? a : b;
    return p > 100 ? 100 : imgview_clamp_pct((int)p);
}

int imgview_step_pct(int pct, int dir)
{
    if (dir > 0)
    {
        for (int i = 0; i < NSTEPS; i++)
            if (STEPS[i] > pct)
                return STEPS[i];
        return IMGV_MAX_PCT;
    }
    for (int i = NSTEPS - 1; i >= 0; i--)
        if (STEPS[i] < pct)
            return STEPS[i];
    return IMGV_MIN_PCT;
}

int imgview_scaled(int side, int pct)
{
    long long s = (long long)side * pct / 100;
    return s < 1 ? 1 : (s > 1000000 ? 1000000 : (int)s);
}

static int clamp_axis(int o, int s, int v)
{
    if (s <= v)
        return (v - s) / 2;
    return o > 0 ? 0 : (o < v - s ? v - s : o);
}

void imgview_clamp(int *ox, int *oy, int sw, int sh, int vw, int vh)
{
    *ox = clamp_axis(*ox, sw, vw);
    *oy = clamp_axis(*oy, sh, vh);
}

void imgview_center(int *ox, int *oy, int sw, int sh, int vw, int vh)
{
    *ox = (vw - sw) / 2;
    *oy = (vh - sh) / 2;
}

void imgview_zoom_at(int *ox, int *oy, int old_pct, int new_pct, int cx, int cy)
{
    if (old_pct <= 0)
        return;
    *ox = cx - (int)((long long)(cx - *ox) * new_pct / old_pct);
    *oy = cy - (int)((long long)(cy - *oy) * new_pct / old_pct);
}

static unsigned mean4(unsigned a, unsigned b, unsigned c, unsigned d)
{
    unsigned r = 0;
    for (int s = 0; s < 24; s += 8)
        r |= (((a >> s & 255) + (b >> s & 255) + (c >> s & 255) + (d >> s & 255) + 2) >> 2) << s;
    return r;
}

void imgview_row(const unsigned int *src, int iw, int ih, int pct, int ox, int oy, int vw, int y, unsigned int bg,
                 unsigned int *line)
{
    int sw = imgview_scaled(iw, pct), sh = imgview_scaled(ih, pct);
    int x0 = ox < 0 ? 0 : ox, x1 = ox + sw > vw ? vw : ox + sw; /* columns of the view that show the picture */
    if (y < oy || y >= oy + sh || x0 >= x1)
    {
        for (int x = 0; x < vw; x++)
            line[x] = bg;
        return;
    }
    for (int x = 0; x < x0; x++)
        line[x] = bg;
    for (int x = x1; x < vw; x++)
        line[x] = bg;
    /* source coordinates in 1/256 pixel: picture pixel = (view pixel + 0.5 - origin) * 100 / pct */
    long long step = 100LL * 256 / pct;
    long long sy = ((long long)(2 * (y - oy) + 1) * 100 * 128) / pct;
    int zoom_out = pct < 100;
    for (int x = x0; x < x1; x++)
    {
        long long sx = ((long long)(2 * (x - ox) + 1) * 100 * 128) / pct;
        int px = (int)(sx >> 8), py = (int)(sy >> 8);
        if (!zoom_out)
        {
            px = px < 0 ? 0 : (px >= iw ? iw - 1 : px);
            py = py < 0 ? 0 : (py >= ih ? ih - 1 : py);
            line[x] = src[(long long)py * iw + px];
        }
        else
        {
            long long q = step / 4; /* four taps inside the span this screen pixel covers */
            int xa = (int)((sx - q) >> 8), xb = (int)((sx + q) >> 8), ya = (int)((sy - q) >> 8), yb = (int)((sy + q) >> 8);
            xa = xa < 0 ? 0 : (xa >= iw ? iw - 1 : xa);
            xb = xb < 0 ? 0 : (xb >= iw ? iw - 1 : xb);
            ya = ya < 0 ? 0 : (ya >= ih ? ih - 1 : ya);
            yb = yb < 0 ? 0 : (yb >= ih ? ih - 1 : yb);
            line[x] = mean4(src[(long long)ya * iw + xa], src[(long long)ya * iw + xb], src[(long long)yb * iw + xa],
                            src[(long long)yb * iw + xb]);
        }
    }
}
