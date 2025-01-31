#ifndef ARRAY_H_INCLUDED
#define ARRAY_H_INCLUDED

#include "context.h"

#define Array(TYPE) \
    struct {                     \
        TYPE           *data;    \
        s64             count;   \
        s64             limit;   \
        Memory_context *context; \
    }

typedef Array(char)          char_array;
typedef Array(char_array)    char_array2;
typedef Array(u8)            u8_array;
typedef Array(u8_array)      u8_array2;
typedef Array(u8_array2)     u8_array3;
typedef Array(char *)        string_array; // An array of null-terminated strings.
typedef Array(int)           int_array;
typedef Array(s16)           s16_array;
typedef Array(s64)           s64_array;

void *maybe_grow_array(void *data, s64 *limit, s64 count, u64 unit_size, Memory_context *context);
void *array_reserve_(void *data, s64 *limit, s64 new_limit, u64 unit_size, Memory_context *context);
void reverse_array_(void *data, s64 limit, s64 count, u64 unit_size, Memory_context *context);
void array_unordered_remove_by_index_(void *data, s64 *count, u64 unit_size, s64 index_to_remove);

#define NewArray(ARRAY, CONTEXT) \
    ((ARRAY) = zero_alloc(1, sizeof(*ARRAY), (CONTEXT)), \
     (ARRAY)->context = (CONTEXT), \
     (ARRAY))

#define Add(ARRAY) \
    ((ARRAY)->data = maybe_grow_array((ARRAY)->data, &(ARRAY)->limit, (ARRAY)->count, sizeof((ARRAY)->data[0]), (ARRAY)->context), \
     (ARRAY)->count += 1, \
     &(ARRAY)->data[(ARRAY)->count-1])

#define array_reserve(ARRAY, LIMIT) \
    ((ARRAY)->data = array_reserve_((ARRAY)->data, &(ARRAY)->limit, (LIMIT), sizeof((ARRAY)->data[0]), (ARRAY)->context))

#define reverse_array(ARRAY) \
    (reverse_array_((ARRAY)->data, (ARRAY)->limit, (ARRAY)->count, sizeof((ARRAY)->data[0]), (ARRAY)->context))

#define array_unordered_remove_by_index(ARRAY, INDEX) \
    array_unordered_remove_by_index_((ARRAY)->data, &(ARRAY)->count, sizeof((ARRAY)->data), (INDEX))

#endif // ARRAY_H_INCLUDED
