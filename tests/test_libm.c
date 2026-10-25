/* Host test for the user math library (user/libm.c) and number conversion (user/numconv.c): the HarisOS functions, compiled under other
 * names (h_sin ...), are compared with the host C library on many inputs. Run by run_libm_tests.sh. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#undef errno
int errno; /* numconv.c sets errno; the host one is not used here */

double h_fabs(double), h_sqrt(double), h_floor(double), h_ceil(double), h_trunc(double), h_round(double), h_exp(double), h_exp2(double);
double h_log(double), h_log2(double), h_log10(double), h_pow(double, double), h_sin(double), h_cos(double), h_tan(double);
double h_asin(double), h_acos(double), h_atan(double), h_atan2(double, double), h_fmod(double, double), h_ldexp(double, int);
double h_frexp(double, int *), h_modf(double, double *), h_strtod(const char *, char **);
int __fmt_double(char *out, double v, int conv, int prec, int alt);

static int failures, checks;
static double abs_floor; /* trig functions near a zero are only absolutely accurate */

static void check_close(const char *name, double arg, double got, double want, double rel)
{
    checks++;
    int ok;
    if (isnan(want))
        ok = isnan(got);
    else if (isinf(want))
        ok = got == want;
    else
    {
        double tol = fabs(want) * rel + abs_floor;
        if (tol < 1e-300)
            tol = 1e-300;
        ok = fabs(got - want) <= tol;
    }
    if (!ok)
    {
        failures++;
        if (failures < 40)
            printf("FAIL %s(%.17g): got %.17g want %.17g\n", name, arg, got, want);
    }
}

static void check_exact(const char *name, double arg, double got, double want)
{
    checks++;
    if (!(got == want || (isnan(got) && isnan(want))) || signbit(got) != signbit(want))
    {
        failures++;
        if (failures < 40)
            printf("FAIL %s(%.17g): got %.17g want %.17g\n", name, arg, got, want);
    }
}

/* the same conversion through printf of the host and through __fmt_double */
static void check_fmt(double v, char conv, int prec, int alt)
{
    char mine[1100], want[1200], fmt[16];
    checks++;
    int n = __fmt_double(mine, v, conv, prec, alt);
    mine[n] = 0;
    snprintf(fmt, sizeof fmt, alt ? "%%#.%d%c" : "%%.%d%c", prec < 0 ? 6 : prec, conv);
    snprintf(want, sizeof want, fmt, v);
    if (strcmp(mine, want) != 0)
    {
        failures++;
        if (failures < 40)
            printf("FAIL fmt %s of %.17g: got \"%s\" want \"%s\"\n", fmt, v, mine, want);
    }
}

static unsigned long long rng = 88172645463325252ULL;
static unsigned long long next(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}
static double rnd(double lo, double hi) { return lo + (hi - lo) * ((double)(next() >> 11) / 9007199254740992.0); }

static double bits_double(unsigned long long u)
{
    double d;
    memcpy(&d, &u, 8);
    return d;
}

int main(void)
{
    static const double specials[] = {0.0, -0.0, 1.0, -1.0, 0.5, -0.5, 1.5, 2.5, -2.5, 3.5, 1e-300, 4.9e-324, 1e300, 1.7976931348623157e308,
                                      123456789.125, 1e15, 1e16, 4503599627370496.0, 4503599627370497.0, 0.1, 0.3, 100.0, 9.999999, 99999.95};
    for (unsigned i = 0; i < sizeof specials / sizeof *specials; i++)
    {
        double x = specials[i];
        check_exact("floor", x, h_floor(x), floor(x));
        check_exact("ceil", x, h_ceil(x), ceil(x));
        check_exact("trunc", x, h_trunc(x), trunc(x));
        check_exact("round", x, h_round(x), round(x));
        check_exact("fabs", x, h_fabs(x), fabs(x));
        check_close("exp", x, h_exp(x), exp(x), 4e-16);
        if (x > 0)
        {
            check_close("log", x, h_log(x), log(x), 4e-16);
            check_close("log2", x, h_log2(x), log2(x), 4e-16);
            check_close("log10", x, h_log10(x), log10(x), 4e-16);
            check_exact("sqrt", x, h_sqrt(x), sqrt(x));
        }
        for (int p = 0; p <= 6; p++)
            for (int a = 0; a < 2; a++)
            {
                check_fmt(fabs(x), 'f', p, a);
                check_fmt(fabs(x), 'e', p, a);
                check_fmt(fabs(x), 'g', p, a);
            }
        check_fmt(fabs(x), 'g', 14, 0);
        check_fmt(fabs(x), 'g', 17, 0);
    }
    check_exact("pow", 2, h_pow(2, 53), 9007199254740992.0);
    check_exact("pow", 10, h_pow(10, 2), 100.0);
    check_exact("pow", 2, h_pow(2, -1), 0.5);
    check_exact("pow", 0, h_pow(0, 0), 1.0);
    check_exact("pow", -2, h_pow(-2, 3), -8.0);
    check_exact("pow", -8, h_pow(-8, 2), 64.0);
    check_exact("pow", 0, h_pow(0, -1), HUGE_VAL);
    check_close("pow", 2, h_pow(2, 0.5), sqrt(2), 4e-16);
    check_exact("exp2", 3, h_exp2(3), 8.0);

    for (int i = 0; i < 20000; i++)
    {
        double x = rnd(-50, 50), y = rnd(-5, 5), p = rnd(0.001, 1000);
        abs_floor = 3e-16;
        check_close("sin", x, h_sin(x), sin(x), 1e-15);
        check_close("cos", x, h_cos(x), cos(x), 1e-15);
        check_close("tan", x, h_tan(x), tan(x), 1e-14);
        abs_floor = 0;
        check_close("atan", x, h_atan(x), atan(x), 4e-16);
        check_close("atan2", x, h_atan2(x, y), atan2(x, y), 4e-16);
        check_close("exp", x, h_exp(x), exp(x), 1e-15);
        check_close("log", p, h_log(p), log(p), 1e-15);
        check_close("pow", p, h_pow(p, y), pow(p, y), 1e-14);
        double u = rnd(-1, 1);
        check_close("asin", u, h_asin(u), asin(u), 1e-15);
        check_close("acos", u, h_acos(u), acos(u), 1e-15);
        check_exact("fmod", x, h_fmod(x * 1000, y), fmod(x * 1000, y));
        check_exact("floor", x, h_floor(x), floor(x));
        check_exact("ceil", x, h_ceil(x), ceil(x));
        int e1, e2;
        double m1 = h_frexp(p, &e1), m2 = frexp(p, &e2);
        check_exact("frexp", p, m1, m2);
        check_exact("frexp.e", p, e1, e2);
        check_exact("ldexp", p, h_ldexp(p, e1 - 7), ldexp(p, e1 - 7));
        double i1, i2;
        check_exact("modf", x, h_modf(x, &i1), modf(x, &i2));
        check_exact("modf.i", x, i1, i2);
    }
    /* random bit patterns: formatting must be exact for every finite double */
    for (int i = 0; i < 6000; i++)
    {
        double v = fabs(bits_double(next()));
        if (isnan(v) || isinf(v))
            continue;
        check_fmt(v, 'e', 16, 0);
        check_fmt(v, 'g', 17, 0);
        check_fmt(v, 'g', 14, 0);
        check_fmt(v, 'e', 0, 0);
        check_fmt(v, 'g', -1, 1);
        if (v < 1e30)
        {
            check_fmt(v, 'f', 6, 0);
            check_fmt(v, 'f', 20, 0);
        }
        char buf[64];
        snprintf(buf, sizeof buf, "%.17g", v);
        check_exact("strtod", v, h_strtod(buf, NULL), strtod(buf, NULL));
    }
    for (int i = 0; i < 20000; i++)
    {
        double v = rnd(0, 1000) * (i % 3 == 0 ? 1e-5 : i % 3 == 1 ? 1 : 1e9);
        check_fmt(v, 'g', 14, 0);
        check_fmt(v, 'f', 2, 0);
        check_fmt(v, 'f', 0, 0);
        check_fmt(v, 'e', 3, 0);
        char buf[64];
        snprintf(buf, sizeof buf, "%.14g", v);
        check_exact("strtod", v, h_strtod(buf, NULL), strtod(buf, NULL));
    }
    /* strtod: text forms and end pointers */
    static const char *texts[] = {"0", "  12", "-3.5e2x", "1e", ".5", "5.", "0x1p4", "0x.8", "0xA.8p1", "inf", "-Infinity", "1e400", "1e-400", "abc",
                                  "+7", "0x", "1.7976931348623157e308", "2.2250738585072014e-308", "123456789012345678901234567890", "0.1e1"};
    for (unsigned i = 0; i < sizeof texts / sizeof *texts; i++)
    {
        char *e1, *e2;
        double a = h_strtod(texts[i], &e1), b = strtod(texts[i], &e2);
        check_exact(texts[i], 0, a, b);
        checks++;
        if ((e1 - texts[i]) != (e2 - texts[i]))
        {
            failures++;
            printf("FAIL strtod end of \"%s\": %d want %d\n", texts[i], (int)(e1 - texts[i]), (int)(e2 - texts[i]));
        }
    }
    printf("libm tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
