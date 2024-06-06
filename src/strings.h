#ifndef STRINGS_H_INCLUDED
#define STRINGS_H_INCLUDED

#include "array.h"

char_array *get_string(Memory_Context *context, char *format, ...);
char_array2 *split_into_lines(char *data, s64 length, Memory_Context *context);

#endif // STRINGS_H_INCLUDED
