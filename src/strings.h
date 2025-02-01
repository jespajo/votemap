#ifndef STRINGS_H_INCLUDED
#define STRINGS_H_INCLUDED

#include "array.h"

char_array *get_string(Memory_context *context, char *format, ...);
void append_string(char_array *out, char *format, ...);
void print_double(double number, char_array *out);
bool string_contains_char(char const *string, s64 length, char c);
char *trim_left_(char *string, char *trim_chars, int num_trim_chars);
bool starts_with_(char *string, char *match, s64 match_length);
char_array2 *split_string(char *string, s64 length, char split_char, Memory_context *context);
u8 hex_to_byte(char c1, char c2);

//
// These macros take a static string as an argument. A static string is either
//
//      #define A_STRING "hello"
//
// or
//
//      char const A_STRING[] = "hello";
//
// The point is, you can use them with a string you've declared on the stack as long as you declare
// it as an array, not a pointer. (You'll get a compiler error if you declare it as a pointer.)
//
// If you #define, you can take advantage of the preprocessor's string concatenation, e.g.
//
//      #define BRACKET_CHARS "(){}[]"
//
//      bool is_bracket_or_x = Contains(BRACKET_CHARS "x", c);
//
// |Inconsistent: These functions and macros are all very similar, so they should look the same.
// Also, why is string_contains_char the only one that needs a const string?
//
#define trim_left(STRING, TRIM_CHARS)     trim_left_((STRING), (TRIM_CHARS), lengthof(TRIM_CHARS))
#define starts_with(DATA, STATIC_STRING)  starts_with_((DATA), (STATIC_STRING), lengthof(STATIC_STRING))
#define Contains(STATIC_STRING, CHAR)     string_contains_char((STATIC_STRING), lengthof(STATIC_STRING), (CHAR))

#define WHITESPACE " \n\t\r"

#endif // STRINGS_H_INCLUDED
