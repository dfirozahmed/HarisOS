/* Image viewer maths: zoom, position and drawing a row of a zoomed picture (pure, no hardware: tests/test_imgview.c).
 *
 * The picture is shown at `pct` percent (100 = one picture pixel per screen pixel). Its top-left corner lies at (ox, oy)
 * inside the view, measured from the view's own top-left. A picture smaller than the view is centred; a bigger one may be
 * moved but never so far that an edge of it leaves a gap inside the view. */
#ifndef HARIS_IMGVIEW_H
#define HARIS_IMGVIEW_H

#define IMGV_MIN_PCT 5
#define IMGV_MAX_PCT 3200

int imgview_fit_pct(int iw, int ih, int vw, int vh);               /* the largest zoom that shows all of it, at most 100 */
int imgview_clamp_pct(int pct);                                    /* into IMGV_MIN_PCT .. IMGV_MAX_PCT */
int imgview_step_pct(int pct, int dir);                            /* the next zoom step up (dir > 0) or down (dir < 0) */
int imgview_scaled(int side, int pct);                             /* side * pct / 100, at least 1 */
void imgview_clamp(int *ox, int *oy, int sw, int sh, int vw, int vh); /* sw x sh = the zoomed picture's size */
void imgview_center(int *ox, int *oy, int sw, int sh, int vw, int vh);
/* zoom from old_pct to new_pct keeping the picture point under (cx, cy) of the view where it is */
void imgview_zoom_at(int *ox, int *oy, int old_pct, int new_pct, int cx, int cy);
/* Row y of the view (vw pixels, 0x00RRGGBB) into line: `bg` outside the picture, else the picture sampled at pct (the
 * nearest pixel when zooming in, the mean of four when zooming out). */
void imgview_row(const unsigned int *src, int iw, int ih, int pct, int ox, int oy, int vw, int y, unsigned int bg,
                 unsigned int *line);

#endif
