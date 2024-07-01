#ifndef ARRAY_H_INCLUDED
#define ARRAY_H_INCLUDED

#include "context.h"

#define Array(TYPE) \
    struct {                     \
        TYPE           *data;    \
        s64             count;   \
        s64             limit;   \
        Memory_Context *context; \
    }

typedef Array(char)          char_array;
typedef Array(u8)            u8_array;
typedef Array(char_array *)  char_array2;
typedef Array(char *)        string_array; // An array of null-terminated strings.
typedef Array(int)           int_array;

void *array_reserve_(void *data, s64 *limit, s64 new_limit, u64 unit_size, Memory_Context *context);
char_array *load_text_file(char *file_name, Memory_Context *context);
u8_array *load_binary_file(char *file_name, Memory_Context *context);
void write_array_to_file_(void *data, u64 unit_size, s64 count, char *file_name);
void reverse_array_(void *data, s64 limit, s64 count, u64 unit_size, Memory_Context *context);
void array_unordered_remove_by_index_(void *data, s64 *count, u64 unit_size, s64 index_to_remove);

#define NewArray(ARRAY, CONTEXT) \
    ((ARRAY) = zero_alloc((CONTEXT), 1, sizeof(*ARRAY)), \
     (ARRAY)->context = (CONTEXT), \
     (ARRAY))

#define Add(ARRAY) \
    ((ARRAY)->data = double_if_needed((ARRAY)->data, &(ARRAY)->limit, (ARRAY)->count, sizeof((ARRAY)->data[0]), (ARRAY)->context), \
     (ARRAY)->count += 1, \
     &(ARRAY)->data[(ARRAY)->count-1])

#define array_reserve(ARRAY, LIMIT) \
    ((ARRAY)->data = array_reserve_((ARRAY)->data, &(ARRAY)->limit, (LIMIT), sizeof((ARRAY)->data[0]), (ARRAY)->context))

#define write_array_to_file(ARRAY, FILE_NAME)  \
    write_array_to_file_((ARRAY)->data, sizeof((ARRAY)->data[0]), (ARRAY)->count, (FILE_NAME))

#define reverse_array(ARRAY) \
    (reverse_array_((ARRAY)->data, (ARRAY)->limit, (ARRAY)->count, sizeof((ARRAY)->data[0]), (ARRAY)->context))

#define array_unordered_remove_by_index(ARRAY, INDEX) \
    array_unordered_remove_by_index_((ARRAY)->data, &(ARRAY)->count, sizeof((ARRAY)->data), (INDEX))

#endif // ARRAY_H_INCLUDED
