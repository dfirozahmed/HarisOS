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

/* a drive: a flat grey box with a dark base strip and a status light */
void ex_draw_drive_icon(int cx, int cy, int s)
{
    int x = cx - s / 2, h = s * 3 / 5, y = cy - h / 2;
    gfx_fill_round_rect(x, y, s, h, 4, gfx_rgb(196, 205, 226));
    gfx_fill_rect_alpha(x + 3, y + 2, s - 6, 1, 255, 255, 255, 140);
    int sh = h * 2 / 5;
    gfx_fill_round_rect(x + 2, y + h - sh - 2, s - 4, sh, 3, gfx_rgb(104, 118, 150));
    gfx_fill_circle(x + s - 6, y + h - sh / 2 - 2, s >= 24 ? 2 : 1, gfx_rgb(86, 214, 140));
    gfx_fill_round_rect(x + 5, y + h - sh / 2 - 3, s / 3, 2, 1, gfx_rgb(150, 164, 196));
}

/* a padlock: closed (locked) or with the shackle lifted (unlocked) */
void ex_draw_lock_icon(int cx, int cy, int s, int locked, UiRgb c)
{
    GfxColor col = ui_color(c);
    int bw = s * 7 / 10, bh = s / 2;
    int bx = cx - bw / 2, by = cy - bh / 2 + s / 6;
    gfx_fill_round_rect(bx, by, bw, bh, 2, col);
    int lx = (cx - bw / 4) * 16, rx = (cx + bw / 4) * 16, top = (by - s / 4) * 16, w = s >= 16 ? 28 : 20;
    gfx_line_q(lx, by * 16, lx, top, w, col, 255);
    gfx_line_q(lx, top, rx, top, w, col, 255);
    if (locked)
        gfx_line_q(rx, top, rx, by * 16, w, col, 255);
    else
        gfx_line_q(rx, top, rx, top + s * 5, w, col, 255);
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
    if (kind == FK_DRIVE)
        ex_draw_drive_icon(cx, cy, s);
    else if (kind == FK_FOLDER)
        ex_draw_folder_icon(cx, cy, s);
    else
        draw_file_icon(cx, cy, s, kind);
}

/* "12.3 GB free of 31.9 GB" for the sidebar card and the drive rows */
static void free_text(const VfsVolInfo *vi, char *out, int cap, int with_total)
{
    int o = 0;
    if (!vi->total || vi->free == (unsigned long long)-1) /* size not known (the RAM Disk) */
    {
        out[0] = 0;
        return;
    }
    char sz[24];
    ex_fmt_size_big(vi->free, sz);
    o = ex_put_str(out, o, cap, sz);
    o = ex_put_str(out, o, cap, " free");
    if (with_total)
    {
        ex_fmt_size_big(vi->total, sz);
        o = ex_put_str(out, o, cap, " of ");
        o = ex_put_str(out, o, cap, sz);
    }
    out[o] = 0;
}

static int used_percent(const VfsVolInfo *vi)
{
    if (!vi->total || vi->free == (unsigned long long)-1 || vi->free > vi->total)
        return -1;
    return (int)((vi->total - vi->free) * 100 / vi->total);
}

/* "HARISOS (nvme0p1)" -> base "HARISOS" and sub "nvme0p1" (sub is "" if the name has no " (...)" at the end) */
static void split_drive_name(const char *name, char *base, char *sub, int cap)
{
    int n = 0, paren = -1;
    while (name[n])
        n++;
    for (int i = n - 1; i > 0; i--)
        if (name[i] == '(' && name[i - 1] == ' ' && name[n - 1] == ')')
        {
            paren = i;
            break;
        }
    int b = 0, s = 0;
    int end = paren > 0 ? paren - 1 : n;
    for (int i = 0; i < end && b < cap - 1; i++)
        base[b++] = name[i];
    base[b] = 0;
    for (int i = paren > 0 ? paren + 1 : n; paren > 0 && i < n - 1 && s < cap - 1; i++)
        sub[s++] = name[i];
    sub[s] = 0;
}

static void draw_sidebar(const Layout *l, const UiTheme *t)
{
    UiRect s = l->side;
    gfx_fill_rect_alpha(s.x, s.y, s.w, s.h, t->surface_alt.r, t->surface_alt.g, t->surface_alt.b, 36);
    gfx_vline(s.x + s.w - 1, s.y, s.h, ui_color(t->border));
    ui_text_in_color(ui_rect(s.x + 16, s.y + 8, s.w - 24, 22), "PLACES", UI_TEXT_SMALL, UI_ALIGN_LEFT, t->text_dim);

    int y = s.y + 32;
    int count = ex_place_count();
    for (int i = 0; i < count && y + PLACE_H <= s.y + s.h - 66; i++, y += PLACE_H + 2)
    {
        int id = ex_place_id(i);
        if (id < 0)
            continue;
        /* "This PC" is current only at the root; a drive while inside it */
        int here = id == VFS_ROOT ? EX_CUR == VFS_ROOT : vfs_is_under(EX_CUR, id);
        UiRect r = ui_rect(s.x + 8, y, s.w - 16, PLACE_H);
        if (!here && i == EX_HOVER_PLACE)
            gfx_fill_round_rect_alpha(r.x, r.y, r.w, r.h, t->radius_s, ui_color(t->accent), 20);
        if (here)
        {
            gfx_fill_round_rect_alpha(r.x, r.y, r.w, r.h, t->radius_s, ui_color(t->accent), 44);
            gfx_fill_round_rect(r.x, r.y + 8, 3, r.h - 16, 1, ui_color(t->accent_hi));
        }
        VfsVolInfo vi;
        int drive = id != VFS_ROOT && vfs_volume_info(vfs_volume_of(id), &vi) == 0;
        if (id == VFS_ROOT)
            ex_draw_drive_icon(r.x + 22, r.y + r.h / 2, 20); /* This PC */
        else
            ex_draw_drive_icon(r.x + 22, r.y + r.h / 2, 20);
        int right = (drive && vi.lockable) ? 22 : 0; /* room for the padlock */
        const char *label = id == VFS_ROOT ? "This PC" : vfs_node(id)->name;
        char base[VFS_NAME_MAX], sub[VFS_NAME_MAX];
        split_drive_name(label, base, sub, VFS_NAME_MAX);
        UiTextStyle st_main = here ? UI_TEXT_LABEL : UI_TEXT_BODY;
        UiRgb c_main = here ? t->text : t->text_dim;
        if (sub[0]) /* the drive's label, and under it the device it is on */
        {
            ui_text_in_color(ui_rect(r.x + 42, r.y + 1, r.w - 46 - right, 18), base, st_main, UI_ALIGN_LEFT, c_main);
            ui_text_in_color(ui_rect(r.x + 42, r.y + 16, r.w - 46 - right, 15), sub, UI_TEXT_SMALL, UI_ALIGN_LEFT, t->text_dim);
        }
        else
            ui_text_in_color(ui_rect(r.x + 42, r.y, r.w - 46 - right, r.h), label, st_main, UI_ALIGN_LEFT, c_main);
        if (right)
            ex_draw_lock_icon(r.x + r.w - 14, r.y + r.h / 2, 13, vi.locked, vi.locked ? t->warning : t->success);
    }

    /* the drive the current folder is on: how full it is */
    VfsVolInfo vi;
    UiRect st = ui_rect(s.x + 16, s.y + s.h - 62, s.w - 32, 54);
    if (st.y > y && ex_cur_drive(&vi))
    {
        ui_text_in_color(ui_rect(st.x, st.y, st.w, 20), vi.device[0] ? vi.device : "Drive", UI_TEXT_LABEL, UI_ALIGN_LEFT,
                         t->text);
        int pct = used_percent(&vi);
        char line[48];
        if (pct >= 0)
        {
            ui_progress(ui_rect(st.x, st.y + 24, st.w, 8), pct);
            free_text(&vi, line, sizeof line, 1);
        }
        else if (!vi.total) /* the RAM Disk: how much of the entry table is used */
        {
            int used = vfs_count();
            ui_progress(ui_rect(st.x, st.y + 24, st.w, 8), used * 100 / VFS_MAX_NODES);
            int o = ex_put_uint(line, 0, used);
            o = ex_put_str(line, o, sizeof line, " of ");
            o = ex_put_uint(line, o, VFS_MAX_NODES);
            o = ex_put_str(line, o, sizeof line, " entries");
            line[o] = 0;
        }
        else
            line[0] = 0;
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

/* one row of "This PC": a drive with its kind, a fill bar and the free space */
static void draw_drive_row(const VfsNode *n, UiRect r, int sel, int type_x, int size_x, int show_type, const UiTheme *t)
{
    VfsVolInfo vi;
    if (vfs_volume_info(n->vol, &vi) != 0)
        return;
    ex_draw_drive_icon(r.x + 24, r.y + r.h / 2, ICON_PX + 2);
    ui_text_in(ui_rect(r.x + 42, r.y + 3, type_x - r.x - 46, 16), n->name, sel ? UI_TEXT_LABEL : UI_TEXT_BODY, UI_ALIGN_LEFT);
    int pct = used_percent(&vi);
    if (pct >= 0)
    {
        int bw = type_x - r.x - 56;
        if (bw > 160)
            bw = 160;
        if (bw > 30)
            ui_progress(ui_rect(r.x + 44, r.y + r.h - 8, bw, 4), pct);
    }
    if (show_type)
    {
        ui_text_in_color(ui_rect(type_x, r.y, TYPE_COL_W - 4, r.h), vi.device, UI_TEXT_SMALL, UI_ALIGN_LEFT, t->text_dim);
    }
    char fr[48];
    free_text(&vi, fr, sizeof fr, 0);
    if (fr[0])
        ui_text_in_color(ui_rect(size_x - 30, r.y, SIZE_COL_W + 6, r.h), fr, UI_TEXT_SMALL, UI_ALIGN_RIGHT, t->text_dim);
    if (vi.lockable)
        ex_draw_lock_icon(r.x + r.w - 10, r.y + r.h / 2, 13, vi.locked, vi.locked ? t->warning : t->success);
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
    head_cell(ui_rect(size_x, h.y, SIZE_COL_W - 6, h.h), EX_CUR == VFS_ROOT ? "Free" : "Size", UI_ALIGN_RIGHT, VFS_SORT_SIZE, t);
    ui_separator(h.x + 8, h.y + h.h - 1, h.w - 16);

    if (EX_NITEMS == 0)
    {
        ex_draw_folder_icon(l->list.x + l->list.w / 2, l->list.y + l->list.h / 2 - 22, 40);
        ui_text_in_color(ui_rect(l->list.x, l->list.y + l->list.h / 2, l->list.w, 24),
                         EX_CUR == VFS_ROOT ? "No drives found" : "This folder is empty", UI_TEXT_BODY, UI_ALIGN_CENTER,
                         t->text_dim);
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
        if (ex_is_drive(n))
        {
            draw_drive_row(n, r, ri == EX_SEL, type_x, size_x, show_type, t);
            continue;
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

    UiRect pill;
    int has_pill = ex_lock_rect(l, &pill);
    int right_w = (s.w >= 480 ? 90 : 0) + (has_pill ? pill.w + 12 : 0);

    char line[128];
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
        VfsVolInfo dv;
        if (ex_is_drive(n) && vfs_volume_info(n->vol, &dv) == 0) /* a drive: its file system and free space */
        {
            if (dv.fs_name[0])
            {
                o = ex_put_str(line, o, sizeof line, "   -   ");
                o = ex_put_str(line, o, sizeof line, dv.fs_name);
            }
            char fr[48];
            free_text(&dv, fr, sizeof fr, 1);
            if (fr[0])
            {
                o = ex_put_str(line, o, sizeof line, "   -   ");
                o = ex_put_str(line, o, sizeof line, fr);
            }
        }
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
    ui_text_in_color(ui_rect(s.x + 14, s.y, s.w - 28 - right_w, s.h), line, UI_TEXT_SMALL, UI_ALIGN_LEFT, color);
    int rx = s.x + s.w - 14;
    if (has_pill)
    {
        VfsVolInfo vi;
        ex_cur_drive(&vi);
        UiRgb c = vi.locked ? t->warning : t->success;
        gfx_fill_round_rect_alpha(pill.x, pill.y, pill.w, pill.h, pill.h / 2, ui_color(c), 38);
        gfx_round_rect_alpha(pill.x, pill.y, pill.w, pill.h, pill.h / 2, 1, ui_color(c), 150);
        ex_draw_lock_icon(pill.x + 15, pill.y + pill.h / 2, 13, vi.locked, c);
        ui_text_in_color(ui_rect(pill.x + 26, pill.y, pill.w - 30, pill.h), vi.locked ? "Read-only" : "Writable", UI_TEXT_SMALL,
                         UI_ALIGN_LEFT, c);
        rx = pill.x - 12;
    }
    if (s.w >= 480)
    {
        char cnt[40];
        int c = ex_put_uint(cnt, 0, EX_NITEMS);
        c = ex_put_str(cnt, c, sizeof cnt, EX_NITEMS == 1 ? " item" : " items");
        cnt[c] = 0;
        ui_text_in_color(ui_rect(rx - 90, s.y, 90, s.h), cnt, UI_TEXT_SMALL, UI_ALIGN_RIGHT, t->text_dim);
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
        EX_MENU.open = 0; /* focus moved elsewhere (another window or a dialog) */
    ex_menu_draw(t);
    if (EX_TIP_SHOWN && !EX_MENU.open)
        ex_tip_draw(w, body, t);
}
