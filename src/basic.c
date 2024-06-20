#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "basic.h"

s64 round_up_pow2(s64 num)
{
    // |Fixme: Assert not out of max range.

    s64 result = 1;

    while (result < num)  result <<= 1;

    return result;
}

bool is_power_of_two(s64 num)
{
    s64 round = round_up_pow2(num);

    return (num == round);
}

void log_error_(char *file, int line, char *format, ...)
{
    fprintf(stderr, "%s:%d: ", file, line);

    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);

    fprintf(stderr, "\n");
    fflush(stderr);
}

void assert_(bool cond, char *cond_text, char *file, int line) // |Deprecated.
{
    if (!cond) {
        log_error("%s:%d: Assertion failed: `%s`.\n", file, line, cond_text);
        exit(1);
    }
}

float frand()
{
    return rand()/(float)RAND_MAX;
}

float lerp(float a, float b, float t)
{
    return (1.0-a)*t + b*t;
}

void Log(char *format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stdout, format, args);
    va_end(args);

    fprintf(stdout, "\n");
    fflush(stdout);
}
