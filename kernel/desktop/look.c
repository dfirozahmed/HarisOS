/* HarisOS desktop look: see include/look.h. */
#include "look.h"
#include "exec.h"
#include "gfx.h"
#include "heap.h"
#include "picture.h"
#include "vfs.h"
#include "wm.h"
#include "desktop_internal.h"

#define CFG_NAME "THEME.CFG"
#define CFG_MAX 512
#define PATH_MAX_LEN 192
#define PICTURE_MAX_FILE (64u * 1024u * 1024u)

static int SCREENOFF = 300; /* seconds, 0 = never */
static int OWN_ACCENT;
static UiRgb ACCENT;
static unsigned int *PIC; /* the picture in use, screen size (ui_set_wallpaper_image) */
static int PIC_NODE = -1;
static const char *MSG = "";

/* ---- small string helpers (no libc) ---- */
static int slen(const char *s)
{
    int n = 0;
    while (s[n])
        n++;
    return n;
}

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

static int same_ci(const char *a, const char *b)
{
    while (*a && lower(*a) == lower(*b))
        a++, b++;
    return !*a && !*b;
}

/* what follows `prefix` at the start of s (ignoring case), 0 if s does not start with it */
static const char *after(const char *s, const char *prefix)
{
    while (*prefix && lower(*s) == lower(*prefix))
        s++, prefix++;
    return *prefix ? 0 : s;
}

static char *put(char *o, const char *s)
{
    while (*s)
        *o++ = *s++;
    return o;
}

static int hexval(char c)
{
    c = lower(c);
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* ---- applying ---- */
static void show(int wallpaper_too)
{
    ui_set_look(DARK, OWN_ACCENT ? &ACCENT : 0);
    if (wallpaper_too || ui_wallpaper_current() == 0) /* the theme's own wallpaper takes the theme's colours */
        WALL_VALID = 0;
    wm_invalidate(); /* every window, the taskbar and the menus */
}

/* the folder THEME.CFG goes in: where it is, else where the boot disk's files are, else the first drive (not the RAM Disk) that
 * can be written; with writable = 0 a locked drive counts too (to read from it, no unlock is needed) */
static int settings_folder(int writable)
{
    static const char *const near[3] = {CFG_NAME, "WIFI.CFG", "INIT.ELF"};
    for (int i = 0; i < 3; i++)
    {
        int n = exec_find(near[i]);
        const VfsNode *vn = n >= 0 ? vfs_node(n) : 0;
        if (vn && vn->vol >= 0)
        {
            VfsVolInfo vi;
            if (vfs_volume_info(vn->vol, &vi) == 0 && !same_ci(vi.fs_name, "RAM") && (!writable || (vi.writable && !vi.locked)))
                return vn->parent;
        }
    }
    for (int v = 0; v < vfs_volume_count(); v++)
    {
        VfsVolInfo vi;
        if (vfs_volume_info(v, &vi) == 0 && !same_ci(vi.fs_name, "RAM") && (!writable || (vi.writable && !vi.locked)))
            return vi.node;
    }
    return -1;
}

static int cfg_folder(void)
{
    return settings_folder(1);
}

int look_config_folder(void)
{
    return settings_folder(1);
}

int look_data_folder(void)
{
    return settings_folder(0);
}

static void save(void)
{
    char text[CFG_MAX], *o = text;
    o = put(o, "theme=");
    o = put(o, DARK ? "dark" : "light");
    o = put(o, "\r\naccent=");
    if (OWN_ACCENT)
    {
        static const char HEX[] = "0123456789ABCDEF";
        unsigned char c[3] = {ACCENT.r, ACCENT.g, ACCENT.b};
        for (int i = 0; i < 3; i++)
        {
            *o++ = HEX[c[i] >> 4];
            *o++ = HEX[c[i] & 15];
        }
    }
    else
        o = put(o, "theme");
    o = put(o, "\r\nwallpaper=");
    if (PIC_NODE >= 0)
    {
        char path[PATH_MAX_LEN];
        vfs_path(PIC_NODE, path, PATH_MAX_LEN);
        o = put(o, path);
    }
    else
        *o++ = (char)('0' + ui_wallpaper_current());
    o = put(o, "\r\nscreenoff=");
    {
        char dg[12];
        int k = 0;
        unsigned v = (unsigned)SCREENOFF;
        do
            dg[k++] = (char)('0' + v % 10);
        while ((v /= 10) != 0);
        while (k)
            *o++ = dg[--k];
    }
    o = put(o, "\r\nbrightness=");
    {
        char dg[12];
        int k = 0;
        unsigned v = (unsigned)gfx_brightness();
        do
            dg[k++] = (char)('0' + v % 10);
        while ((v /= 10) != 0);
        while (k)
            *o++ = dg[--k];
    }
    o = put(o, "\r\n");
    int dir = cfg_folder();
    if (dir < 0 || vfs_save(dir, CFG_NAME, text, (unsigned)(o - text)) < 0)
        MSG = dir < 0 ? "Shown, but not saved: no drive that can be written" : "Shown, but THEME.CFG could not be written";
}

/* ---- the picture ---- */
static int load_picture(int node)
{
    const VfsNode *n = vfs_node(node);
    int W = (int)gfx_width(), H = (int)gfx_height();
    if (!n || n->is_dir || n->size == 0 || n->size > PICTURE_MAX_FILE)
    {
        MSG = "Not a picture file (or bigger than 64 MB)";
        return -1;
    }
    unsigned size = n->size;
    unsigned char *file = kmalloc(size);
    unsigned int *pic = file ? kmalloc((unsigned long long)W * (unsigned long long)H * 4u) : 0;
    if (!file || !pic)
    {
        kfree(file);
        MSG = "Not enough memory for the picture";
        return -1;
    }
    for (unsigned done = 0; done < size;)
    {
        unsigned want = size - done > 65536 ? 65536 : size - done;
        int got = vfs_read(node, done, file + done, want);
        if (got <= 0)
        {
            kfree(file);
            kfree(pic);
            MSG = "The picture could not be read from the drive";
            return -1;
        }
        done += (unsigned)got;
    }
    int r = picture_cover(file, size, pic, W, H);
    kfree(file);
    if (r != 0)
    {
        kfree(pic);
        MSG = "Not a picture this can show: BMP (24 / 32 bit), PNG or JPEG (not progressive)";
        return -1;
    }
    ui_set_wallpaper_image(pic); /* the new one first: the old one is not in use any more when it is freed */
    kfree(PIC);
    PIC = pic;
    PIC_NODE = node;
    return 0;
}

static void drop_picture(void)
{
    ui_set_wallpaper_image(0);
    kfree(PIC);
    PIC = 0;
    PIC_NODE = -1;
}

/* ---- the user's changes ---- */
int look_dark(void) { return DARK; }

void look_set_dark(int dark)
{
    MSG = "";
    DARK = dark ? 1 : 0;
    show(0);
    save();
}

UiRgb look_accent(void) { return ui_theme()->accent; }
int look_accent_own(void) { return OWN_ACCENT; }

void look_set_accent(const UiRgb *accent)
{
    MSG = "";
    OWN_ACCENT = accent != 0;
    if (accent)
        ACCENT = *accent;
    show(0);
    save();
}

int look_wallpaper(void) { return ui_wallpaper_current(); }
int look_picture(void) { return PIC_NODE; }

void look_set_wallpaper(int preset)
{
    MSG = "";
    drop_picture();
    ui_set_wallpaper(preset);
    show(1);
    save();
}

int look_set_picture(int node)
{
    MSG = "";
    if (load_picture(node) != 0)
        return -1;
    show(1);
    save();
    return 0;
}

const char *look_message(void) { return MSG; }

static int add_pictures(int dir, int *ids, int n, int max, int depth)
{
    int list[256];
    int k = vfs_list(dir, list, 256);
    if (k > 256)
        k = 256;
    for (int i = 0; i < k && n < max; i++)
    {
        const VfsNode *vn = vfs_node(list[i]);
        if (!vn)
            continue;
        if (!vn->is_dir && picture_is_name(vn->name))
            ids[n++] = list[i];
        else if (vn->is_dir && depth == 0 && (same_ci(vn->name, "Pictures") || same_ci(vn->name, "Wallpapers")))
            n = add_pictures(list[i], ids, n, max, 1);
    }
    return n;
}

int look_find_pictures(int *ids, int max)
{
    int n = 0;
    for (int v = 0; v < vfs_volume_count() && n < max; v++)
    {
        VfsVolInfo vi;
        if (vfs_volume_info(v, &vi) != 0 || same_ci(vi.fs_name, "RAM")) /* the RAM Disk's files have no content */
            continue;
        n = add_pictures(vi.node, ids, n, max, 0);
    }
    return n;
}

/* ---- THEME.CFG at boot ---- */
int look_screenoff(void) { return SCREENOFF; }
void look_set_screenoff(int seconds)
{
    SCREENOFF = seconds < 0 ? 0 : seconds;
    MSG = "";
    save();
}

/* The brightness itself is applied by the Display Settings app (gfx_set_brightness); this only writes THEME.CFG. It is called
 * when a change is finished (slider released, key, Reset, window closed), not on every step of a drag: a drive write per pixel
 * of slider movement would be slow. */
void look_save_brightness(void)
{
    MSG = "";
    save();
}

void look_load(void)
{
    char text[CFG_MAX + 1];
    int node = exec_find(CFG_NAME);
    int got = node >= 0 ? vfs_read(node, 0, text, CFG_MAX) : -1;
    if (got > 0)
    {
        text[got] = 0;
        for (char *line = text; *line;)
        {
            char *end = line;
            while (*end && *end != '\n' && *end != '\r')
                end++;
            char keep = *end;
            *end = 0;
            const char *val;
            if ((val = after(line, "theme=")))
                DARK = !same_ci(val, "light");
            else if ((val = after(line, "accent=")) && slen(val) == 6)
            {
                int v[6], okv = 1;
                for (int i = 0; i < 6; i++)
                    okv &= (v[i] = hexval(val[i])) >= 0;
                if (okv)
                {
                    ACCENT = ui_rgb((unsigned char)(v[0] * 16 + v[1]), (unsigned char)(v[2] * 16 + v[3]), (unsigned char)(v[4] * 16 + v[5]));
                    OWN_ACCENT = 1;
                }
            }
            else if ((val = after(line, "screenoff=")))
            {
                int v = 0, nd = 0;
                for (; val[nd] >= '0' && val[nd] <= '9' && nd < 7; nd++)
                    v = v * 10 + (val[nd] - '0');
                if (nd > 0)
                    SCREENOFF = v;
            }
            else if ((val = after(line, "brightness=")))
            {
                int v = 0, nd = 0;
                for (; val[nd] >= '0' && val[nd] <= '9' && nd < 4; nd++)
                    v = v * 10 + (val[nd] - '0');
                if (nd > 0)
                    gfx_set_brightness(v); /* clamped to GFX_BRIGHTNESS_MIN..MAX */
            }
            else if ((val = after(line, "wallpaper=")))
            {
                if (val[0] >= '0' && val[0] <= '9' && !val[1])
                    ui_set_wallpaper(val[0] - '0');
                else if (val[0] == '/')
                {
                    int pic = vfs_lookup(val);
                    if (pic < 0)
                        MSG = "The wallpaper picture of THEME.CFG was not found";
                    if (pic < 0 || load_picture(pic) != 0) /* load_picture says why it failed */
                        ui_set_wallpaper(0); /* the picture is gone or broken: the theme's own */
                }
            }
            *end = keep;
            line = *end ? end + 1 : end;
        }
    }
    ui_set_look(DARK, OWN_ACCENT ? &ACCENT : 0);
    WALL_VALID = 0;
}
