// Change the signature of aprintf to:
//
//      char_array *aprintf(Memory_Context *context, char *format, ...);
//
// Then the string builder can be as easy as:
//
//      char_array2 *builder = NewArray(builder, ctx);
//
//      *Add(builder) = aprintf("%d\n", num);
//      *Add(builder) = a_different_char_array;
//
// Then there's just:
//
//      char_array merge_strings(char_array2 string_array);
//

#include <errno.h> // For diagnosing file read/write errors.
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "array.h"

void *array_reserve_(void *data, s64 *limit, s64 new_limit, u64 unit_size, Memory_Context *context)
// Resize or allocate room for `new_limit` items. Modify *limit and return the new data pointer.
//
// This function modifies *limit, so why don't we make the first parameter `void **data` and get
// the function to modify *data as well? The reason is that the compiler lets us implicitly cast
// e.g. `int *` to `void *`, but not `int **` to `void **`.
{
    assert(new_limit > 0);
    assert(context);
    assert(new_limit > *limit); // We could just return early instead, but I can't think of a use-case. If one comes up we can change this.

    if (!data) {
        assert(*limit == 0);
        data = alloc(context, new_limit, unit_size);
    } else {
        data = resize(context, data, new_limit, unit_size);
    }

    *limit = new_limit;

    return data;
}

char_array *load_text_file(char *file_name, Memory_Context *context)
// Return NULL if the file can't be opened.
{
    char_array *buffer = NewArray(buffer, context);

    FILE *file = fopen(file_name, "r");
    if (!file) {
        Log("Couldn't open file %s.", file_name);
        return NULL;
    }

    while (true) {
        int c = fgetc(file);
        if (c == EOF)  break;

        // @Memory: This doubles the buffer when needed.
        *Add(buffer) = (char)c;
    }

    *Add(buffer) = '\0';
    buffer->count -= 1;

    fclose(file);

    return buffer;
}

u8_array *load_binary_file(char *file_name, Memory_Context *context)
// Return NULL if the file can't be opened.
{
    u8_array *buffer = NewArray(buffer, context);

    FILE *file = fopen(file_name, "rb");
    if (!file) {
        Log("Couldn't open file %s.", file_name);
        return NULL;
    }

    while (true) {
        int c = fgetc(file);
        if (c == EOF)  break;

        // @Memory: This doubles the buffer when needed.
        *Add(buffer) = (u8)c;
    }

    fclose(file);

    return buffer;
}

void write_array_to_file_(void *data, u64 unit_size, s64 count, char *file_name)
{
    FILE *file = fopen(file_name, "wb");
    
    if (!file) {
        char *reason = "";
        if (errno == 2)  reason = "Does that directory exist?";
        Error("Couldn't create file %s. %s", file_name, reason);
    }

    u64 num_chars_written = fwrite(data, unit_size, count, file);
    assert(num_chars_written > 0);

    fclose(file);
}

void reverse_array_(void *data, s64 limit, s64 count, u64 unit_size, Memory_Context *context)
// Reverse an array's data in place.
{
    assert(data);
    assert(context);

    // We can't reverse arrays with 0 or 1 elements.
    if (count <= 1)  return;

    // We need a temporary buffer the size of one element in the array. If the array has any spare
    // space after its data, we'll use that. If not, we'll allocate a temporary buffer, though we
    // won't bother to free it later. @Memory
    void *tmp;
    if (count < limit)  tmp = (u8 *)data + count*unit_size;
    else                tmp = alloc(context, 1, unit_size);

    for (s64 i = 0; i < count/2; i++) {
        void *first = (u8 *)data + i*unit_size;
        void *last  = (u8 *)data + (count-1-i)*unit_size;

        memcpy(tmp,   first, unit_size);
        memcpy(first, last,  unit_size);
        memcpy(last,  tmp,   unit_size);
    }

    // This is not necessary, but may help prevent confusion when debugging.
    memset(tmp, 0, unit_size);
}
