/* <math.h> for HarisOS user programs (implementation: user/libm.c). double only. */
#ifndef HARIS_MATH_H
#define HARIS_MATH_H

#define HUGE_VAL (__builtin_huge_val())
#define INFINITY (__builtin_inff())
#define NAN (__builtin_nanf(""))

#define M_E 2.7182818284590452354
#define M_LN2 0.69314718055994530942
#define M_LN10 2.30258509299404568402
#define M_PI 3.14159265358979323846
#define M_PI_2 1.57079632679489661923
#define M_SQRT2 1.41421356237309504880

int __isnan(double x);
int __isinf(double x);
int __signbit(double x);
#define isnan(x) __isnan((double)(x))
#define isinf(x) __isinf((double)(x))
#define isfinite(x) (!__isnan((double)(x)) && !__isinf((double)(x)))
#define signbit(x) __signbit((double)(x))

double fabs(double x);
double copysign(double x, double y);
double sqrt(double x);
double trunc(double x);
double floor(double x);
double ceil(double x);
double round(double x);
double modf(double x, double *ip);
double frexp(double x, int *e);
double ldexp(double x, int n);
double fmod(double x, double y);
double exp(double x);
double exp2(double x);
double log(double x);
double log2(double x);
double log10(double x);
double pow(double x, double y);
double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);
double sinh(double x);
double cosh(double x);
double tanh(double x);
double hypot(double x, double y);

#endif
