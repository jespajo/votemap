#include <stdarg.h>

#include "basic.h"

s64 round_up_pow2(s64 num)
{
    assert(num >= 0);
    assert(num <= (INT64_MAX >> 1));

    s64 result = 1;

    while (result < num)  result <<= 1;

    return result;
}

bool is_power_of_two(s64 num)
{
    return num == round_up_pow2(num);
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
