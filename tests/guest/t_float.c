/* COP1 single/double arithmetic, conversions and comparisons. */
#include "../../guest/glibc.h"

static volatile float fv[] = {0.0f, 1.0f, -1.5f, 3.25f, 1e-3f, -2.75e4f, 123456.789f, 0.1f};
static volatile double dv[] = {0.0, 1.0, -1.5, 3.25, 1e-9, -2.75e8, 123456.789, 0.1};
#define NF (int)(sizeof(fv) / sizeof(fv[0]))

static u32 h = 2166136261u;
static void mixf(float f) { h = g_fnv1a(&f, 4, h); }
static void mixd(double d) { h = g_fnv1a(&d, 8, h); }
static void mixi(s32 v) { h = g_fnv1a(&v, 4, h); }

__attribute__((noinline)) static double my_sqrt(double x) { return __builtin_sqrt(x); }
__attribute__((noinline)) static float my_sqrtf(float x) { return __builtin_sqrtf(x); }

__attribute__((noinline)) static double sine(double x) {
  double term = x, sum = x;
  for (int n = 1; n < 12; n++) {
    term *= -x * x / ((2 * n) * (2 * n + 1));
    sum += term;
  }
  return sum;
}

int main(void) {
  for (int i = 0; i < NF; i++) {
    for (int j = 0; j < NF; j++) {
      float a = fv[i], b = fv[j];
      double c = dv[i], d = dv[j];
      mixf(a + b), mixf(a - b), mixf(a * b);
      if (b != 0) mixf(a / b);
      mixd(c + d), mixd(c - d), mixd(c * d);
      if (d != 0) mixd(c / d);
      mixi(a < b), mixi(a <= b), mixi(a == b), mixi(c < d), mixi(c <= d), mixi(c == d);
      mixd((double)a * d), mixf((float)c + b);
    }
    float a = fv[i];
    double c = dv[i];
    mixi((s32)a), mixi((s32)c), mixf((float)(s32)(a * 100)), mixd((double)(s32)(c / 3));
    mixf(-a), mixd(-c), mixf(__builtin_fabsf(a)), mixd(__builtin_fabs(c));
    if (a >= 0) mixf(my_sqrtf(a));
    if (c >= 0) mixd(my_sqrt(c));
  }
  g_printf("t_float hash=%08x\n", h);
  g_printf("sin(1)=%.9f sqrt2=%.9f pi*1e3=%.3f\n", sine(1.0), my_sqrt(2.0), 3141.5926535);
  float acc = 0;
  for (int i = 1; i <= 1000; i++) acc += 1.0f / (float)i;
  g_printf("harmonic(1000)=%.6f\n", (double)acc);
  return 0;
}
