#ifndef STRINGS_H_INCLUDED
#define STRINGS_H_INCLUDED

#include "array.h"

char_array *get_string(Memory_Context *context, char *format, ...);
void print_string(char_array *out, char *format, ...);
void print_double(double number, char_array *out);

#endif // STRINGS_H_INCLUDED
