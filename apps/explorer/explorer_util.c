/* File Explorer: file kinds, size text and small string helpers. */
#include "explorer.h"

int ex_lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int ext_is(const char *name, const char *ext)
{
    const char *dot = 0;
    for (const char *p = name; *p; p++)
        if (*p == '.')
            dot = p + 1;
    if (!dot)
        return 0;
    while (*dot && *ext && ex_lower((unsigned char)*dot) == *ext)
        dot++, ext++;
    return !*dot && !*ext;
}

int ex_kind_of(const VfsNode *n)
{
    static const char *const TEXT[] = {"txt", "md", "ini", "log", 0};
    static const char *const CODE[] = {"c", "h", "asm", 0};
    static const char *const IMAGE[] = {"bmp", "png", "jpg", 0};
    static const char *const AUDIO[] = {"wav", "mp3", 0};
    if (n->is_dir)
        return FK_FOLDER;
    for (int i = 0; TEXT[i]; i++)
        if (ext_is(n->name, TEXT[i]))
            return FK_TEXT;
    for (int i = 0; CODE[i]; i++)
        if (ext_is(n->name, CODE[i]))
            return FK_CODE;
    for (int i = 0; IMAGE[i]; i++)
        if (ext_is(n->name, IMAGE[i]))
            return FK_IMAGE;
    for (int i = 0; AUDIO[i]; i++)
        if (ext_is(n->name, AUDIO[i]))
            return FK_AUDIO;
    return ext_is(n->name, "efi") ? FK_EFI : FK_OTHER;
}

const char *ex_kind_name(int k)
{
    static const char *const NAMES[] = {"Folder", "Text file", "Source code", "Image", "Audio", "EFI program", "File", "Drive"};
    return NAMES[k];
}

UiRgb ex_kind_color(int k)
{
    switch (k)
    {
    case FK_TEXT:
        return ui_rgb(110, 150, 230);
    case FK_CODE:
        return ui_rgb(80, 200, 150);
    case FK_IMAGE:
        return ui_rgb(240, 130, 170);
    case FK_AUDIO:
        return ui_rgb(170, 120, 240);
    case FK_EFI:
        return ui_rgb(250, 170, 60);
    default:
        return ui_rgb(150, 160, 185);
    }
}

/* "812 B", "1.2 KB", "3.0 GB", "1.8 TB": binary units like Windows shows them, one decimal rounded down */
void ex_fmt_size_big(unsigned long long bytes, char *out)
{
    static const char *const UNIT[] = {" B", " KB", " MB", " GB", " TB"};
    int u = 0;
    unsigned long long scaled = bytes, rest = 0;
    while (scaled >= 1024 && u < 4)
    {
        rest = scaled % 1024;
        scaled /= 1024;
        u++;
    }
    int o = ex_put_uint(out, 0, (int)scaled);
    if (u > 0)
    {
        out[o++] = '.';
        out[o++] = (char)('0' + rest * 10 / 1024);
    }
    o = ex_put_str(out, o, 24, UNIT[u]);
    out[o] = 0;
}

/* "812 B", "1.2 KB", "3.0 MB" (one decimal, rounded down) */
void ex_fmt_size(unsigned bytes, char *out)
{
    unsigned whole, tenth;
    char unit;
    if (bytes < 1024)
    {
        whole = bytes;
        tenth = 0;
        unit = 'B';
    }
    else if (bytes < 1024u * 1024u)
    {
        whole = bytes / 1024;
        tenth = (bytes % 1024) * 10 / 1024;
        unit = 'K';
    }
    else
    {
        whole = bytes / (1024u * 1024u);
        tenth = (bytes % (1024u * 1024u)) * 10 / (1024u * 1024u);
        unit = 'M';
    }
    char digits[12];
    int n = 0, o = 0;
    do
        digits[n++] = (char)('0' + whole % 10), whole /= 10;
    while (whole);
    while (n)
        out[o++] = digits[--n];
    if (unit != 'B')
    {
        out[o++] = '.';
        out[o++] = (char)('0' + tenth);
        out[o++] = ' ';
        out[o++] = unit;
        out[o++] = 'B';
    }
    else
    {
        out[o++] = ' ';
        out[o++] = 'B';
    }
    out[o] = 0;
}

int ex_put_uint(char *out, int o, int v)
{
    char digits[12];
    int n = 0;
    do
        digits[n++] = (char)('0' + v % 10), v /= 10;
    while (v);
    while (n)
        out[o++] = digits[--n];
    return o;
}

int ex_put_str(char *out, int o, int cap, const char *s)
{
    while (*s && o < cap - 1)
        out[o++] = *s++;
    return o;
}

int ex_same_text(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

