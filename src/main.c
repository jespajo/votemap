#include "map.h"
#include "array.h"

#include <stdio.h>

int main()
{
    Memory_Context *context = new_context(NULL);

    char_array *a = NewArray(a, context);

    *Add(a) = 'v';
    *Add(a) = 'o';
    *Add(a) = 't';
    *Add(a) = 'e';
    *Add(a) = 'm';
    *Add(a) = 'a';
    *Add(a) = 'p';
    *Add(a) = '!';
    *Add(a) = '\0';

    puts(a->data);

    free_context(context);
    return 0;
}
