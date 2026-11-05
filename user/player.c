/* HarisOS user program: Player, a music player window.
 *
 *   player [FILE.MP3 | FILE.WAV | FOLDER]
 *
 * The window is a compositor surface (SYS_GUI_*, include/gui.h): this program draws every pixel itself, with the same pre-rendered
 * fonts as the desktop (kernel/graphics/font_data.c is compiled in). It shows the MP3 / WAV files of one folder as a play list and
 * plays them.
 *
 * Sound is not made here. Playing is a separate process, PLAY.ELF, started with the file and the second to start at; SYS_AUDIO_PLAY
 * blocks until the sound has finished, so the window (this program) stays free to react. The window
 *   - stops the sound with SYS_AUDIO_STOP (the player process notices within a few milliseconds and ends),
 *   - pauses by remembering the time and stopping, and continues by starting the player process at that time again,
 *   - seeks by starting it at another time,
 *   - notices that a song is over when the player process has ended on its own, and starts the next song.
 * The position shown is the time since the process was started plus where it started (the sound itself has no clock the program can
 * read); it can run a little ahead of what is heard at the start of a song. */
#include "libc.h"
#include <math.h>
#include "font.h"

#pragma clang diagnostic ignored "-Wunused-function"
#include "mp3dec.c" /* only mp3_estimate_ms and the frame locator are used here: how long a song is, its sample rate and bitrate */
#include "../kernel/graphics/font_data.c"

/* key codes of include/keyboard.h that the window events carry */
#define KEY_ESC 256
#define KEY_ENTER 259
#define KEY_UP 266
#define KEY_DOWN 267
#define KEY_LEFT 268
#define KEY_RIGHT 269

#define WIN_W 560
#define WIN_H 400
#define MARGIN 24
#define MAX_TRACKS 128
#define ROW_H 26
#define LIST_Y 224
#define VISIBLE_ROWS ((WIN_H - LIST_Y - 12) / ROW_H)

/* colours: the desktop's dark theme (kernel/ui/ui_theme.c) */
#define C_BG 0x1B2134
#define C_PANEL 0x28304A
#define C_BORDER 0x3A4462
#define C_TEXT 0xECF1FC
#define C_DIM 0x94A0BE
#define C_ACCENT 0x3678F6
#define C_ACCENT_HI 0x5E9CFF

/* ------------------------------------------------------------ drawing ------------------------------------------------------------ */
static unsigned *PIX;

static unsigned blend(unsigned dst, unsigned src, int a) /* a = 0..255 coverage of src */
{
    if (a >= 255)
        return src;
    if (a <= 0)
        return dst;
    unsigned r = (((src >> 16) & 255) * (unsigned)a + ((dst >> 16) & 255) * (unsigned)(255 - a)) / 255;
    unsigned g = (((src >> 8) & 255) * (unsigned)a + ((dst >> 8) & 255) * (unsigned)(255 - a)) / 255;
    unsigned b = ((src & 255) * (unsigned)a + (dst & 255) * (unsigned)(255 - a)) / 255;
    return r << 16 | g << 8 | b;
}

static void put_px(int x, int y, unsigned c, int a)
{
    if (x >= 0 && x < WIN_W && y >= 0 && y < WIN_H)
        PIX[y * WIN_W + x] = blend(PIX[y * WIN_W + x], c, a);
}

static void fill_rect(int x, int y, int w, int h, unsigned c)
{
    for (int yy = y < 0 ? 0 : y; yy < y + h && yy < WIN_H; yy++)
        for (int xx = x < 0 ? 0 : x; xx < x + w && xx < WIN_W; xx++)
            PIX[yy * WIN_W + xx] = c;
}

/* A rounded rectangle: the corners are covered by how far the pixel centre is inside the corner circle. */
static void fill_round(int x, int y, int w, int h, int r, unsigned c)
{
    if (r > w / 2)
        r = w / 2;
    if (r > h / 2)
        r = h / 2;
    for (int yy = 0; yy < h; yy++)
        for (int xx = 0; xx < w; xx++)
        {
            int cx = xx < r ? r : (xx >= w - r ? w - 1 - r : xx);
            int cy = yy < r ? r : (yy >= h - r ? h - 1 - r : yy);
            float dx = (float)(xx - cx), dy = (float)(yy - cy);
            float d = (float)sqrt((double)(dx * dx + dy * dy));
            float cov = (float)r + 0.5f - d;
            int a = cov >= 1.f ? 255 : (cov <= 0.f ? 0 : (int)(cov * 255.f));
            if (a)
                put_px(x + xx, y + yy, c, a);
        }
}

static void fill_circle(int cx, int cy, int r, unsigned c)
{
    for (int yy = -r - 1; yy <= r + 1; yy++)
        for (int xx = -r - 1; xx <= r + 1; xx++)
        {
            float d = (float)sqrt((double)(xx * xx + yy * yy));
            float cov = (float)r + 0.5f - d;
            int a = cov >= 1.f ? 255 : (cov <= 0.f ? 0 : (int)(cov * 255.f));
            if (a)
                put_px(cx + xx, cy + yy, c, a);
        }
}

/* A triangle by 3 x 3 supersampling (the play / next / previous symbols). */
static float edge(float ax, float ay, float bx, float by, float px, float py) { return (bx - ax) * (py - ay) - (by - ay) * (px - ax); }

static void fill_tri(float x0, float y0, float x1, float y1, float x2, float y2, unsigned c)
{
    float minx = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2), maxx = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
    float miny = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2), maxy = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
    for (int y = (int)miny - 1; y <= (int)maxy + 1; y++)
        for (int x = (int)minx - 1; x <= (int)maxx + 1; x++)
        {
            int hit = 0;
            for (int sy = 0; sy < 3; sy++)
                for (int sx = 0; sx < 3; sx++)
                {
                    float px = (float)x + ((float)sx + 0.5f) / 3.f, py = (float)y + ((float)sy + 0.5f) / 3.f;
                    float e0 = edge(x0, y0, x1, y1, px, py), e1 = edge(x1, y1, x2, y2, px, py), e2 = edge(x2, y2, x0, y0, px, py);
                    if ((e0 >= 0 && e1 >= 0 && e2 >= 0) || (e0 <= 0 && e1 <= 0 && e2 <= 0))
                        hit++;
                }
            if (hit)
                put_px(x, y, c, hit * 255 / 9);
        }
}

/* ---- text: the same layout rules as gfx_text (kernel/graphics/font.c), drawn into PIX ---- */
static const FontGlyph *glyph_of(const Font *f, unsigned char ch)
{
    if (ch < f->first || ch > f->last)
        ch = '?';
    return &f->glyphs[ch - f->first];
}

static int text_width(const Font *f, const char *s)
{
    int w = 0;
    for (; *s; s++)
        w += glyph_of(f, (unsigned char)*s)->advance;
    return (w + 8) >> 4;
}

static int round_pen(int pen)
{
    int pixel = pen / 16, rem = pen % 16;
    return rem >= 8 ? pixel + 1 : (rem <= -8 ? pixel - 1 : pixel);
}

/* max_w > 0: the text is cut with ".." so that it fits. */
static void draw_text(const Font *f, int x, int y, const char *s, unsigned c, int max_w)
{
    char cut[160];
    if (max_w > 0 && text_width(f, s) > max_w)
    {
        int n = 0;
        int w = 2 * glyph_of(f, '.')->advance;
        while (s[n] && n < (int)sizeof cut - 4 && (((w + glyph_of(f, (unsigned char)s[n])->advance + 8) >> 4) <= max_w))
        {
            w += glyph_of(f, (unsigned char)s[n])->advance;
            cut[n] = s[n];
            n++;
        }
        cut[n++] = '.';
        cut[n++] = '.';
        cut[n] = 0;
        s = cut;
    }
    int pen = x * 16;
    for (; *s; s++)
    {
        const FontGlyph *g = glyph_of(f, (unsigned char)*s);
        if (g->w)
        {
            const unsigned char *src = f->bitmap + g->offset;
            int gx = round_pen(pen) + g->bx, gy = y + g->by;
            for (int row = 0; row < g->h; row++)
                for (int col = 0; col < g->w; col++)
                    if (src[row * g->w + col])
                        put_px(gx + col, gy + row, c, src[row * g->w + col]);
        }
        pen += g->advance;
    }
}

/* ------------------------------------------------------------ the play list ------------------------------------------------------------ */
typedef struct
{
    char name[96];
    char path[256];
    int is_mp3;
} Track;

static Track *TR;
static int NT;
static char DIR[256];
static int SEL = -1;    /* the song that is playing / paused, or was chosen */
static int SCROLL;      /* first visible row */

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* 1 = mp3, 2 = wav, 0 = something else */
static int kind_of(const char *name)
{
    int n = (int)strlen(name);
    if (n < 5 || name[n - 4] != '.')
        return 0;
    int a = lower(name[n - 3]), b = lower(name[n - 2]), c = lower(name[n - 1]);
    if (a == 'm' && b == 'p' && c == '3')
        return 1;
    if (a == 'w' && b == 'a' && c == 'v')
        return 2;
    return 0;
}

static void join_path(char *out, int cap, const char *dir, const char *name)
{
    int n = (int)strlen(dir);
    if (n > 0 && dir[n - 1] == '/')
        snprintf(out, (usize)cap, "%s%s", dir, name);
    else
        snprintf(out, (usize)cap, "%s/%s", dir, name);
}

static void load_folder(const char *dir)
{
    snprintf(DIR, sizeof DIR, "%s", dir);
    NT = 0;
    DirEntry e;
    for (unsigned i = 0; NT < MAX_TRACKS && sys_readdir(DIR, i, &e) == 1; i++)
    {
        int k = e.is_dir ? 0 : kind_of(e.name);
        if (!k)
            continue;
        snprintf(TR[NT].name, sizeof TR[NT].name, "%s", e.name);
        join_path(TR[NT].path, sizeof TR[NT].path, DIR, e.name);
        TR[NT].is_mp3 = k == 1;
        NT++;
    }
    if (NT == 0 && !strcmp(DIR, "/")) /* "/" lists the drives, which hold no files themselves: show the first drive that has songs */
        for (unsigned i = 0; sys_readdir("/", i, &e) == 1 && i < 16; i++)
        {
            char drive[160];
            join_path(drive, sizeof drive, "/", e.name);
            if (!e.is_dir)
                continue;
            load_folder(drive);
            if (NT > 0)
                return;
        }
}

/* ------------------------------------------------------------ song information ------------------------------------------------------------ */
static char TITLE[96], ARTIST[96], FORMAT[64];
static long long DUR_MS;       /* 0 = not known */

static long long be28(const unsigned char *p) { return ((long long)p[0] << 21) | ((long long)p[1] << 14) | ((long long)p[2] << 7) | p[3]; }

/* A text frame's text as plain ASCII ('?' for what the fonts do not have). enc: 0 Latin-1, 1 UTF-16 with BOM, 2 UTF-16BE, 3 UTF-8. */
static void id3_text(const unsigned char *p, int n, char *out, int cap)
{
    int o = 0;
    if (n < 1)
        return;
    int enc = p[0];
    p++;
    n--;
    if (enc == 1 || enc == 2)
    {
        int le = enc == 1 && n >= 2 && p[0] == 0xFF && p[1] == 0xFE;
        if (enc == 1 && n >= 2)
        {
            p += 2;
            n -= 2;
        }
        for (int i = 0; i + 1 < n && o < cap - 1; i += 2)
        {
            int lo = le || enc == 1 ? p[i] : p[i + 1], hi = le || enc == 1 ? p[i + 1] : p[i];
            if (lo == 0 && hi == 0)
                break;
            out[o++] = (hi == 0 && lo >= 32 && lo < 127) ? (char)lo : '?';
        }
    }
    else
        for (int i = 0; i < n && o < cap - 1 && p[i]; i++)
            out[o++] = (p[i] >= 32 && p[i] < 127) ? (char)p[i] : (p[i] >= 0x80 && enc == 3 && (p[i] & 0xC0) == 0x80 ? 0 : '?');
    /* UTF-8: one '?' per multi-byte character (continuation bytes were dropped above as 0); remove the zeros */
    int w = 0;
    for (int i = 0; i < o; i++)
        if (out[i])
            out[w++] = out[i];
    out[w] = 0;
}

/* Reads the ID3v2 tag at the start of the file: title and artist (TIT2 / TPE1, versions 2.3 and 2.4). Returns the size of the tag
 * (0 = none), so that the audio can be found after it. */
static long long read_id3(long long fd)
{
    unsigned char h[10];
    sys_seek((int)fd, 0, 0);
    if (sys_read((int)fd, h, 10) != 10 || h[0] != 'I' || h[1] != 'D' || h[2] != '3' || ((h[6] | h[7] | h[8] | h[9]) & 0x80))
        return 0;
    long long size = 10 + be28(h + 6) + ((h[5] & 0x10) ? 10 : 0);
    int ver = h[3];
    if (ver == 3 || ver == 4)
    {
        long long limit = size - 10 > 32768 ? 32768 : size - 10;
        unsigned char *buf = malloc((usize)limit + 16);
        if (buf)
        {
            long long got = 0, r;
            while (got < limit && (r = sys_read((int)fd, buf + got, (usize)(limit - got))) > 0)
                got += r;
            for (long long i = 0; i + 10 <= got;)
            {
                const unsigned char *f = buf + i;
                if (!f[0])
                    break; /* padding */
                long long fs = ver == 4 ? be28(f + 4) : ((long long)f[4] << 24 | (long long)f[5] << 16 | (long long)f[6] << 8 | f[7]);
                if (fs <= 0 || i + 10 + fs > got)
                    break;
                if (f[0] == 'T' && f[1] == 'I' && f[2] == 'T' && f[3] == '2')
                    id3_text(f + 10, (int)fs, TITLE, (int)sizeof TITLE);
                else if (f[0] == 'T' && f[1] == 'P' && f[2] == 'E' && f[3] == '1')
                    id3_text(f + 10, (int)fs, ARTIST, (int)sizeof ARTIST);
                i += 10 + fs;
            }
            free(buf);
        }
    }
    return size;
}

static unsigned le32(const unsigned char *p) { return p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16 | (unsigned)p[3] << 24; }

/* Title, artist, length and format of a song. */
static void load_info(const Track *t)
{
    TITLE[0] = ARTIST[0] = FORMAT[0] = 0;
    DUR_MS = 0;
    long long fd = sys_open(t->path);
    if (fd < 0)
        return;
    long long size = sys_seek((int)fd, 0, 2);
    unsigned char *buf = malloc(16384);
    if (t->is_mp3)
    {
        long long tag = read_id3(fd);
        sys_seek((int)fd, tag, 0);
        int have = 0;
        long long r;
        while (buf && have < 16384 && (r = sys_read((int)fd, buf + have, (usize)(16384 - have))) > 0)
            have += (int)r;
        if (buf && have > 8)
        {
            DUR_MS = mp3_estimate_ms(buf, have, size - tag);
            static Mp3Dec dec;
            Mp3Info info;
            mp3_init(&dec);
            if (mp3_decode_frame(&dec, buf, have, 0, &info) > 0)
                snprintf(FORMAT, sizeof FORMAT, "MP3, %d Hz, %s, %d kbps", info.hz, info.channels == 1 ? "mono" : "stereo", info.bitrate_kbps);
        }
    }
    else
    {
        sys_seek((int)fd, 0, 0);
        long long got = buf ? sys_read((int)fd, buf, 4096) : 0;
        unsigned rate = 0, ch = 0, bits = 0, data = 0;
        for (long long i = 12; i + 8 <= got;)
        {
            unsigned cs = le32(buf + i + 4);
            if (!memcmp(buf + i, "fmt ", 4) && i + 24 <= got)
            {
                ch = buf[i + 10] | (unsigned)buf[i + 11] << 8;
                rate = le32(buf + i + 12);
                bits = buf[i + 22] | (unsigned)buf[i + 23] << 8;
            }
            else if (!memcmp(buf + i, "data", 4))
            {
                data = cs;
                break;
            }
            i += 8 + cs + (cs & 1);
        }
        if (rate && ch && bits)
        {
            DUR_MS = (long long)data * 1000 / ((long long)rate * ch * (bits / 8));
            snprintf(FORMAT, sizeof FORMAT, "WAV, %u Hz, %s, %u bit", rate, ch == 1 ? "mono" : "stereo", bits);
        }
    }
    free(buf);
    sys_close((int)fd);
}

/* ------------------------------------------------------------ playing ------------------------------------------------------------ */
enum { ST_STOPPED, ST_PLAYING, ST_PAUSED };
static int STATE = ST_STOPPED;
static long long CHILD;         /* pid of the PLAY.ELF process, 0 = none */
static long long T0, POS0;      /* when it was started (SYS_CLOCK) and the song time it started at, ms */
static long long POS_MS;        /* the song time while paused / stopped */
static int VOLUME = -1;         /* percent, -1 = no sound hardware */

static long long now_pos(void)
{
    if (STATE != ST_PLAYING)
        return POS_MS;
    long long p = POS0 + (sys_clock() - T0);
    return DUR_MS && p > DUR_MS ? DUR_MS : p;
}

/* Ends the player process: the stop request makes its SYS_AUDIO_PLAY return at once; wait for it, then take back a request nobody saw. */
static void halt_child(void)
{
    if (CHILD > 0)
    {
        sys_audio_stop(1);
        int st;
        sys_wait(CHILD, &st);
        sys_audio_stop(0);
        CHILD = 0;
    }
}

static void spawn_child(long long from_ms)
{
    char secs[24];
    snprintf(secs, sizeof secs, "%lld", from_ms / 1000);
    const char *argv[6] = {"PLAY.ELF", TR[SEL].path, 0, 0, 0, 0};
    if (from_ms >= 1000 && TR[SEL].is_mp3) /* only MP3 can start in the middle (play.c) */
    {
        argv[2] = "-s";
        argv[3] = secs;
    }
    CHILD = sys_spawn("PLAY.ELF", argv);
    if (CHILD < 0)
        CHILD = 0;
    T0 = sys_clock();
    POS0 = TR[SEL].is_mp3 ? from_ms / 1000 * 1000 : 0; /* the player starts at whole seconds */
    STATE = CHILD ? ST_PLAYING : ST_STOPPED;
}

static void play_track(int idx, long long from_ms)
{
    halt_child();
    if (idx < 0 || idx >= NT)
    {
        STATE = ST_STOPPED;
        POS_MS = 0;
        return;
    }
    if (idx != SEL)
    {
        SEL = idx;
        load_info(&TR[SEL]);
        if (SEL < SCROLL)
            SCROLL = SEL;
        if (SEL >= SCROLL + VISIBLE_ROWS)
            SCROLL = SEL - VISIBLE_ROWS + 1;
    }
    POS_MS = from_ms;
    spawn_child(from_ms);
}

static void do_pause_resume(void)
{
    if (STATE == ST_PLAYING)
    {
        POS_MS = now_pos();
        halt_child();
        STATE = ST_PAUSED;
    }
    else if (SEL >= 0)
        play_track(SEL, STATE == ST_PAUSED ? POS_MS : 0);
    else if (NT > 0)
        play_track(0, 0);
}

static void do_stop(void)
{
    halt_child();
    STATE = ST_STOPPED;
    POS_MS = 0;
}

static void seek_to(long long ms)
{
    if (SEL < 0)
        return;
    if (ms < 0)
        ms = 0;
    if (DUR_MS && ms >= DUR_MS)
        ms = DUR_MS > 1000 ? DUR_MS - 1000 : 0;
    if (STATE == ST_PLAYING)
        play_track(SEL, ms);
    else
        POS_MS = ms;
}

static void next_track(int step)
{
    if (NT == 0)
        return;
    int n = (SEL < 0 ? (step > 0 ? 0 : NT - 1) : SEL + step + NT) % NT;
    play_track(n, 0);
}

/* ------------------------------------------------------------ the window ------------------------------------------------------------ */
#define PB_X MARGIN
#define PB_Y 92
#define PB_W (WIN_W - 2 * MARGIN)
#define BTN_Y 152
#define VOL_X 408
#define VOL_W 128

static int DRAG_SEEK = -1;      /* x of the progress bar drag, -1 = none */
static int DRAG_VOL;            /* 1 while the volume slider is dragged */
static int HOVER = -1;          /* the list row under the mouse */

static void format_time(char *out, int cap, long long ms)
{
    long long s = ms / 1000;
    if (s >= 3600)
        snprintf(out, (usize)cap, "%d:%02d:%02d", (int)(s / 3600), (int)(s / 60 % 60), (int)(s % 60));
    else
        snprintf(out, (usize)cap, "%d:%02d", (int)(s / 60), (int)(s % 60));
}

static void draw_button(int cx, int cy, int r, int kind, unsigned bg, unsigned fg)
{
    fill_circle(cx, cy, r, bg);
    float s = (float)r;
    switch (kind)
    {
    case 0: /* play */
        fill_tri((float)cx - s * 0.28f, (float)cy - s * 0.42f, (float)cx - s * 0.28f, (float)cy + s * 0.42f, (float)cx + s * 0.46f, (float)cy, fg);
        break;
    case 1: /* pause */
        fill_rect(cx - (int)(s * 0.30f), cy - (int)(s * 0.38f), (int)(s * 0.2f) + 1, (int)(s * 0.76f), fg);
        fill_rect(cx + (int)(s * 0.10f), cy - (int)(s * 0.38f), (int)(s * 0.2f) + 1, (int)(s * 0.76f), fg);
        break;
    case 2: /* stop */
        fill_rect(cx - (int)(s * 0.32f), cy - (int)(s * 0.32f), (int)(s * 0.64f) + 1, (int)(s * 0.64f) + 1, fg);
        break;
    case 3: /* previous */
        fill_rect(cx - (int)(s * 0.40f), cy - (int)(s * 0.36f), 2, (int)(s * 0.72f), fg);
        fill_tri((float)cx + s * 0.40f, (float)cy - s * 0.38f, (float)cx + s * 0.40f, (float)cy + s * 0.38f, (float)cx - s * 0.30f, (float)cy, fg);
        break;
    default: /* next */
        fill_rect(cx + (int)(s * 0.40f) - 1, cy - (int)(s * 0.36f), 2, (int)(s * 0.72f), fg);
        fill_tri((float)cx - s * 0.40f, (float)cy - s * 0.38f, (float)cx - s * 0.40f, (float)cy + s * 0.38f, (float)cx + s * 0.30f, (float)cy, fg);
        break;
    }
}

static void render(void)
{
    fill_rect(0, 0, WIN_W, WIN_H, C_BG);

    /* title and details */
    const char *title = TITLE[0] ? TITLE : (SEL >= 0 ? TR[SEL].name : "Nothing playing");
    draw_text(&font_bold_20, MARGIN, 16, title, C_TEXT, WIN_W - 2 * MARGIN);
    char sub[200];
    if (SEL >= 0)
        snprintf(sub, sizeof sub, "%s%s%s", ARTIST[0] ? ARTIST : "", ARTIST[0] && FORMAT[0] ? "  -  " : "", FORMAT);
    else
        snprintf(sub, sizeof sub, "%d song(s) in %s", NT, DIR);
    draw_text(&font_ui_14, MARGIN, 50, sub, C_DIM, WIN_W - 2 * MARGIN);

    /* progress */
    long long pos = DRAG_SEEK >= 0 && DUR_MS ? (long long)(DRAG_SEEK - PB_X) * DUR_MS / PB_W : now_pos();
    if (pos < 0)
        pos = 0;
    fill_round(PB_X, PB_Y, PB_W, 6, 3, C_PANEL);
    int filled = DUR_MS ? (int)(pos * PB_W / DUR_MS) : 0;
    if (filled > PB_W)
        filled = PB_W;
    if (filled > 0)
        fill_round(PB_X, PB_Y, filled < 6 ? 6 : filled, 6, 3, C_ACCENT);
    if (SEL >= 0 && DUR_MS)
        fill_circle(PB_X + filled, PB_Y + 3, 6, C_ACCENT_HI);
    char t1[24], t2[24];
    format_time(t1, sizeof t1, pos);
    if (DUR_MS)
        format_time(t2, sizeof t2, DUR_MS);
    else
        snprintf(t2, sizeof t2, "--:--");
    draw_text(&font_ui_12, PB_X, PB_Y + 14, t1, C_DIM, 0);
    draw_text(&font_ui_12, PB_X + PB_W - text_width(&font_ui_12, t2), PB_Y + 14, t2, C_DIM, 0);

    /* buttons */
    draw_button(150, BTN_Y, 17, 2, C_PANEL, C_TEXT);
    draw_button(214, BTN_Y, 19, 3, C_PANEL, C_TEXT);
    draw_button(280, BTN_Y, 28, STATE == ST_PLAYING ? 1 : 0, C_ACCENT, 0xFFFFFF);
    draw_button(346, BTN_Y, 19, 4, C_PANEL, C_TEXT);

    /* volume */
    if (VOLUME >= 0)
    {
        fill_round(VOL_X, BTN_Y - 2, VOL_W, 4, 2, C_PANEL);
        int vf = VOL_W * VOLUME / 100;
        if (vf > 0)
            fill_round(VOL_X, BTN_Y - 2, vf < 4 ? 4 : vf, 4, 2, C_ACCENT);
        fill_circle(VOL_X + vf, BTN_Y, 6, C_ACCENT_HI);
        char vt[16];
        snprintf(vt, sizeof vt, "%d%%", VOLUME);
        draw_text(&font_ui_12, VOL_X, BTN_Y + 12, "Volume", C_DIM, 0);
        draw_text(&font_ui_12, VOL_X + VOL_W - text_width(&font_ui_12, vt), BTN_Y + 12, vt, C_DIM, 0);
    }
    else
        draw_text(&font_ui_12, VOL_X, BTN_Y - 8, "No sound device", C_DIM, 0);

    /* play list */
    char head[160];
    snprintf(head, sizeof head, "Play list - %s", DIR);
    draw_text(&font_med_14, MARGIN, 192, head, C_TEXT, WIN_W - 2 * MARGIN);
    fill_rect(MARGIN, 216, WIN_W - 2 * MARGIN, 1, C_BORDER);
    if (NT == 0)
        draw_text(&font_ui_14, MARGIN, LIST_Y + 8, "No MP3 or WAV files in this folder.", C_DIM, 0);
    for (int i = 0; i < VISIBLE_ROWS && SCROLL + i < NT; i++)
    {
        int idx = SCROLL + i, y = LIST_Y + i * ROW_H;
        if (idx == SEL)
            fill_round(MARGIN - 6, y, WIN_W - 2 * MARGIN + 12, ROW_H - 2, 6, C_PANEL);
        else if (idx == HOVER)
            fill_round(MARGIN - 6, y, WIN_W - 2 * MARGIN + 12, ROW_H - 2, 6, 0x222A40);
        if (idx == SEL && STATE == ST_PLAYING)
            fill_tri((float)MARGIN, (float)y + 6, (float)MARGIN, (float)y + 18, (float)MARGIN + 9, (float)y + 12, C_ACCENT_HI);
        char num[12];
        snprintf(num, sizeof num, "%d", idx + 1);
        draw_text(&font_ui_12, MARGIN + 18, y + 5, num, C_DIM, 0);
        draw_text(&font_ui_14, MARGIN + 48, y + 4, TR[idx].name, idx == SEL ? C_TEXT : 0xC8D0E4, WIN_W - MARGIN - 48 - MARGIN - 8);
    }
    if (NT > VISIBLE_ROWS) /* a thin scroll bar */
    {
        int track_h = VISIBLE_ROWS * ROW_H, th = track_h * VISIBLE_ROWS / NT;
        if (th < 16)
            th = 16;
        int ty = LIST_Y + (track_h - th) * SCROLL / (NT - VISIBLE_ROWS);
        fill_round(WIN_W - 12, ty, 4, th, 2, C_BORDER);
    }
}

static int in_circle(int x, int y, int cx, int cy, int r) { return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r; }

static void set_volume_from_x(int x)
{
    int v = (x - VOL_X) * 100 / VOL_W;
    VOLUME = (int)sys_audio_volume(v < 0 ? 0 : v > 100 ? 100 : v);
}

static void on_mouse_down(int x, int y)
{
    if (in_circle(x, y, 280, BTN_Y, 28))
        do_pause_resume();
    else if (in_circle(x, y, 150, BTN_Y, 17))
        do_stop();
    else if (in_circle(x, y, 214, BTN_Y, 19))
        next_track(-1);
    else if (in_circle(x, y, 346, BTN_Y, 19))
        next_track(1);
    else if (y >= PB_Y - 8 && y <= PB_Y + 14 && x >= PB_X - 4 && x <= PB_X + PB_W + 4)
        DRAG_SEEK = x < PB_X ? PB_X : (x > PB_X + PB_W ? PB_X + PB_W : x);
    else if (VOLUME >= 0 && y >= BTN_Y - 12 && y <= BTN_Y + 12 && x >= VOL_X - 8 && x <= VOL_X + VOL_W + 8)
    {
        DRAG_VOL = 1;
        set_volume_from_x(x);
    }
    else if (y >= LIST_Y && x >= MARGIN - 6 && x <= WIN_W - MARGIN + 6)
    {
        int idx = SCROLL + (y - LIST_Y) / ROW_H;
        if (idx < NT && (y - LIST_Y) / ROW_H < VISIBLE_ROWS)
            play_track(idx, 0);
    }
}

static void on_key(int key)
{
    switch (key)
    {
    case ' ': do_pause_resume(); break;
    case KEY_ENTER: if (SEL >= 0 || NT) play_track(SEL < 0 ? 0 : SEL, 0); break;
    case KEY_LEFT: seek_to(now_pos() - 5000); break;
    case KEY_RIGHT: seek_to(now_pos() + 5000); break;
    case KEY_UP: case 'p': next_track(-1); break;
    case KEY_DOWN: case 'n': next_track(1); break;
    case 's': do_stop(); break;
    case '+': case '=': if (VOLUME >= 0) VOLUME = (int)sys_audio_volume(VOLUME + 5); break;
    case '-': if (VOLUME >= 0) VOLUME = (int)sys_audio_volume(VOLUME - 5); break;
    default: break;
    }
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(long long argc, char **argv)
{
    TR = malloc(sizeof(Track) * MAX_TRACKS);
    if (!TR)
        sys_exit(1);

    /* what to show: the given file's folder (and that file selected), the given folder, or the working folder */
    char start_file[256];
    start_file[0] = 0;
    char dir[256];
    if (argc >= 2)
    {
        DirEntry e;
        snprintf(dir, sizeof dir, "%s", argv[1]);
        if (sys_stat(argv[1], &e) == 0 && !e.is_dir)
        {
            snprintf(start_file, sizeof start_file, "%s", argv[1]);
            int cut = -1;
            for (int i = 0; dir[i]; i++)
                if (dir[i] == '/')
                    cut = i;
            if (cut > 0)
                dir[cut] = 0;
            else if (cut == 0)
                dir[1] = 0;
            else if (sys_getcwd(dir, sizeof dir) < 0)
                snprintf(dir, sizeof dir, "/");
            if (cut < 0) /* a bare name: the file is in the working folder */
            {
                char full[256];
                join_path(full, sizeof full, dir, argv[1]);
                snprintf(start_file, sizeof start_file, "%s", full);
            }
        }
    }
    else if (sys_getcwd(dir, sizeof dir) < 0)
        snprintf(dir, sizeof dir, "/");
    load_folder(dir);
    for (int i = 0; i < NT && start_file[0]; i++)
        if (!strcmp(TR[i].path, start_file))
            SEL = i;

    GuiInfo gi;
    memset(&gi, 0, sizeof gi);
    snprintf(gi.title, sizeof gi.title, "Player");
    gi.width = WIN_W;
    gi.height = WIN_H;
    if (sys_gui_create(&gi) != 0)
    {
        fdprintf(2, "player: cannot open a window\n");
        sys_exit(1);
    }
    PIX = (unsigned *)gi.pixels;
    int h = gi.handle;
    VOLUME = (int)sys_audio_volume(-1);

    int first = SEL;
    if (first >= 0)
    {
        SEL = -1; /* play_track loads the info of a song that is not the selected one yet */
        play_track(first, 0);
    }
    render();
    sys_gui_present(h, 0, 0, 0, 0);

    long long last_draw = 0;
    int dirty = 0, done = 0;
    while (!done)
    {
        GuiEvent ev;
        long long r;
        while ((r = sys_gui_event(h, &ev, 1)) == 1)
        {
            dirty = 1;
            switch (ev.type)
            {
            case GUI_EV_MOUSE_DOWN:
                if (ev.buttons & 1)
                    on_mouse_down(ev.x, ev.y);
                break;
            case GUI_EV_MOUSE_MOVE:
                if (DRAG_SEEK >= 0)
                    DRAG_SEEK = ev.x < PB_X ? PB_X : (ev.x > PB_X + PB_W ? PB_X + PB_W : ev.x);
                else if (DRAG_VOL)
                    set_volume_from_x(ev.x);
                else
                {
                    int row = ev.y >= LIST_Y ? (ev.y - LIST_Y) / ROW_H : -1;
                    HOVER = (row >= 0 && row < VISIBLE_ROWS && SCROLL + row < NT) ? SCROLL + row : -1;
                }
                break;
            case GUI_EV_MOUSE_UP:
                if (DRAG_SEEK >= 0 && DUR_MS)
                {
                    long long target = (long long)(DRAG_SEEK - PB_X) * DUR_MS / PB_W;
                    DRAG_SEEK = -1;
                    seek_to(target);
                }
                DRAG_SEEK = -1;
                DRAG_VOL = 0;
                break;
            case GUI_EV_WHEEL:
                if (NT > VISIBLE_ROWS)
                {
                    SCROLL += ev.x > 0 ? 2 : -2;
                    if (SCROLL < 0)
                        SCROLL = 0;
                    if (SCROLL > NT - VISIBLE_ROWS)
                        SCROLL = NT - VISIBLE_ROWS;
                }
                break;
            case GUI_EV_KEY:
                if (ev.pressed)
                {
                    if (ev.key == KEY_ESC)
                        done = 1;
                    else
                        on_key(ev.key);
                }
                break;
            case GUI_EV_CLOSE:
                done = 1;
                break;
            default:
                break;
            }
        }
        if (r < 0) /* the window is gone */
            break;
        if (done)
            break;

        /* the player process ended on its own: the song is over, go on with the next one */
        int st = 0;
        if (STATE == ST_PLAYING && CHILD > 0 && sys_wait_nohang(CHILD, &st) > 0)
        {
            CHILD = 0;
            if (st == 0 && SEL + 1 < NT)
                play_track(SEL + 1, 0);
            else
            {
                STATE = ST_STOPPED;
                POS_MS = 0;
            }
            dirty = 1;
        }

        long long t = sys_clock();
        if (dirty || (STATE == ST_PLAYING && t - last_draw >= 250))
        {
            render();
            sys_gui_present(h, 0, 0, 0, 0);
            last_draw = t;
            dirty = 0;
        }
        sys_sleep(30);
    }

    halt_child();
    sys_gui_destroy(h);
    sys_exit(0);
}
