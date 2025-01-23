#include <stdarg.h>

#include "array.h"

void *maybe_grow_array(void *data, s64 *limit, s64 count, u64 unit_size, Memory_context *context)
// If the array doesn't exist yet, create it. If it exists and it's full, double its size. In either case,
// modify *limit and return a pointer to the new data. Otherwise just return data.
//
// You may ask, if this function modifies *limit, why does it rely on its caller to assign the return value
// to data themselves? Couldn't we just make make the first parameter `void **data` and get the function to
// modify *data as well? No. The reason is that the compiler lets us implicitly cast e.g. `int *` to `void *`,
// but not `int **` to `void **`.
{
    s64 INITIAL_LIMIT = 4; // If the array is unitialised, how many units to make room for in the first allocation.

    if (!data) {
        // The array needs to be initialised.
        assert(*limit == 0 && count == 0);

        *limit = INITIAL_LIMIT;
        data   = alloc(*limit, unit_size, context);
    } else if (count >= *limit) {
        // The array needs to be resized.
        assert(count == *limit);

        // Make sure we only use this function for arrays that should increase in powers of two. This assert will trip
        // if we use array_reserve() to reserve a non-power-of-two number of bytes for an array and then exceed this
        // limit with Add(). In this case, just round up the array_reserve() argument to a power of two.
        assert(is_power_of_two(*limit));

        *limit *= 2;
        data    = resize(data, *limit, unit_size, context);
    }

    return data;
}

void *array_reserve_(void *data, s64 *limit, s64 new_limit, u64 unit_size, Memory_context *context)
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
        data = alloc(new_limit, unit_size, context);
    } else {
        data = resize(data, new_limit, unit_size, context);
    }

    *limit = new_limit;

    return data;
}

void reverse_array_(void *data, s64 limit, s64 count, u64 unit_size, Memory_context *context)
// Reverse an array's data in place.
{
    assert(data);
    assert(context);

    // We can't reverse arrays with 0 or 1 elements.
    if (count <= 1)  return;

    // We need a temporary buffer the size of one element in the array. If the array has any spare
    // space after its data, we'll use that. If not, we'll allocate a temporary buffer, though we
    // won't bother to free it later. |Memory
    void *tmp;
    if (count < limit)  tmp = (u8 *)data + count*unit_size;
    else                tmp = alloc(1, unit_size, context);

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

void array_unordered_remove_by_index_(void *data, s64 *count, u64 unit_size, s64 index_to_remove)
// Remove the item by replacing it with the last item in the array. We rely on this behaviour because it
// means we can remove items from an array we're iterating over as long as we go from back to front.
{
    assert(0 <= index_to_remove && index_to_remove < *count);

    u8 *item_to_remove = (u8 *)data + index_to_remove*unit_size;
    u8 *last_item      = (u8 *)data + (*count-1)*unit_size;

    if (item_to_remove != last_item) {
        memcpy(item_to_remove, last_item, unit_size);
    }

    memset(last_item, 0, unit_size);//|Speed.

    *count -= 1;
}
