/* HarisOS user C library, math (double only; see math.h). The bit-level functions (floor, ceil, fmod, frexp, ldexp, modf) are plain C
 * on the IEEE 754 layout; exp, log, pow and the trigonometric functions use the x87 unit, which does the work in 80 bits inside one
 * instruction sequence, so the 64-bit result is rounded only once at the end (a "long double" is just a double on this target, so C
 * code cannot ask for that precision directly). The kernel gives every process a clean x87 state (include/fpu.h). */
#include "libc.h"
#include <math.h>

typedef union
{
    double d;
    unsigned long long u;
} Bits;

static unsigned long long bits_of(double x)
{
    Bits b;
    b.d = x;
    return b.u;
}

static double from_bits(unsigned long long u)
{
    Bits b;
    b.u = u;
    return b.d;
}

/* The x87 unit may be set to round every step to 53 bits (the host C library of a test does that); these make it keep all 64 bits
 * of its registers around a block of x87 instructions, so the result is rounded only once, when it is stored as a double. */
#define X87_BEGIN                                  \
    unsigned short cw_old_, cw_ext_;               \
    __asm__ volatile("fnstcw %0" : "=m"(cw_old_)); \
    cw_ext_ = (unsigned short)(cw_old_ | 0x300);   \
    __asm__ volatile("fldcw %0" : : "m"(cw_ext_))
#define X87_END __asm__ volatile("fldcw %0" : : "m"(cw_old_))

#define SIGN_MASK 0x8000000000000000ULL
#define EXP_MASK 0x7FF0000000000000ULL
#define MANT_MASK 0x000FFFFFFFFFFFFFULL

int __isnan(double x) { return (bits_of(x) & ~SIGN_MASK) > EXP_MASK; }
int __isinf(double x) { return (bits_of(x) & ~SIGN_MASK) == EXP_MASK; }
int __signbit(double x) { return (int)(bits_of(x) >> 63); }

double fabs(double x) { return from_bits(bits_of(x) & ~SIGN_MASK); }

double copysign(double x, double y) { return from_bits((bits_of(x) & ~SIGN_MASK) | (bits_of(y) & SIGN_MASK)); }

double sqrt(double x)
{
    double r;
    __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x));
    return r;
}

/* ---- rounding to integers ------------------------------------------------------------------------------------------------------ */
/* Below 2^52 a double has fractional bits; clearing the mantissa bits under the binary point gives the value rounded toward zero. */
double trunc(double x)
{
    unsigned long long u = bits_of(x);
    int e = (int)((u >> 52) & 0x7FF) - 1023; /* unbiased exponent */
    if (e >= 52)
        return x; /* already an integer (or inf / nan) */
    if (e < 0)
        return from_bits(u & SIGN_MASK); /* |x| < 1: a zero with the sign of x */
    return from_bits(u & ~(MANT_MASK >> e));
}

double floor(double x)
{
    double t = trunc(x);
    return (x < t) ? t - 1.0 : t;
}

double ceil(double x)
{
    double t = trunc(x);
    return (x > t) ? t + 1.0 : (t == 0 && __signbit(x) ? -0.0 : t);
}

double round(double x)
{
    double t = trunc(x);
    if (fabs(x - t) >= 0.5)
        t += copysign(1.0, x);
    return t;
}

double modf(double x, double *ip)
{
    double t = trunc(x);
    *ip = t;
    if (__isinf(x))
        return copysign(0.0, x);
    return copysign(x - t, x);
}

/* ---- frexp / ldexp / fmod ------------------------------------------------------------------------------------------------------ */
double frexp(double x, int *e)
{
    unsigned long long u = bits_of(x);
    int ex = (int)((u >> 52) & 0x7FF);
    *e = 0;
    if (ex == 0x7FF || (u & ~SIGN_MASK) == 0)
        return x; /* inf, nan, zero */
    if (ex == 0)  /* subnormal: scale up by 2^64 first */
    {
        x *= 18446744073709551616.0;
        u = bits_of(x);
        ex = (int)((u >> 52) & 0x7FF);
        *e = -64;
    }
    *e += ex - 1022;
    return from_bits((u & ~EXP_MASK) | (1022ULL << 52)); /* mantissa in [0.5, 1) */
}

double ldexp(double x, int n)
{
    if (x == 0 || __isinf(x) || __isnan(x))
        return x;
    /* multiply in steps of 2^1023 / 2^-1022 so that no intermediate over- or underflows too early */
    if (n > 1023)
    {
        x *= 8.98846567431158e307; /* 2^1023 */
        n -= 1023;
        if (n > 1023)
        {
            x *= 8.98846567431158e307;
            n -= 1023;
            if (n > 1023)
                n = 1023;
        }
    }
    else if (n < -1022)
    {
        /* 2^-1022 * 2^53 keeps the low bits of x until the last step, which rounds once */
        x *= 2.2250738585072014e-308 * 9007199254740992.0;
        n += 1022 - 53;
        if (n < -1022)
        {
            x *= 2.2250738585072014e-308 * 9007199254740992.0;
            n += 1022 - 53;
            if (n < -1022)
                n = -1022;
        }
    }
    return x * from_bits((unsigned long long)(n + 1023) << 52);
}

/* exact remainder with the sign of x: shift-and-subtract on the integer mantissas (as in the classic fdlibm / musl code) */
double fmod(double x, double y)
{
    unsigned long long ux = bits_of(x), uy = bits_of(y);
    int ex = (int)((ux >> 52) & 0x7FF), ey = (int)((uy >> 52) & 0x7FF);
    unsigned long long sx = ux & SIGN_MASK;
    if ((uy << 1) == 0 || __isnan(y) || ex == 0x7FF)
        return (x * y) / (x * y); /* nan */
    if ((ux << 1) <= (uy << 1))
    {
        if ((ux << 1) == (uy << 1))
            return 0.0 * x; /* signed zero */
        return x;           /* |x| < |y| */
    }
    /* make both mantissas integers with the hidden bit in place */
    if (ex == 0)
    {
        for (unsigned long long i = ux << 12; !(i >> 63); ex--, i <<= 1)
            ;
        ux <<= (unsigned)(-ex + 1);
    }
    else
    {
        ux &= (~0ULL) >> 12;
        ux |= 1ULL << 52;
    }
    if (ey == 0)
    {
        for (unsigned long long i = uy << 12; !(i >> 63); ey--, i <<= 1)
            ;
        uy <<= (unsigned)(-ey + 1);
    }
    else
    {
        uy &= (~0ULL) >> 12;
        uy |= 1ULL << 52;
    }
    for (; ex > ey; ex--)
    {
        unsigned long long d = ux - uy;
        if (!(d >> 63))
        {
            if (d == 0)
                return 0.0 * x;
            ux = d;
        }
        ux <<= 1;
    }
    unsigned long long d = ux - uy;
    if (!(d >> 63))
    {
        if (d == 0)
            return 0.0 * x;
        ux = d;
    }
    for (; (ux >> 52) == 0; ux <<= 1, ex--)
        ;
    if (ex > 0)
    {
        ux -= 1ULL << 52;
        ux |= (unsigned long long)ex << 52;
    }
    else
        ux >>= (unsigned)(-ex + 1);
    return from_bits(ux | sx);
}

/* ---- exp / log / pow (x87) ----------------------------------------------------------------------------------------------------- */
/* 2^t, with t on top of the x87 stack, in 80 bits: split t into an integer n and a fraction f in [-0.5, 0.5], 2^f = f2xm1(f) + 1, then
 * fscale adds n to the exponent. Leaves the result on top. The caller has already checked that the result is in range. */
#define X87_EXP2_ASM                 \
    "fld %%st(0)\n\t"                /* t t */ \
    "frndint\n\t"                    /* n t */ \
    "fsubr %%st(0), %%st(1)\n\t"     /* n f   (st1 = st1 - st0) */ \
    "fxch\n\t"                       /* f n */ \
    "f2xm1\n\t"                      /* 2^f-1 n */ \
    "fld1\n\t"                       \
    "faddp\n\t"                      /* 2^f n */ \
    "fscale\n\t"                     /* 2^f * 2^n n */ \
    "fstp %%st(1)\n\t"

static double exp2_x87(double t)
{
    double r;
    X87_BEGIN;
    __asm__ volatile(
        "fldl %1\n\t" X87_EXP2_ASM "fstpl %0\n\t"
        : "=m"(r)
        : "m"(t)
        : "st", "st(1)", "memory");
    X87_END;
    return r;
}

double exp2(double x)
{
    if (__isnan(x))
        return x;
    if (x >= 1024.0)
        return HUGE_VAL;
    if (x < -1075.0)
        return 0.0;
    if (x < -1021.0) /* the subnormal range: do it in two steps so fscale does not flush to zero early */
        return exp2_x87(x + 64.0) * 5.421010862427522e-20; /* 2^-64 */
    return exp2_x87(x);
}

double exp(double x)
{
    if (__isnan(x))
        return x;
    if (x > 709.782712893384)
        return HUGE_VAL;
    if (x < -745.2)
        return 0.0;
    double r;
    double off = x < -708.0 ? 64.0 : 0.0; /* near the subnormal range: scale up first, then back down */
    X87_BEGIN;
    __asm__ volatile(
        "fldl %1\n\t"
        "fldl2e\n\t"
        "fmulp\n\t" /* x * log2(e), in 80 bits */
        "faddl %2\n\t" X87_EXP2_ASM "fstpl %0\n\t"
        : "=m"(r)
        : "m"(x), "m"(off)
        : "st", "st(1)", "memory");
    X87_END;
    return off != 0.0 ? r * 5.421010862427522e-20 : r;
}

/* log of x for finite x > 0 in 80 bits: CONST_INSN pushes ln 2, 1 or log10 2, which fyl2x multiplies with log2(x) */
#define LOG_X87(NAME, CONST_INSN)                                      \
    double NAME(double x)                                              \
    {                                                                  \
        if (__isnan(x))                                                \
            return x;                                                  \
        if (x < 0)                                                     \
            return (x - x) / (x - x); /* nan */                        \
        if (x == 0)                                                    \
            return -HUGE_VAL;                                          \
        if (__isinf(x))                                                \
            return x;                                                  \
        double r;                                                      \
        X87_BEGIN;                                                     \
        __asm__ volatile(CONST_INSN "\n\t"                             \
                         "fldl %1\n\t"                                 \
                         "fyl2x\n\t"                                   \
                         "fstpl %0\n\t"                                \
                         : "=m"(r)                                     \
                         : "m"(x)                                      \
                         : "st", "st(1)", "memory");                   \
        X87_END;                                                       \
        return r;                                                      \
    }

LOG_X87(log, "fldln2")
LOG_X87(log2, "fld1")
LOG_X87(log10, "fldlg2")

static int is_odd_integer(double y)
{
    if (fabs(y) >= 9007199254740992.0 || y != trunc(y))
        return 0;
    return fmod(y, 2.0) != 0.0;
}

double pow(double x, double y)
{
    if (y == 0 || x == 1)
        return 1.0;
    if (__isnan(x) || __isnan(y))
        return x + y;
    if (__isinf(y))
    {
        double ax = fabs(x);
        if (ax == 1)
            return 1.0;
        return (ax > 1) == (y > 0) ? HUGE_VAL : 0.0;
    }
    if (x == 0)
    {
        if (y < 0)
            return is_odd_integer(y) ? copysign(HUGE_VAL, x) : HUGE_VAL;
        return is_odd_integer(y) ? x : 0.0;
    }
    if (__isinf(x))
    {
        if (x > 0)
            return y > 0 ? HUGE_VAL : 0.0;
        if (y > 0)
            return is_odd_integer(y) ? -HUGE_VAL : HUGE_VAL;
        return is_odd_integer(y) ? -0.0 : 0.0;
    }
    int neg = 0;
    if (x < 0)
    {
        if (y != trunc(y))
            return (x - x) / (x - x); /* nan: a fractional power of a negative number */
        neg = is_odd_integer(y);
        x = -x;
    }
    /* x^y = 2^(y * log2 x). The product stays in an 80 bit register all the way into the 2^t step: rounding it to a double first would
     * lose about 3e-16 relative to the result, so that pow(10, 2) would not be exactly 100. A rough double estimate decides the range. */
    double est = y * log2(x);
    if (est >= 1024.0)
        return neg ? -HUGE_VAL : HUGE_VAL;
    if (est < -1076.0)
        return neg ? -0.0 : 0.0;
    double off = est < -1020.0 ? 64.0 : 0.0; /* subnormal results: scale up first, then back down */
    double r;
    X87_BEGIN;
    __asm__ volatile(
        "fldl %2\n\t"
        "fldl %1\n\t"
        "fyl2x\n\t"
        "faddl %3\n\t" X87_EXP2_ASM "fstpl %0\n\t"
        : "=m"(r)
        : "m"(x), "m"(y), "m"(off)
        : "st", "st(1)", "memory");
    X87_END;
    if (off != 0.0)
        r *= 5.421010862427522e-20;
    return neg ? -r : r;
}

/* ---- trigonometry (x87) -------------------------------------------------------------------------------------------------------- */
/* fsin / fcos / fptan only accept |x| < 2^63; a larger argument is first brought into range with fmod by 2*pi (a 53-bit pi, so
 * such huge arguments lose accuracy, as they do on most simple C libraries). */
static double reduce_arg(double x)
{
    if (fabs(x) >= 9223372036854775808.0)
        x = fmod(x, 6.283185307179586);
    return x;
}

double sin(double x)
{
    if (__isnan(x) || __isinf(x))
        return (x - x) / (x - x);
    x = reduce_arg(x);
    double r;
    X87_BEGIN;
    __asm__("fldl %1\n\tfsin\n\tfstpl %0" : "=m"(r) : "m"(x) : "st");
    X87_END;
    return r;
}

double cos(double x)
{
    if (__isnan(x) || __isinf(x))
        return (x - x) / (x - x);
    x = reduce_arg(x);
    double r;
    X87_BEGIN;
    __asm__("fldl %1\n\tfcos\n\tfstpl %0" : "=m"(r) : "m"(x) : "st");
    X87_END;
    return r;
}

double tan(double x)
{
    if (__isnan(x) || __isinf(x))
        return (x - x) / (x - x);
    x = reduce_arg(x);
    double r;
    /* fptan leaves tan(x) and then pushes 1.0 */
    X87_BEGIN;
    __asm__("fldl %1\n\tfptan\n\tfstp %%st(0)\n\tfstpl %0" : "=m"(r) : "m"(x) : "st", "st(1)");
    X87_END;
    return r;
}

double atan2(double y, double x)
{
    if (__isnan(x) || __isnan(y))
        return x + y;
    double r;
    /* fpatan: st(1) = atan(st(1) / st(0)), quadrant-correct, then pops */
    X87_BEGIN;
    __asm__("fldl %1\n\tfldl %2\n\tfpatan\n\tfstpl %0" : "=m"(r) : "m"(y), "m"(x) : "st", "st(1)");
    X87_END;
    return r;
}

double atan(double x) { return atan2(x, 1.0); }

double asin(double x)
{
    if (__isnan(x) || x > 1 || x < -1)
        return (x - x) / (x - x);
    return atan2(x, sqrt((1.0 - x) * (1.0 + x)));
}

double acos(double x)
{
    if (__isnan(x) || x > 1 || x < -1)
        return (x - x) / (x - x);
    return atan2(sqrt((1.0 - x) * (1.0 + x)), x);
}

double sinh(double x)
{
    double e = exp(fabs(x));
    double r = (e - 1.0 / e) / 2.0;
    return x < 0 ? -r : r;
}

double cosh(double x)
{
    double e = exp(fabs(x));
    return (e + 1.0 / e) / 2.0;
}

double tanh(double x)
{
    if (fabs(x) > 22.0)
        return x < 0 ? -1.0 : 1.0;
    double e = exp(2.0 * fabs(x));
    double r = (e - 1.0) / (e + 1.0);
    return x < 0 ? -r : r;
}

double hypot(double x, double y) { return sqrt(x * x + y * y); }
