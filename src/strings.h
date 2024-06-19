#ifndef STRINGS_H_INCLUDED
#define STRINGS_H_INCLUDED

#include "array.h"

char_array *get_string(Memory_Context *context, char *format, ...);
void print_string(char_array *out, char *format, ...);
void print_double(double number, char_array *out);
bool string_contains_char(char const *string, s64 length, char c);

#define Contains(STATIC_STRING, CHAR) \
    string_contains_char((STATIC_STRING), lengthof(STATIC_STRING), (CHAR))

#endif // STRINGS_H_INCLUDED
