/* BoltOS Mini - mini/mathx.h: exact-to-an-ulp elementary functions for the
 * programs the Code app runs (mini/mathx.c). Including it after math.h with
 * MATHX_REPLACE defined makes exp, log, pow... mean these in that file. */
#pragma once
double x_exp(double x);
double x_log(double x);
double x_log2(double x);
double x_log10(double x);
double x_expm1(double x);
double x_log1p(double x);
double x_pow(double x, double y);
double x_sin(double x);
double x_cos(double x);
double x_tan(double x);
double x_atan(double x);
double x_atan2(double y, double x);
double x_asin(double x);
double x_acos(double x);
double x_sinh(double x);
double x_cosh(double x);
double x_tanh(double x);
double x_cbrt(double x);
double x_hypot(double a, double b);
double x_fmod(double x, double y);
#ifdef MATHX_REPLACE
#define exp   x_exp
#define log   x_log
#define log2  x_log2
#define log10 x_log10
#define expm1 x_expm1
#define log1p x_log1p
#define pow   x_pow
#define sin   x_sin
#define cos   x_cos
#define tan   x_tan
#define atan  x_atan
#define atan2 x_atan2
#define asin  x_asin
#define acos  x_acos
#define sinh  x_sinh
#define cosh  x_cosh
#define tanh  x_tanh
#define cbrt  x_cbrt
#define hypot x_hypot
#define fmod  x_fmod
#endif
