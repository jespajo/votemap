#include <stdarg.h>

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

char_array *get_string(Memory_context *context, char *format, ...)
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

    // Find the index of the decimel place '.' character, or -1 if there was none.
    s64 decimel_index = -1;
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

char *trim_left_(char *string, char *trim_chars, int num_trim_chars)
{
    for (int i = 0; i < num_trim_chars; i++) {
        if (string[0] == trim_chars[i]) {
            return trim_left_(string + 1, trim_chars, num_trim_chars);
        }
    }
    return string;
}

bool starts_with_(char *string, char *match, s64 match_length)
{
    for (s64 i = 0; i < match_length; i++) {
        if (string[i] != match[i])  return false;
    }
    return true;
}

bool is_match(char *str, char *pat)
// Check whether a string matches a regex pattern. It must be a full match; /^(pat)$/ is implied.
// For now we only support the special meanings of two symbols: '.' and '*'.
//
// |Todo: Backslash to escape. Also when this is not so simplistic, we should create a regex module.
{
    if (*str == '\0') {
        if (*pat == '\0')  return true;

        while (*(pat+1) == '*') {
            if (*(pat+2) == '\0')  return true;
            pat += 2;
        }

        return false;
    }

    // There's more string to match.
    if (*pat == '\0')  return false;

    if (*(pat+1) == '*') {
        if (is_match(str, pat+2))  return true;

        if (*pat == *str || *pat == '.')  return is_match(str+1, pat);
    } else {
        if (*pat == *str || *pat == '.')  return is_match(str+1, pat+1);
    }

    return false;
}
