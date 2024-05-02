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


char_array load_text_file(char *file_name, Memory_Context *context)
{
    char_array buffer = {.context = context};

    FILE *file = fopen(file_name, "r");
    assert(file || !"Couldn't open file");

    while (true) {
        int c = fgetc(file);
        if (c == EOF)  break;

        // @Memory: This doubles the buffer when needed.
        *Add(&buffer) = (char)c;
    }

    *Add(&buffer) = '\0';
    buffer.count -= 1;

    fclose(file);

    return buffer;
}

u8_array load_binary_file(char *file_name, Memory_Context *context)
{
    u8_array buffer = {.context = context};

    FILE *file = fopen(file_name, "rb");
    assert(file || !"Couldn't open file");

    while (true) {
        int c = fgetc(file);
        if (c == EOF)  break;

        // @Memory: This doubles the buffer when needed.
        *Add(&buffer) = (u8)c;
    }

    fclose(file);

    return buffer;
}
