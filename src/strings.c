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
        array_reserve(array, round_up_pow2(array->count + length + 1));
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

void print_string(char_array *out, char *format, ...)
{
    va_list vargs;
    va_start(vargs, format);
    vaprintf(out, format, vargs);
    va_end(vargs);
}

void print_double(double number, char_array *out)
// We can extend this later. For now just remove trailing zero decimal places and if it's a whole
// number then remove the whole decimal part.
{
    s64 old_count = out->count;
    print_string(out, "%.15f", number);

    s64 decimel_index = -1; // The index of the decimel place '.' character, or -1 if there was none.
    for (s64 i = old_count; i < out->count; i++) {
        if (out->data[i] != '.')  continue;
        decimel_index = i;
        break;
    }
    if (decimel_index < 0)  return;

    // It's a number with a fractional part. Remove trailing zeroes.
    for (s64 i = out->count-1; i > decimel_index; i--) {
         if (out->data[i] == '0')  out->count -= 1;
    }
    if (out->data[out->count-1] == '.')  out->count -= 1; // Remove the dot too if it was all zeroes.
    out->data[out->count] = '\0';
}

bool string_contains_char(char const *string, s64 length, char c)
{
    for (s64 i = 0; i < length; i++) {
        if (string[i] == c)  return true;
    }
    return false;
}
