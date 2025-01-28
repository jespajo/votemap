#include <stdarg.h>

#include "basic.h"

bool is_power_of_two(s64 x)
{
    if (x <= 0)  return false;

    return !(x & (x - 1));
}

s64 round_up_pow2(s64 x)
{
    assert(x >= 0);

    if (is_power_of_two(x))  return x;

    // Right-propagate the leftmost 1-bit.
    x |= (x >> 1);
    x |= (x >> 2);
    x |= (x >> 4);
    x |= (x >> 8);
    x |= (x >> 16);
    x |= (x >> 32);

    assert(x < INT64_MAX);

    return x + 1;
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
