#include <math.h>
#include <stdint.h>
#include <stdlib.h>

// Viper FFI expects primitive arguments directly.
// We map these C functions directly to avoid intermediate structs for simple math.

double math_sin(double x) { return sin(x); }
double math_cos(double x) { return cos(x); }
double math_tan(double x) { return tan(x); }
double math_asin(double x) { return asin(x); }
double math_acos(double x) { return acos(x); }
double math_atan(double x) { return atan(x); }
double math_atan2(double y, double x) { return atan2(y, x); }
double math_sinh(double x) { return sinh(x); }
double math_cosh(double x) { return cosh(x); }
double math_tanh(double x) { return tanh(x); }

double math_exp(double x) { return exp(x); }
double math_log(double x) { return log(x); }
double math_log10(double x) { return log10(x); }
double math_pow(double base, double exp) { return pow(base, exp); }
double math_sqrt(double x) { return sqrt(x); }
double math_ceil(double x) { return ceil(x); }
double math_floor(double x) { return floor(x); }
double math_abs(double x) { return fabs(x); }

// Constants
double math_pi() { return 3.14159265358979323846; }
double math_e() { return 2.71828182845904523536; }
