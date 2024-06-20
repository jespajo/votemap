#ifndef BASIC_H_INCLUDED
#define BASIC_H_INCLUDED

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h> // for exit().

// Support `#if OS == LINUX` style macros:
#define LINUX    1
#define WINDOWS  2
#ifdef _WIN32
  #define OS WINDOWS
#elif __linux__
  #define OS LINUX
#else
  #error "We couldn't identify the operating system."
#endif

typedef unsigned char   u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef size_t         u64;
typedef signed char     s8;
typedef signed short   s16;
typedef signed int     s32;
typedef ptrdiff_t      s64;

#define S64_MIN  PTRDIFF_MIN
#define S64_MAX  PTRDIFF_MAX

s64 round_up_pow2(s64 num);
bool is_power_of_two(s64 num);
void log_error_(char *file, int line, char *format, ...);
float frand();
float lerp(float a, float b, float t);
void Log(char *format, ...);

#define log_error(...)  log_error_(__FILE__, __LINE__, __VA_ARGS__)

#ifdef DEBUG
  #if OS == LINUX
    #define Breakpoint()  __builtin_trap()
  #elif OS == WINDOWS
    #define Breakpoint()  __debugbreak()
  #endif
#else
  #define Breakpoint()  ((void)0)
#endif

#ifdef NDEBUG
  #define assert(...)   ((void)0)
#else
  #define assert(COND) \
               ((COND) ? (void)0 \
                       : (log_error("Assertion failed: %s.", #COND), \
                          Breakpoint()))
#endif

#define Error(...)  (log_error(__VA_ARGS__), Breakpoint())

#define Fatal(...)  (Error("Fatal error: " __VA_ARGS__), exit(1))

#define Min(A, B)  ((A) < (B) ? (A) : (B))
#define Max(A, B)  ((A) > (B) ? (A) : (B))
#define Clamp(MIN, VAL, MAX)  Min(Max(VAL, MIN), MAX)

#define countof(STATIC_ARRAY)    (s64)(sizeof(STATIC_ARRAY)/sizeof((STATIC_ARRAY)[0]))
#define lengthof(STATIC_STRING)  (countof(STATIC_STRING)-1)

#endif // BASIC_H_INCLUDED
