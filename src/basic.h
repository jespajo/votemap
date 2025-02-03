#ifndef BASIC_H_INCLUDED
#define BASIC_H_INCLUDED

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
// |Note: We conflate "OS" and "compiler". The assumption is that we're using GCC or Clang to compile on Linux and MSVC on Windows. It'd be better to separate these concepts, but that would imply that we test combinations like Clang on Windows, which we don't.

typedef  uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef   int8_t  s8;
typedef  int16_t s16;
typedef  int32_t s32;
typedef  int64_t s64;

#if OS == LINUX
  // FORMAT_ARG is the index of the format-string argument, starting from 1.
  // We assume the variadic arguments come right after the format string.
  #define PrintfLike(FORMAT_ARG)  __attribute__((format(printf, FORMAT_ARG, FORMAT_ARG+1)))
#else
  #define PrintfLike(FORMAT_ARG)
#endif

s64 round_up_pow2(s64 num);
bool is_power_of_two(s64 num);
void log_error_(char *file, int line, char *format, ...) PrintfLike(3);

#define log_error(...)  log_error_(__FILE__, __LINE__, __VA_ARGS__)

// Our assert() is pretty much the same as the standard C one, in that assertions will be removed if
// NDEBUG is defined. But they trigger a breakpoint before exiting if we're running in a debugger.
#ifndef NDEBUG
  #if OS == LINUX
    #define Breakpoint()  __builtin_trap()
  #elif OS == WINDOWS
    #define Breakpoint()  __debugbreak()
  #endif

  #define assert(COND) \
               ((COND) ? (void)0 \
                       : (log_error("Assertion failed: %s.", #COND), \
                          Breakpoint()))
#else
  #define Breakpoint()  ((void)0)
  #define assert(...)   ((void)0)
#endif

#define Fatal(...)  (log_error("Fatal error: " __VA_ARGS__), Breakpoint(), exit(1))

#define Min(A, B)  ((A) < (B) ? (A) : (B))
#define Max(A, B)  ((A) > (B) ? (A) : (B))
#define Clamp(MIN, VAL, MAX)  Min(Max(VAL, MIN), MAX)

#define countof(STATIC_ARRAY)    (s64)(sizeof(STATIC_ARRAY)/sizeof((STATIC_ARRAY)[0]))
#define lengthof(STATIC_STRING)  (countof(STATIC_STRING)-1)

#endif // BASIC_H_INCLUDED
