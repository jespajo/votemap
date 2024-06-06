#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "strings.h"

static void vaprintf(char_array *array, char *format, va_list args)
{
    int length = -1;

    // Try vsnprintf() twice. If there isn't enough room in the array the first time, we'll know
    // the length of the string post-formatting, so we can reserve space for another go.
    for (int attempt = 0; attempt < 2; attempt++) {
        // Avoid the undefined behaviour of adding an integer to NULL.
        char *data_end = (array->data) ? array->data + array->count : NULL;
        s64 free_space = array->limit - array->count;

        va_list args_copy;
        va_copy(args_copy, args);
        length = vsnprintf(data_end, free_space, format, args_copy);
        va_end(args_copy);

        if (length < free_space)  break;

        assert(attempt == 0);
        array_reserve(array, array->count + length + 1);
    }

    assert(0 <= length);
    assert(array->count + length < array->limit);

    array->count += length;
}

char_array *get_string(Memory_Context *context, char *format, ...)
{
    char_array *string = NewArray(string, context);

    va_list vargs;
    va_start(vargs, format);
    vaprintf(string, format, vargs);
    va_end(vargs);

    return string;
}
