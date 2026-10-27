/* FAT32: names (short 8.3 and long names). */
#include "fat32_internal.h"

/* ---- names ------------------------------------------------------ */
u8 f32_sfn_checksum(const u8 *n)
{
    u8 sum = 0;
    for (int i = 0; i < 11; i++)
        sum = (u8)(((sum & 1) << 7) + (sum >> 1) + n[i]);
    return sum;
}

int f32_name_valid(const char *name)
{
    unsigned n = slen(name);
    if (n == 0 || n > 255 || name[0] == ' ' || name[n - 1] == ' ' || name[n - 1] == '.')
        return 0;
    for (unsigned i = 0; i < n; i++)
    {
        u8 c = (u8)name[i];
        if (c < 0x20 || c == 0x7F || c == '"' || c == '*' || c == '/' || c == ':' || c == '<' || c == '>' || c == '?' ||
            c == '\\' || c == '|')
            return 0;
    }
    return 1;
}

/* characters allowed in a short name besides letters and digits */
static int sfn_char_ok(u8 c) { return c > 0x20 && c < 0x7F && !(c == '+' || c == ',' || c == ';' || c == '=' || c == '[' || c == ']' || c == '.'); }

/* Short name for `name`. Returns 1 if the name needs long-name entries as well (it does not fit 8.3 exactly,
 * has mixed case in a part, or characters a short name cannot hold). `tail` > 0 adds "~tail". */
int f32_make_sfn(const char *name, u8 sfn[11], u8 *ntres, unsigned tail)
{
    unsigned n = slen(name), dot = n;
    for (unsigned i = n; i > 0; i--)
        if (name[i - 1] == '.')
        {
            dot = i - 1;
            break;
        }
    unsigned bstart = 0;
    int lossy = 0;
    if (dot == 0) /* ".name": the short name drops the dot */
    {
        bstart = 1;
        dot = n;
        lossy = 1;
        for (unsigned i = n; i > 1; i--)
            if (name[i - 1] == '.')
            {
                dot = i - 1;
                break;
            }
        if (dot < 1)
            dot = n;
    }
    for (int i = 0; i < 11; i++)
        sfn[i] = ' ';
    int bl = 0, bu = 0, el = 0, eu = 0;
    unsigned bn = 0, en = 0;
    for (unsigned i = bstart; i < dot; i++)
    {
        u8 c = (u8)name[i];
        if (c == '.' || c == ' ')
        {
            lossy = 1; /* extra dots and spaces are dropped */
            continue;
        }
        if (!sfn_char_ok(c))
        {
            c = '_';
            lossy = 1;
        }
        if (c >= 'a' && c <= 'z')
            bl = 1;
        if (c >= 'A' && c <= 'Z')
            bu = 1;
        if (bn < 8)
            sfn[bn++] = (u8)upper(c);
        else
            lossy = 1;
    }
    for (unsigned i = dot + 1; dot < n && i < n; i++)
    {
        u8 c = (u8)name[i];
        if (c == ' ')
        {
            lossy = 1;
            continue;
        }
        if (!sfn_char_ok(c))
        {
            c = '_';
            lossy = 1;
        }
        if (c >= 'a' && c <= 'z')
            el = 1;
        if (c >= 'A' && c <= 'Z')
            eu = 1;
        if (en < 3)
            sfn[8 + en++] = (u8)upper(c);
        else
            lossy = 1;
    }
    if (bn == 0)
    {
        sfn[0] = '_';
        bn = 1;
        lossy = 1;
    }
    int need_lfn = lossy || (bl && bu) || (el && eu);
    *ntres = 0;
    if (!need_lfn)
    {
        if (bl)
            *ntres |= 0x08;
        if (el)
            *ntres |= 0x10;
        if (sfn[0] == 0xE5)
            sfn[0] = 0x05;
        return 0;
    }
    if (tail)
    {
        char t[8];
        int tn = 0;
        t[tn++] = '~';
        char digits[8];
        int dn = 0;
        for (unsigned v = tail; v; v /= 10)
            digits[dn++] = (char)('0' + v % 10);
        while (dn)
            t[tn++] = digits[--dn];
        unsigned keep = 8 - (unsigned)tn;
        if (bn < keep)
            keep = bn;
        for (int i = 0; i < tn; i++)
            sfn[keep + i] = (u8)t[i];
        for (unsigned i = keep + (unsigned)tn; i < 8; i++)
            sfn[i] = ' ';
    }
    if (sfn[0] == 0xE5)
        sfn[0] = 0x05;
    return 1;
}

/* the name of a short entry: "NAME.EXT" (lower-case parts as the NT flags say) */
void f32_sfn_to_name(const u8 *e, char *out)
{
    int n = 0, lowb = (e[12] & 0x08) != 0, lowe = (e[12] & 0x10) != 0;
    for (int i = 0; i < 8 && e[i] != ' '; i++)
    {
        int c = (i == 0 && e[0] == 0x05) ? 0xE5 : e[i];
        out[n++] = (char)(c >= 0x20 && c < 0x7F ? (lowb ? lower(c) : c) : '_');
    }
    if (e[8] != ' ')
    {
        out[n++] = '.';
        for (int i = 8; i < 11 && e[i] != ' '; i++)
            out[n++] = (char)(e[i] >= 0x20 && e[i] < 0x7F ? (lowe ? lower(e[i]) : e[i]) : '_');
    }
    out[n] = 0;
}
