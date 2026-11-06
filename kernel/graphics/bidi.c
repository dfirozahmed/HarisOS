/* HarisOS bidirectional text: see include/bidi.h. The rule names (W1, N1 ...) are those of UAX #9. */
#include "bidi.h"
#include "uni_data.h"

#define MAXN 256

int bidi_class(unsigned cp)
{
    if (cp > 0xFFFF)
        return BIDI_L;
    int lo = 0, hi = (int)uni_bidi_n - 1;
    while (lo < hi) /* the last range that starts at or before cp */
    {
        int mid = (lo + hi + 1) / 2;
        if (uni_bidi_start[mid] <= cp)
            lo = mid;
        else
            hi = mid - 1;
    }
    return uni_bidi_class[lo];
}

int bidi_is_rtl(unsigned cp)
{
    if (cp < 0x0590) /* nothing below Hebrew is right to left: the quick way out for ASCII and Latin */
        return 0;
    int k = bidi_class(cp);
    return k == BIDI_R || k == BIDI_AL;
}

unsigned bidi_mirror(unsigned cp)
{
    int lo = 0, hi = (int)uni_mirror_n - 1;
    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        unsigned v = uni_mirror_from[mid];
        if (v == cp)
            return uni_mirror_to[mid];
        if (v < cp)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return cp;
}

static int is_ni(int t) { return t == BIDI_B || t == BIDI_S || t == BIDI_WS || t == BIDI_ON; } /* neutral or isolate */
static int strong_dir(int t) /* 0 = left to right, 1 = right to left (numbers count as right to left), -1 = not strong */
{
    if (t == BIDI_L)
        return 0;
    if (t == BIDI_R || t == BIDI_AL || t == BIDI_EN || t == BIDI_AN)
        return 1;
    return -1;
}

void bidi_resolve(const unsigned *cp, int n, int base, unsigned char *level, int *para_level)
{
    unsigned char t[MAXN];
    if (n > MAXN)
        n = MAXN;
    for (int i = 0; i < n; i++)
        t[i] = (unsigned char)bidi_class(cp[i]);

    /* P2, P3: the paragraph direction is that of the first strong letter */
    int para = base;
    if (para < 0)
    {
        para = 0;
        for (int i = 0; i < n; i++)
            if (t[i] == BIDI_L || t[i] == BIDI_R || t[i] == BIDI_AL)
            {
                para = t[i] == BIDI_L ? 0 : 1;
                break;
            }
    }
    int sos = para ? BIDI_R : BIDI_L; /* the start and end of the paragraph count as a strong letter of its direction */

    /* W1: a mark takes the class of the character before it; a joiner (BN) is passed over the same way */
    for (int i = 0; i < n; i++)
        if (t[i] == BIDI_NSM || t[i] == BIDI_BN)
            t[i] = (unsigned char)(i ? t[i - 1] : sos);
    /* W2: a European digit after an Arabic letter is an Arabic digit */
    for (int i = 0, last = sos; i < n; i++)
    {
        if (t[i] == BIDI_L || t[i] == BIDI_R || t[i] == BIDI_AL)
            last = t[i];
        else if (t[i] == BIDI_EN && last == BIDI_AL)
            t[i] = BIDI_AN;
    }
    /* W3: Arabic letters are right to left letters from here on */
    for (int i = 0; i < n; i++)
        if (t[i] == BIDI_AL)
            t[i] = BIDI_R;
    /* W4: a single separator between two numbers of one kind joins them */
    for (int i = 1; i + 1 < n; i++)
    {
        if (t[i] == BIDI_ES && t[i - 1] == BIDI_EN && t[i + 1] == BIDI_EN)
            t[i] = BIDI_EN;
        else if (t[i] == BIDI_CS && t[i - 1] == t[i + 1] && (t[i - 1] == BIDI_EN || t[i - 1] == BIDI_AN))
            t[i] = t[i - 1];
    }
    /* W5: terminators ($ %) next to a European digit are part of the number */
    for (int i = 0; i < n; i++)
        if (t[i] == BIDI_ET)
        {
            int e = i;
            while (e < n && t[e] == BIDI_ET)
                e++;
            if ((i > 0 && t[i - 1] == BIDI_EN) || (e < n && t[e] == BIDI_EN))
                for (int k = i; k < e; k++)
                    t[k] = BIDI_EN;
            i = e - 1;
        }
    /* W6: the separators and terminators left are neutral */
    for (int i = 0; i < n; i++)
        if (t[i] == BIDI_ES || t[i] == BIDI_ET || t[i] == BIDI_CS)
            t[i] = BIDI_ON;
    /* W7: a European digit after a left to right letter is left to right */
    for (int i = 0, last = sos; i < n; i++)
    {
        if (t[i] == BIDI_L || t[i] == BIDI_R)
            last = t[i];
        else if (t[i] == BIDI_EN && last == BIDI_L)
            t[i] = BIDI_L;
    }
    /* N1, N2: a run of neutrals between two letters (or numbers) of one direction takes it; otherwise the paragraph's */
    for (int i = 0; i < n; i++)
        if (is_ni(t[i]))
        {
            int e = i;
            while (e < n && is_ni(t[e]))
                e++;
            int before = i > 0 ? strong_dir(t[i - 1]) : para ? 1 : 0;
            int after = e < n ? strong_dir(t[e]) : para ? 1 : 0;
            int dir = before == after && before >= 0 ? before : para;
            for (int k = i; k < e; k++)
                t[k] = dir ? BIDI_R : BIDI_L;
            i = e - 1;
        }
    /* I1, I2: the levels */
    for (int i = 0; i < n; i++)
    {
        int lv = para;
        if (para == 0)
            lv += t[i] == BIDI_R ? 1 : (t[i] == BIDI_EN || t[i] == BIDI_AN) ? 2 : 0;
        else
            lv += (t[i] == BIDI_L || t[i] == BIDI_EN || t[i] == BIDI_AN) ? 1 : 0;
        level[i] = (unsigned char)lv;
    }
    /* L1: spaces at the end of the line stand at the paragraph's level */
    for (int i = n - 1; i >= 0 && bidi_class(cp[i]) == BIDI_WS; i--)
        level[i] = (unsigned char)para;
    if (para_level)
        *para_level = para;
}

void bidi_reorder(const unsigned char *level, int n, unsigned short *vis)
{
    int hi = 0, lo_odd = 255;
    if (n > MAXN)
        n = MAXN;
    for (int i = 0; i < n; i++)
    {
        vis[i] = (unsigned short)i;
        if (level[i] > hi)
            hi = level[i];
        if ((level[i] & 1) && level[i] < lo_odd)
            lo_odd = level[i];
    }
    /* L2: from the highest level down to the lowest odd one, reverse every run of items at that level or higher */
    for (int lv = hi; lv >= lo_odd && lv > 0; lv--)
        for (int i = 0; i < n;)
        {
            if (level[vis[i]] < lv)
            {
                i++;
                continue;
            }
            int e = i;
            while (e < n && level[vis[e]] >= lv)
                e++;
            for (int a = i, b = e - 1; a < b; a++, b--)
            {
                unsigned short x = vis[a];
                vis[a] = vis[b];
                vis[b] = x;
            }
            i = e;
        }
}
