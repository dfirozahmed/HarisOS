/* HarisOS user C library, number conversion (see libc.h): strtod / atof for decimal and hexadecimal text, and the exact digit generator
 * behind printf's %e %f %g. Kept in a file of its own, with no system calls, so that tests/ can compile it on the host. */
#include "libc.h"
#include <math.h>

/* ---- strtod -------------------------------------------------------------------------------------------------------------------- */
static const double pow10_exact[23] = {1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
                                       1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};

static int hex_value(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int match_word(const char *p, const char *word) /* case-insensitive prefix test: the length matched, or 0 */
{
    int i = 0;
    for (; word[i]; i++)
        if (tolower((unsigned char)p[i]) != word[i])
            return 0;
    return i;
}

/* m * 10^e10 for m < 2^63. The common case (m < 2^53, |e10| <= 22) is exact in double arithmetic and rounds once. Otherwise the scaling
 * is done in the x87 unit, which has 64 mantissa bits: 10^|e10| by repeated squaring, then one multiply or divide, then the single
 * rounding to double. That is right except for the rare input within about 2^-11 ulp of a rounding boundary. */
static double scale10(unsigned long long m, int e10)
{
    if (m == 0)
        return 0.0;
    if (m < 9007199254740992ULL && e10 >= -22 && e10 <= 22)
        return e10 >= 0 ? (double)m * pow10_exact[e10] : (double)m / pow10_exact[-e10];
    if (e10 > 4000)
        return HUGE_VAL;
    if (e10 < -4000)
        return 0.0;
    long long sm = (long long)m;
    unsigned n = (unsigned)(e10 < 0 ? -e10 : e10);
    unsigned short cw_old, cw_ext;
    double ten = 10.0, r;
    __asm__ volatile("fnstcw %0" : "=m"(cw_old));
    cw_ext = (unsigned short)(cw_old | 0x300); /* all 64 bits, whatever the caller left in the control word */
    __asm__ volatile("fldcw %0" : : "m"(cw_ext));
    __asm__ volatile(
        "fld1\n\t"      /* pw = 1 */
        "fldl %[ten]\n\t" /* acc = 10, pw */
        "1:\n\t"
        "testl $1, %[n]\n\t"
        "jz 2f\n\t"
        "fmul %%st(0), %%st(1)\n\t" /* pw *= acc */
        "2:\n\t"
        "shrl $1, %[n]\n\t"
        "jz 3f\n\t"
        "fmul %%st(0), %%st(0)\n\t" /* acc *= acc */
        "jmp 1b\n\t"
        "3:\n\t"
        "fstp %%st(0)\n\t"  /* drop acc: pw is on top */
        "fildll %[m]\n\t"   /* m, pw */
        : [n] "+r"(n)
        : [m] "m"(sm), [ten] "m"(ten)
        : "st", "st(1)", "cc", "memory");
    if (e10 < 0)
        __asm__ volatile("fdiv %%st(1), %%st\n\tfstpl %0\n\tfstp %%st(0)" : "=m"(r) : : "st", "st(1)", "memory");
    else
        __asm__ volatile("fmul %%st(1), %%st\n\tfstpl %0\n\tfstp %%st(0)" : "=m"(r) : : "st", "st(1)", "memory");
    __asm__ volatile("fldcw %0" : : "m"(cw_old));
    return r;
}

double strtod(const char *s, char **end)
{
    const char *p = s;
    double result = 0;
    int neg = 0;
    while (isspace((unsigned char)*p))
        p++;
    if (*p == '-' || *p == '+')
        neg = *p++ == '-';

    int w;
    if ((w = match_word(p, "infinity")) || (w = match_word(p, "inf")))
    {
        p += w;
        result = HUGE_VAL;
    }
    else if ((w = match_word(p, "nan")))
    {
        p += w;
        if (*p == '(') /* nan(chars) */
        {
            const char *q = p + 1;
            while (isalnum((unsigned char)*q) || *q == '_')
                q++;
            if (*q == ')')
                p = q + 1;
        }
        result = __builtin_nan("");
    }
    else if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && (hex_value(p[2]) >= 0 || (p[2] == '.' && hex_value(p[3]) >= 0)))
    {
        p += 2;
        unsigned long long m = 0;
        int e2 = 0, ndig = 0;
        for (; hex_value(*p) >= 0; p++)
        {
            if (ndig < 15) /* 60 bits of mantissa is more than a double holds */
            {
                m = m << 4 | (unsigned)hex_value(*p);
                if (m)
                    ndig++;
            }
            else
                e2 += 4;
        }
        if (*p == '.')
            for (p++; hex_value(*p) >= 0; p++)
                if (ndig < 15)
                {
                    m = m << 4 | (unsigned)hex_value(*p);
                    if (m)
                        ndig++;
                    e2 -= 4;
                }
        if (*p == 'p' || *p == 'P')
        {
            const char *q = p + 1;
            int eneg = 0, ev = 0;
            if (*q == '-' || *q == '+')
                eneg = *q++ == '-';
            if (isdigit((unsigned char)*q))
            {
                for (; isdigit((unsigned char)*q); q++)
                    if (ev < 100000)
                        ev = ev * 10 + (*q - '0');
                e2 += eneg ? -ev : ev;
                p = q;
            }
        }
        result = ldexp((double)m, e2);
    }
    else
    {
        unsigned long long m = 0;
        int e10 = 0, ndig = 0, any = 0;
        for (; isdigit((unsigned char)*p); p++)
        {
            any = 1;
            if (ndig < 18)
            {
                m = m * 10 + (unsigned)(*p - '0');
                if (m)
                    ndig++;
            }
            else
                e10++; /* digits beyond what m holds: only their place value counts */
        }
        if (*p == '.')
        {
            const char *q = p + 1;
            if (isdigit((unsigned char)*q) || any)
            {
                any |= isdigit((unsigned char)*q);
                for (p = q; isdigit((unsigned char)*p); p++)
                    if (ndig < 18)
                    {
                        m = m * 10 + (unsigned)(*p - '0');
                        if (m)
                            ndig++;
                        e10--;
                    }
            }
        }
        if (!any)
        {
            if (end)
                *end = (char *)s; /* no number at all */
            return 0.0;
        }
        if (*p == 'e' || *p == 'E')
        {
            const char *q = p + 1;
            int eneg = 0, ev = 0;
            if (*q == '-' || *q == '+')
                eneg = *q++ == '-';
            if (isdigit((unsigned char)*q))
            {
                for (; isdigit((unsigned char)*q); q++)
                    if (ev < 100000)
                        ev = ev * 10 + (*q - '0');
                e10 += eneg ? -ev : ev;
                p = q;
            }
        }
        result = scale10(m, e10);
        if (__isinf(result))
            errno = ERANGE;
    }
    if (end)
        *end = (char *)p;
    return neg ? -result : result;
}

double atof(const char *s) { return strtod(s, 0); }

/* ---- the digits behind %e %f %g ------------------------------------------------------------------------------------------------ */
/* The exact decimal expansion of a double is finite (at most 309 digits before the point and 1074 after it), so printf can round
 * correctly by looking at the real digits instead of approximating. The expansion is built with a small big-number in base 10^9. */
#define LIMB_BASE 1000000000u
#define MAX_LIMBS 128
#define DIGITS_MAX (1 + 310 + 1075 + 8)

typedef struct
{
    unsigned limb[MAX_LIMBS]; /* little endian, base 10^9 */
    int n;
} Big;

static void big_set(Big *b, unsigned long long v)
{
    b->n = 0;
    while (v)
    {
        b->limb[b->n++] = (unsigned)(v % LIMB_BASE);
        v /= LIMB_BASE;
    }
}

static void big_mul(Big *b, unsigned m) /* m < 2^32 */
{
    unsigned long long carry = 0;
    for (int i = 0; i < b->n; i++)
    {
        unsigned long long t = (unsigned long long)b->limb[i] * m + carry;
        b->limb[i] = (unsigned)(t % LIMB_BASE);
        carry = t / LIMB_BASE;
    }
    while (carry && b->n < MAX_LIMBS)
    {
        b->limb[b->n++] = (unsigned)(carry % LIMB_BASE);
        carry /= LIMB_BASE;
    }
}

/* the decimal digits of b into out (no leading zeros, "0" for zero); returns how many */
static int big_text(const Big *b, char *out)
{
    int len = 0;
    if (b->n == 0)
    {
        out[0] = '0';
        return 1;
    }
    char tmp[10];
    for (int i = b->n - 1; i >= 0; i--)
    {
        unsigned v = b->limb[i];
        int t = 0;
        do
            tmp[t++] = (char)('0' + v % 10);
        while ((v /= 10) != 0 || (i < b->n - 1 && t < 9));
        if (i < b->n - 1)
            while (t < 9)
                tmp[t++] = '0';
        while (t)
            out[len++] = tmp[--t];
    }
    return len;
}

/* d[0] is a spare '0' for a carry out of the top; d[1 ..] the digits; the point lies before index *point. Returns the digit count n
 * (index of the last digit + 1). v is finite and >= 0. */
static int expand_double(double v, char *d, int *point)
{
    union
    {
        double f;
        unsigned long long u;
    } bits;
    bits.f = v;
    int ex = (int)((bits.u >> 52) & 0x7FF);
    unsigned long long mant = bits.u & 0x000FFFFFFFFFFFFFULL;
    int e2;
    if (ex == 0)
        e2 = -1074;
    else
    {
        mant |= 1ULL << 52;
        e2 = ex - 1075;
    }
    d[0] = '0';
    int n = 1;
    if (mant == 0)
    {
        d[n++] = '0';
        *point = n;
        return n;
    }
    Big big;
    if (e2 >= 0)
    {
        big_set(&big, mant);
        while (e2 > 0)
        {
            int step = e2 > 29 ? 29 : e2;
            big_mul(&big, 1u << step);
            e2 -= step;
        }
        n += big_text(&big, d + n);
        *point = n;
        return n;
    }
    int k = -e2; /* v = mant / 2^k */
    unsigned long long ipart = 0, fm = mant;
    if (k < 64)
    {
        ipart = mant >> k;
        fm = mant & ((1ULL << k) - 1);
    }
    big_set(&big, ipart);
    n += big_text(&big, d + n);
    *point = n;
    if (fm == 0)
        return n;
    /* fraction = fm / 2^k = fm * 5^k / 10^k: the digits of fm * 5^k, padded on the left to k places */
    big_set(&big, fm);
    for (int left = k; left > 0;)
    {
        int step = left > 13 ? 13 : left; /* 5^13 < 2^32 */
        unsigned m = 1;
        for (int i = 0; i < step; i++)
            m *= 5;
        big_mul(&big, m);
        left -= step;
    }
    char fr[1100];
    int fl = big_text(&big, fr);
    for (int i = fl; i < k; i++)
        d[n++] = '0';
    for (int i = 0; i < fl; i++)
        d[n++] = fr[i];
    return n;
}

static char digit_at(const char *d, int n, int i) { return i < n ? d[i] : '0'; }

/* keep the first `keep` characters of d (counted from index 0), rounding the rest to nearest, ties to even */
static void round_digits(char *d, int n, int keep)
{
    if (keep >= n)
        return;
    int up = 0;
    if (d[keep] > '5')
        up = 1;
    else if (d[keep] == '5')
    {
        int rest = 0;
        for (int j = keep + 1; j < n; j++)
            if (d[j] != '0')
                rest = 1;
        up = rest || ((d[keep - 1] - '0') & 1);
    }
    for (int j = keep; j < n; j++)
        d[j] = '0';
    for (int j = keep - 1; up && j >= 0; j--)
    {
        if (d[j] == '9')
            d[j] = '0';
        else
        {
            d[j]++;
            up = 0;
        }
    }
}

/* Formats v (finite, >= 0) as printf would for conv 'e', 'f' or 'g' with precision prec (< 0: 6), '#' flag alt. No sign, no padding.
 * out needs room for FLOAT_BODY_MAX characters. Returns the length. */
int __fmt_double(char *out, double v, int conv, int prec, int alt)
{
    char d[DIGITS_MAX];
    int point, n = expand_double(v, d, &point);
    int o = 0;
    if (prec < 0)
        prec = 6;
    if (prec > 600)
        prec = 600; /* far beyond what a double holds; keeps the buffers small */
    int is_zero = (v == 0);

    if (conv == 'g')
    {
        int P = prec == 0 ? 1 : prec;
        int z = 1;
        while (z < n && d[z] == '0')
            z++;
        if (!is_zero)
            round_digits(d, n, z + P);
        int z0 = 0;
        while (z0 < n && d[z0] == '0')
            z0++;
        int X = is_zero ? 0 : point - 1 - z0;
        int fixed = (X >= -4 && X < P);
        int decimals = fixed ? P - 1 - X : P - 1;
        /* the digits are already rounded to P significant digits; drop trailing zeros unless '#' */
        if (!alt)
        {
            int last = is_zero ? 0 : P - 1;
            while (last > 0 && digit_at(d, n, is_zero ? 0 : z0 + last) == '0')
                last--;
            decimals = fixed ? (last - X > 0 ? last - X : 0) : last;
            if (is_zero)
                decimals = 0;
        }
        if (fixed)
        {
            int first = d[0] != '0' ? 0 : 1;
            if (is_zero)
                first = 1;
            for (int i = first; i < point; i++)
                out[o++] = digit_at(d, n, i);
            if (decimals > 0 || alt)
                out[o++] = '.';
            for (int i = 0; i < decimals; i++)
                out[o++] = digit_at(d, n, point + i);
        }
        else
        {
            out[o++] = digit_at(d, n, z0);
            if (decimals > 0 || alt)
                out[o++] = '.';
            for (int i = 0; i < decimals; i++)
                out[o++] = digit_at(d, n, z0 + 1 + i);
            out[o++] = 'e';
            out[o++] = X < 0 ? '-' : '+';
            int ax = X < 0 ? -X : X;
            if (ax >= 100)
                out[o++] = (char)('0' + ax / 100);
            out[o++] = (char)('0' + ax / 10 % 10);
            out[o++] = (char)('0' + ax % 10);
        }
        return o;
    }
    if (conv == 'f')
    {
        round_digits(d, n, point + prec);
        int first = d[0] != '0' ? 0 : 1;
        for (int i = first; i < point; i++)
            out[o++] = digit_at(d, n, i);
        if (prec > 0 || alt)
            out[o++] = '.';
        for (int i = 0; i < prec; i++)
            out[o++] = digit_at(d, n, point + i);
        return o;
    }
    /* 'e' */
    int z = 1;
    while (z < n && d[z] == '0')
        z++;
    if (!is_zero)
        round_digits(d, n, z + prec + 1);
    int z0 = 0;
    while (z0 < n && d[z0] == '0')
        z0++;
    int X = is_zero ? 0 : point - 1 - z0;
    out[o++] = is_zero ? '0' : digit_at(d, n, z0);
    if (prec > 0 || alt)
        out[o++] = '.';
    for (int i = 0; i < prec; i++)
        out[o++] = is_zero ? '0' : digit_at(d, n, z0 + 1 + i);
    out[o++] = 'e';
    out[o++] = X < 0 ? '-' : '+';
    int ax = X < 0 ? -X : X;
    if (ax >= 100)
        out[o++] = (char)('0' + ax / 100);
    out[o++] = (char)('0' + ax / 10 % 10);
    out[o++] = (char)('0' + ax % 10);
    return o;
}
