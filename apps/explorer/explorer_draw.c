/* File Explorer: icons, sidebar, file list, status bar and the window's draw function. */
#include "explorer.h"

void ex_draw_folder_icon(int cx, int cy, int s)
{
    int x = cx - s / 2, y = cy - s / 2;
    int h = s * 3 / 4;
    int top = y + (s - h) / 2;
    gfx_fill_round_rect(x, top, s * 2 / 5, h / 3 + 3, 3, gfx_rgb(224, 148, 36));
    gfx_fill_round_rect(x, top + 3, s, h - 3, 4, gfx_rgb(232, 160, 44));
    gfx_fill_round_rect(x, top + 6, s, h - 6, 4, gfx_rgb(255, 204, 92));
    gfx_fill_rect_alpha(x + 3, top + 7, s - 6, 1, 255, 255, 255, 90);
}

static void draw_file_icon(int cx, int cy, int s, int kind)
{
    UiRgb c = ex_kind_color(kind);
    int w = s * 3 / 4, h = s;
    int x = cx - w / 2, y = cy - h / 2;
    int fold = w / 3;
    gfx_fill_round_rect(x, y, w, h, 3, gfx_rgb(236, 241, 252));
    int tri[6] = {(x + w - fold) * 16, y * 16, (x + w) * 16, (y + fold) * 16, (x + w - fold) * 16, (y + fold) * 16};
    gfx_fill_poly_q(tri, 3, gfx_rgb(178, 190, 216), 255);
    gfx_fill_round_rect(x + 3, y + h - 9, w - 6, 5, 2, ui_color(c));
    gfx_fill_rect(x + 3, y + h - 14, w - 6 - w / 4, 2, ui_color(ui_rgb_mix(c, ui_rgb(236, 241, 252), 150)));
}

void ex_draw_icon(int cx, int cy, int s, int kind)
{
    if (kind == FK_FOLDER)
        ex_draw_folder_icon(cx, cy, s);
    else
        draw_file_icon(cx, cy, s, kind);
}

static void draw_sidebar(const Layout *l, const UiTheme *t)
{
    UiRect s = l->side;
    gfx_fill_rect_alpha(s.x, s.y, s.w, s.h, t->surface_alt.r, t->surface_alt.g, t->surface_alt.b, 36);
    gfx_vline(s.x + s.w - 1, s.y, s.h, ui_color(t->border));
    ui_text_in_color(ui_rect(s.x + 16, s.y + 8, s.w - 24, 22), "EX_PLACES", UI_TEXT_SMALL, UI_ALIGN_LEFT, t->text_dim);

    int y = s.y + 32;
    for (int i = 0; i < PLACE_N && y + PLACE_H <= s.y + s.h; i++, y += PLACE_H + 2)
    {
        int id = ex_place_id(i);
        if (id < 0)
            continue;
        /* "This PC" is current only at the root; the others while inside them */
        int here = id == VFS_ROOT ? EX_CUR == VFS_ROOT : vfs_is_under(EX_CUR, id);
        UiRect r = ui_rect(s.x + 8, y, s.w - 16, PLACE_H);
        if (!here && i == EX_HOVER_PLACE)
            gfx_fill_round_rect_alpha(r.x, r.y, r.w, r.h, t->radius_s, ui_color(t->accent), 20);
        if (here)
        {
            gfx_fill_round_rect_alpha(r.x, r.y, r.w, r.h, t->radius_s, ui_color(t->accent), 44);
            gfx_fill_round_rect(r.x, r.y + 8, 3, r.h - 16, 1, ui_color(t->accent_hi));
        }
        ex_draw_icon(r.x + 22, r.y + r.h / 2, 20, FK_FOLDER);
        ui_text_in_color(ui_rect(r.x + 42, r.y, r.w - 46, r.h), EX_PLACES[i].label, here ? UI_TEXT_LABEL : UI_TEXT_BODY,
                         UI_ALIGN_LEFT, here ? t->text : t->text_dim);
    }

    /* storage: how much of the node pool is in use */
    int used = vfs_count();
    UiRect st = ui_rect(s.x + 16, s.y + s.h - 62, s.w - 32, 54);
    if (st.y > y)
    {
        ui_text_in_color(ui_rect(st.x, st.y, st.w, 20), "RAM volume", UI_TEXT_LABEL, UI_ALIGN_LEFT, t->text);
        ui_progress(ui_rect(st.x, st.y + 24, st.w, 8), used * 100 / VFS_MAX_NODES);
        char line[40];
        int o = ex_put_uint(line, 0, used);
        o = ex_put_str(line, o, sizeof line, " of ");
        o = ex_put_uint(line, o, VFS_MAX_NODES);
        o = ex_put_str(line, o, sizeof line, " entries");
        line[o] = 0;
        ui_text_in_color(ui_rect(st.x, st.y + 34, st.w, 18), line, UI_TEXT_SMALL, UI_ALIGN_LEFT, t->text_dim);
    }
}

Cols ex_columns(const Layout *l)
{
    Cols c;
    c.show_type = l->list.w >= 360;
    int right = l->list.x + l->list.w - 14; /* leave room for the scrollbar */
    c.size_x = right - SIZE_COL_W;
    c.type_x = c.show_type ? c.size_x - TYPE_COL_W : c.size_x;
    return c;
}

/* a column title; the sorted column is brighter and has a small up / down triangle */
static void head_cell(UiRect cell, const char *label, UiAlign align, int key, const UiTheme *t)
{
    int active = EX_SORT_KEY == key;
    ui_text_in_color(cell, label, UI_TEXT_SMALL, align, active ? t->text : t->text_dim);
    if (!active)
        return;
    int tw = ui_text_width(label, UI_TEXT_SMALL);
    int ax = align == UI_ALIGN_RIGHT ? cell.x + cell.w - tw - 10 : cell.x + tw + 9;
    int ay = cell.y + cell.h / 2;
    int up[6] = {(ax - 4) * 16, (ay + 2) * 16, (ax + 4) * 16, (ay + 2) * 16, ax * 16, (ay - 3) * 16};
    int down[6] = {(ax - 4) * 16, (ay - 2) * 16, (ax + 4) * 16, (ay - 2) * 16, ax * 16, (ay + 3) * 16};
    gfx_fill_poly_q(EX_SORT_DESC ? down : up, 3, ui_color(t->accent_hi), 255);
}

static void draw_list(const Layout *l, const UiTheme *t)
{
    Cols cols = ex_columns(l);
    int show_type = cols.show_type, size_x = cols.size_x, type_x = cols.type_x;

    /* column header (click a title to sort by it) */
    UiRect h = l->head;
    head_cell(ui_rect(h.x + 44, h.y, 200, h.h), "Name", UI_ALIGN_LEFT, VFS_SORT_NAME, t);
    if (show_type)
        head_cell(ui_rect(type_x, h.y, TYPE_COL_W, h.h), "Type", UI_ALIGN_LEFT, VFS_SORT_TYPE, t);
    head_cell(ui_rect(size_x, h.y, SIZE_COL_W - 6, h.h), "Size", UI_ALIGN_RIGHT, VFS_SORT_SIZE, t);
    ui_separator(h.x + 8, h.y + h.h - 1, h.w - 16);

    if (EX_NITEMS == 0)
    {
        ex_draw_folder_icon(l->list.x + l->list.w / 2, l->list.y + l->list.h / 2 - 22, 40);
        ui_text_in_color(ui_rect(l->list.x, l->list.y + l->list.h / 2, l->list.w, 24), "This folder is empty",
                         UI_TEXT_BODY, UI_ALIGN_CENTER, t->text_dim);
        return;
    }

    GfxClip saved = gfx_get_clip();
    ui_clip(l->list);
    int vis = ex_visible_rows(l);
    for (int i = 0; i <= vis && EX_SCROLL + i < EX_NITEMS; i++)
    {
        int ri = EX_SCROLL + i;
        const VfsNode *n = vfs_node(EX_ITEMS[ri]);
        if (!n)
            continue;
        UiRect r = ui_rect(l->list.x + 6, l->list.y + i * ROW_H, l->list.w - 12 - 8, ROW_H - 2);
        if (ri == EX_SEL)
        {
            gfx_fill_round_rect_alpha(r.x, r.y, r.w, r.h, t->radius_s, ui_color(t->accent), 46);
            gfx_fill_round_rect(r.x, r.y + 7, 3, r.h - 14, 1, ui_color(t->accent_hi));
        }
        else if (ri == EX_HOVER_ROW)
        {
            gfx_fill_round_rect_alpha(r.x, r.y, r.w, r.h, t->radius_s, ui_color(t->accent), 20);
        }
        else if (ri & 1)
        {
            gfx_fill_round_rect_alpha(r.x, r.y, r.w, r.h, t->radius_s, ui_color(t->surface_alt), 40);
        }
        int kind = ex_kind_of(n);
        ex_draw_icon(r.x + 24, r.y + r.h / 2, ICON_PX, kind);
        if (EX_CLIP_CUT && EX_ITEMS[ri] == EX_CLIP_ID) /* waiting to be moved: faded */
            gfx_fill_round_rect_alpha(r.x + 12, r.y + 4, ICON_PX + 2, ICON_PX + 2, 4, ui_color(t->surface), 140);
        if (EX_ITEMS[ri] == EX_RENAME_ID && EX_WIN)
            ui_edit_draw(ui_rect(r.x + 40, r.y + 2, type_x - r.x - 46, r.h - 4), &EX_WIN->edits[0], "", 1);
        else
            ui_text_in(ui_rect(r.x + 42, r.y, type_x - r.x - 46, r.h), n->name, ri == EX_SEL ? UI_TEXT_LABEL : UI_TEXT_BODY,
                       UI_ALIGN_LEFT);
        if (show_type)
            ui_text_in_color(ui_rect(type_x, r.y, TYPE_COL_W, r.h), ex_kind_name(kind), UI_TEXT_SMALL, UI_ALIGN_LEFT,
                             t->text_dim);
        if (!n->is_dir)
        {
            char sz[20];
            ex_fmt_size(n->size, sz);
            ui_text_in_color(ui_rect(size_x, r.y, SIZE_COL_W - 6, r.h), sz, UI_TEXT_SMALL, UI_ALIGN_RIGHT, t->text_dim);
        }
    }
    gfx_restore_clip(saved);

    /* scrollbar */
    if (EX_NITEMS > vis)
    {
        int track_h = l->list.h - 8;
        int thumb_h = track_h * vis / EX_NITEMS;
        if (thumb_h < 24)
            thumb_h = 24;
        int span = EX_NITEMS - vis;
        int thumb_y = l->list.y + 4 + (track_h - thumb_h) * EX_SCROLL / span;
        int tx = l->list.x + l->list.w - 9;
        gfx_fill_round_rect_alpha(tx, l->list.y + 4, 4, track_h, 2, ui_color(t->surface_alt), 120);
        gfx_fill_round_rect(tx, thumb_y, 4, thumb_h, 2, ui_color(t->text_dim));
    }
}

static void draw_status(const Layout *l, const UiTheme *t)
{
    UiRect s = l->status;
    gfx_fill_rect_alpha(s.x, s.y, s.w, s.h, t->surface_alt.r, t->surface_alt.g, t->surface_alt.b, 70);
    ui_separator(s.x, s.y, s.w);

    char line[96];
    int o = 0;
    UiRgb color = t->text_dim;
    if (EX_NOTE)
    {
        o = ex_put_str(line, 0, sizeof line, EX_NOTE);
        color = t->warning;
    }
    else if (EX_SEL >= 0 && EX_SEL < EX_NITEMS && vfs_node(EX_ITEMS[EX_SEL]))
    {
        const VfsNode *n = vfs_node(EX_ITEMS[EX_SEL]);
        o = ex_put_str(line, 0, sizeof line, n->name);
        if (!n->is_dir)
        {
            char sz[20];
            ex_fmt_size(n->size, sz);
            o = ex_put_str(line, o, sizeof line, "   -   ");
            o = ex_put_str(line, o, sizeof line, sz);
        }
        color = t->text;
    }
    else
    {
        o = ex_put_uint(line, 0, EX_NITEMS);
        o = ex_put_str(line, o, sizeof line, EX_NITEMS == 1 ? " item" : " items");
    }
    line[o] = 0;
    int right_w = s.w >= 420 ? 190 : 0;
    ui_text_in_color(ui_rect(s.x + 14, s.y, s.w - 28 - right_w, s.h), line, UI_TEXT_SMALL, UI_ALIGN_LEFT, color);
    if (right_w)
    {
        char cnt[40];
        int c = ex_put_uint(cnt, 0, EX_NITEMS);
        c = ex_put_str(cnt, c, sizeof cnt, EX_NITEMS == 1 ? " item" : " items");
        cnt[c] = 0;
        ui_text_in_color(ui_rect(s.x + s.w - right_w - 14, s.y, right_w, s.h), cnt, UI_TEXT_SMALL, UI_ALIGN_RIGHT,
                         t->text_dim);
    }
}

void ex_draw(Window *w, UiRect body)
{
    const UiTheme *t = ui_theme();
    Layout l = ex_layout(body);
    ex_clamp_scroll(&l);
    ex_sync_buttons(); /* before the window draws its buttons */
    draw_sidebar(&l, t);
    draw_list(&l, t);
    ex_draw_toolbar(&l, t);
    draw_status(&l, t);
    if (EX_MENU.open && w != wm_top())
        EX_MENU.open = 0; /* focus moved elsewhere (another window or the delete dialog) */
    ex_menu_draw(t);
    if (EX_TIP_SHOWN && !EX_MENU.open)
        ex_tip_draw(w, body, t);
}

