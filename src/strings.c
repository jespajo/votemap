#include <ctype.h>
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

void append_string(char_array *out, char *format, ...)
{
    va_list vargs;
    va_start(vargs, format);
    vaprintf(out, format, vargs);
    va_end(vargs);
}

void print_double(double number, char_array *out)
// Remove trailing zeroes from the fractional part.
// If it's a whole number then remove the whole fractional part.
//
// |Todo:
// | - Make it configurable: padding, precision, trailing zeroes.
// | - Get this function to take a size, then we can have a FormatFloat macro that you can call with floats or doubles and by default it should print the precision that makes sense for the type.
// | - Maybe just return a struct on the stack with an embedded char[] since the size is predictable.
{
    s64 old_count = out->count;
    append_string(out, "%.15f", number);

    // Find the index of the decimel place '.' character.
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

char_array2 *split_string(char *string, s64 length, char split_char, Memory_context *context)
{
    char_array2 *result = NewArray(result, context);

    char *buffer = alloc(length+1, sizeof(char), context);
    memcpy(buffer, string, length);

    // If true, the returned array will have empty char_arrays where the split_char appears twice in a row,
    // or at the start or end of the string. We may want to make this configurable later.
    bool include_empties = false;

    s64 i = 0;
    while (i <= length) {
        s64 j = i;
        while (j < length && buffer[j] != split_char)  j += 1;

        buffer[j] = '\0';

        // We don't give the segments a limit or context because you can't Add() to them,
        // since they're just pointers into a buffer---they don't own their data.
        char_array segment = {.data = &buffer[i], .count = j-i};

        if (segment.count > 0 || include_empties)  *Add(result) = segment;

        i = j + 1;
    }

    return result;
}

u8 hex_to_byte(char c1, char c2)
// Turn two hexadecimal digits into a byte of data. E.g. hex_to_byte('8', '0') -> 128 (0x80).
{
    // The caller should have already checked the characters with isxdigit().
    assert(isxdigit(c1) && isxdigit(c2));

    c1 |= 0x20; // OR-ing with 0x20 makes ASCII letters lowercase and doesn't affect ASCII numbers.
    c2 |= 0x20;

    u8 x1 = c1 <= '9' ? c1-'0' : c1-'a'+10;
    u8 x2 = c2 <= '9' ? c2-'0' : c2-'a'+10;

    return (u8)((x1 << 4) | x2);
}
