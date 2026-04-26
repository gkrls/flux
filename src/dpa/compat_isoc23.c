// compat_isoc23.c
// Provide __isoc23_* on old glibc by forwarding to classic strto*.
// Build this into libdpa.a (compiled as C, not C++).
#include <stdlib.h>
#include <inttypes.h>

#ifdef __cplusplus
extern "C" {
#endif

// Avoid macro redirects on newer glibc headers.
#ifdef strtol
#undef strtol
#endif
#ifdef strtoul
#undef strtoul
#endif
#ifdef strtoll
#undef strtoll
#endif
#ifdef strtoull
#undef strtoull
#endif
#ifdef strtod
#undef strtod
#endif
#ifdef strtof
#undef strtof
#endif
#ifdef strtold
#undef strtold
#endif
#ifdef strtoimax
#undef strtoimax
#endif
#ifdef strtoumax
#undef strtoumax
#endif

__attribute__((weak))
long __isoc23_strtol(const char* s, char** end, int base) {
  return strtol(s, end, base);
}
__attribute__((weak))
unsigned long __isoc23_strtoul(const char* s, char** end, int base) {
  return strtoul(s, end, base);
}
__attribute__((weak))
long long __isoc23_strtoll(const char* s, char** end, int base) {
  return strtoll(s, end, base);
}
__attribute__((weak))
unsigned long long __isoc23_strtoull(const char* s, char** end, int base) {
  return strtoull(s, end, base);
}
__attribute__((weak))
double __isoc23_strtod(const char* s, char** end) {
  return strtod(s, end);
}
__attribute__((weak))
float __isoc23_strtof(const char* s, char** end) {
  return strtof(s, end);
}
__attribute__((weak))
long double __isoc23_strtold(const char* s, char** end) {
  return strtold(s, end);
}
__attribute__((weak))
intmax_t __isoc23_strtoimax(const char* s, char** end, int base) {
  return strtoimax(s, end, base);
}
__attribute__((weak))
uintmax_t __isoc23_strtoumax(const char* s, char** end, int base) {
  return strtoumax(s, end, base);
}

#ifdef __cplusplus
}
#endif
