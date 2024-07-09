// alloc() and resize() do not currently assert that their Memory_Context arguments are not NULL.
// This should probably change, because it's too easy to accidentally create a new context:
//
//      u8_array array = {0};
//      *Add(&array) = 1; // |Leak! A memory context for the array was created in the background.
//
// But memory leaks created in this way aren't hard to find: put a breakpoint in new_context() for
// when parent == NULL. Generally there should only be one hit for the top-level context.

// Some kind of visualisation would be really helpful.

// At the moment we frequently operate on the arrays of blocks by deleting a block with
// delete_block() and then adding blocks with add_free_block() or add_used_block(). Each of these
// functions leaves the array sorted, so if we delete the first block in the array, it will shift
// all subsequent blocks to the left, and if we then insert a new block at the start of the array,
// it will shift everything back to the right. There is room for improvement here. |Speed

#include <stdlib.h>
#include <string.h>

#include "context.h"

void *double_if_needed(void *data, s64 *limit, s64 count, u64 unit_size, Memory_Context *context)
// Make sure there's room for at least one more item in the array. If reallocation occurs, modify
// *limit and return a pointer to the new data. Otherwise return `data`.
//
// This function modifies *limit, so why don't we just make the first parameter `void **data` and
// get the function to modify *data as well? The reason is that the compiler lets us implicitly cast
// e.g. `int *` to `void *`, but not `int **` to `void **`.
{
    s64 INITIAL_LIMIT = 4; // If the array is unitialised, how many units to make room for in the first allocation.

    if (!data) {
        // The array needs to be initialised.
        assert(*limit == 0 && count == 0);

        *limit = INITIAL_LIMIT;
        data   = alloc(context, *limit, unit_size);
    } else if (count >= *limit) {
        // The array needs to be resized.
        assert(count == *limit);

        // Make sure we only use this function for arrays that should increase in powers of two.
        // This assert will trip if we use array_reserve() to allocate a non-power-of-two number of bytes
        // and the array needs to be resized later.
        assert(is_power_of_two(*limit));

        *limit *= 2;
        data    = resize(context, data, *limit, unit_size);
    }

    return data;
}

static s64 get_free_block_index(Memory_Context *context, u64 size, u8 *data)
// Return the index of the block if it exists or the index where it would be inserted.
{
    s64 i = 0;
    s64 j = context->free_count-1;

    while (i <= j) {
        s64 mid = (i + j)/2;
        Memory_Block *block = &context->free_blocks[mid];

        s64 cmp = size - block->size;
        if (!cmp) {
            cmp = data - block->data;
            if (!cmp)  return mid;
        }

        if (cmp < 0)  j = mid-1;
        else          i = mid+1;
    }

    return i;
}

static Memory_Block *find_free_block(Memory_Context *context, u64 size, u8 *data)
{
    s64 index = get_free_block_index(context, size, data);

    if (index < context->free_count) {
        Memory_Block *block = &context->free_blocks[index];

        if (block->data == data && block->size == size)  return block;
    }

    return NULL;
}

static s64 get_used_block_index(Memory_Context *context, u8 *data)
// Return the index of the block if it exists or the index where it would be inserted.
{
    s64 i = 0;
    s64 j = context->used_count-1;

    while (i <= j) {
        s64 mid = (i + j)/2;
        Memory_Block *block = &context->used_blocks[mid];

        s64 cmp = data - block->data;
        if (!cmp)  return mid;

        if (cmp < 0)  j = mid-1;
        else          i = mid+1;
    }

    return i;
}

static Memory_Block *find_used_block(Memory_Context *context, u8 *data)
{
    s64 index = get_used_block_index(context, data);

    if (index < context->used_count) {
        Memory_Block *block = &context->used_blocks[index];

        if (block->data == data)  return block;
    }

    return NULL;
}

static bool in_range(void *zero, void *x, void *zero_plus_count)
{
    return (u64)zero <= (u64)x && (u64)x < (u64)zero_plus_count;
}

#ifndef DEBUG_MEMORY_CONTEXT
#define assert_context_makes_sense(C)  ((void)0)
#else
static bool are_in_free_order(Memory_Block *blocks, s64 count)
{
    for (s64 i = 0; i < count-1; i++) {
        if (blocks[i].size > blocks[i+1].size)   return false;

        if (blocks[i].size == blocks[i+1].size) {
            if (blocks[i].data > blocks[i+1].data)  return false;
        }
    }
    return true;
}

static bool are_in_used_order(Memory_Block *blocks, s64 count)
{
    for (s64 i = 0; i < count-1; i++) {
        if (blocks[i].data > blocks[i+1].data)  return false;
    }
    return true;
}

static void assert_context_makes_sense(Memory_Context *context)
{
    Memory_Context *c = context;

    assert(are_in_free_order(c->free_blocks, c->free_count));
    assert(are_in_used_order(c->used_blocks, c->used_count));

    s64 num_free = 0;
    s64 num_used = 0;

    // For each buffer, enumerate all blocks to make sure they look right.
    for (s64 buffer_index = 0; buffer_index < c->buffer_count; buffer_index++) {
        Memory_Block *buffer = &c->buffers[buffer_index];
        u8 *buffer_end = buffer->data + buffer->size;

        u8 *data = buffer->data;

        Memory_Block *last_used = find_used_block(c, data);
        assert(last_used->data == buffer->data);
        assert(last_used->size == 1);
        assert(last_used->sentinel);

        data     += 1;
        num_used += 1;

        while (data < buffer_end) {
            Memory_Block *used_block = find_used_block(c, data);
            if (used_block) {
                data     += used_block->size;
                num_used += 1;
                last_used = used_block;

                if (used_block->sentinel)  break;
                continue;
            }

            s64 last_used_index = last_used - c->used_blocks;
            assert(last_used_index < c->used_count-1);

            u8 *next_data = (last_used+1)->data;
            assert(next_data < buffer_end);

            s64 free_size = next_data - data;
            Memory_Block *free_block = find_free_block(c, free_size, data);
            assert(free_block);

            data     += free_block->size;
            num_free += 1;
        }

        assert(last_used->sentinel);
        assert(data == buffer_end);
    }

    assert(num_free == c->free_count);
    assert(num_used == c->used_count);
}
#endif // DEBUG_MEMORY_CONTEXT

static Memory_Block *add_free_block(Memory_Context *context, void *data, u64 size)
{
    Memory_Context *c = context;

    assert(data);
    assert(size);

    c->free_blocks = double_if_needed(c->free_blocks, &c->free_limit, c->free_count, sizeof(*c->free_blocks), c->parent);

    s64 insert_index = get_free_block_index(c, size, data);

    // Make room by shifting everything after block_index right one.
    for (s64 i = c->free_count; i > insert_index; i--)  c->free_blocks[i] = c->free_blocks[i-1];

    c->free_blocks[insert_index] = (Memory_Block){.data=data, .size=size};
    c->free_count += 1;

    return &c->free_blocks[insert_index];
}

static Memory_Block *add_used_block(Memory_Context *context, u8 *data, u64 size)
{
    Memory_Context *c = context;

    assert(data && size);

    c->used_blocks = double_if_needed(c->used_blocks, &c->used_limit, c->used_count, sizeof(*c->used_blocks), c->parent);

    s64 insert_index = get_used_block_index(c, data);

    // Make room by shifting everything after block_index right one.
    for (s64 i = c->used_count; i > insert_index; i--)  c->used_blocks[i] = c->used_blocks[i-1];

    c->used_blocks[insert_index] = (Memory_Block){.data=data, .size=size};
    c->used_count += 1;

    return &c->used_blocks[insert_index];
}

static Memory_Block *grow_context(Memory_Context *context, u64 size)
// Add a new buffer of at least `size` bytes to a context. Return the associated free block. It
// might seem like a good idea to make `size` a power of two. But in this case, the actual size of
// the buffer created will be double `size`. The reason is that we have to reserve the first and
// last byte of the buffer as sentinels. Reserving the first byte effectively takes the first 16 out
// of action for large allocations due to alignment.
{
    u64 FIRST_BUFFER_SIZE = 8192;

    Memory_Context *c = context;

    Memory_Block buffer = {0};

    // Our idea here is to double the size of each additional buffer that we add to a context.
    // We think this will help with fragmentation (particularly with child contexts) and reduce
    // the number of allocations from the OS.
    if (!c->buffer_count)  buffer.size = FIRST_BUFFER_SIZE;
    else                   buffer.size = 2 * c->buffers[c->buffer_count-1].size;

    // Keep doubling until we know we have room for an allocation of length `size`. We need to be
    // conservative here. Even though the first sentinel only takes the first byte of a new buffer,
    // it effectively prevents large allocations for the first 16 bytes due to alignment issues.
    // We won't need to take anything from size once we remove sentinels. |Memory |Hack
    while ((buffer.size - 16 - 1) < size)  buffer.size *= 2;

    buffer.data = alloc(c->parent, 1, buffer.size);

    c->buffers = double_if_needed(c->buffers, &c->buffer_limit, c->buffer_count, sizeof(*c->buffers), c->parent);

    c->buffers[c->buffer_count] = buffer;
    c->buffer_count += 1;

    // Reserve the buffer's first and last bytes as sentinels.
    add_used_block(c, buffer.data,               1)->sentinel = true;
    add_used_block(c, buffer.data+buffer.size-1, 1)->sentinel = true;

    Memory_Block *free_block = add_free_block(c, buffer.data+1, buffer.size-2);

    assert_context_makes_sense(c);

    return free_block;
}

static void delete_block(Memory_Block *blocks, s64 *count, Memory_Block *block)
// Remove a block from an array of blocks. Decrement *count.
{
    s64 index = block - blocks;
    assert(0 <= index && index < *count);

    // Move subsequent blocks left one, then delete the final block.
    for (s64 i = index+1; i < *count; i++)  blocks[i-1] = blocks[i];

    blocks[*count-1] = (Memory_Block){0};

    *count -= 1;
}

static u64 get_alignment(u64 unit_size)
{
    u64 max_align = 16;
    s64 alignment = (unit_size < max_align) ? round_up_pow2(unit_size) : max_align;

    return alignment;
}

static u64 get_padding(u8 *data, u64 alignment)
// Get alignment padding size in bytes.
{
    u64 gap     = (u64)data % alignment;
    u64 padding = (gap) ? alignment - gap : 0;

    return padding;
}

static Memory_Block *alloc_block(Memory_Context *context, Memory_Block *free_block, u64 size, u64 alignment)
// Return a pointer to the newly used block on success. Return NULL if there's not room due to alignment.
{
    Memory_Context *c = context;

    assert(in_range(c->free_blocks, free_block, c->free_blocks+c->free_count));
    assert(free_block->size >= size); // This is not necessary (we return NULL in this case) but otherwise why are you calling this function?

    u64 padding = get_padding(free_block->data, alignment);

    if (free_block->size < padding)         return NULL;
    if (free_block->size - padding < size)  return NULL;

    u64 remaining = free_block->size - padding - size;

    // |Speed: For now we're just going to add and delete the relevant blocks one at a time.

    u8 *free_data = free_block->data;

    delete_block(c->free_blocks, &c->free_count, free_block);

    if (padding)  add_free_block(c, free_data, padding);

    Memory_Block *used_block = add_used_block(c, free_data+padding, size);

    if (remaining) {
        u8 *next_free = used_block->data + used_block->size;
        add_free_block(c, next_free, remaining);
    }

    assert_context_makes_sense(c);

    return used_block;
}

static Memory_Block *dealloc_block(Memory_Context *context, Memory_Block *used_block)
// Return coalesced free block.
{
    Memory_Context *c = context;

    assert(in_range(c->used_blocks, used_block, c->used_blocks+c->used_count));
    assert(!used_block->sentinel); // |Cleanup: If we're only going to do this during debug builds, we should probably not bother with the `sentinel` flag and we can make the whole check more expensive.

    // |Speed: For now we're just going to add and delete the relevant blocks one at a time.

    u8 *freed_data = used_block->data;
    u64 freed_size = used_block->size;
    s64 used_index = used_block - c->used_blocks;

    // These asserts should always be true due to the presence of sentinels. If they are untrue
    // the pointer arithmetic used below to check the neighbouring used blocks is invalid.
    assert(used_index > 0);
    assert(used_index < c->used_count-1);

    // See if we should coalesce with the left neighbour.
    {
        Memory_Block *prev_used = used_block - 1;
        u8 *prev_used_end = prev_used->data + prev_used->size;
        s64 distance = used_block->data - prev_used_end;
        if (distance) {
            Memory_Block *left = find_free_block(c, distance, prev_used_end);
            freed_data -= left->size;
            freed_size += left->size;
            delete_block(c->free_blocks, &c->free_count, left);
        }
    }
    // See if we should coalesce with the right neighbour.
    {
        Memory_Block *next_used = used_block + 1;
        u8 *used_block_end = used_block->data + used_block->size;
        s64 distance = next_used->data - used_block_end;
        if (distance) {
            Memory_Block *right = find_free_block(c, distance, used_block_end);
            freed_size += right->size;
            delete_block(c->free_blocks, &c->free_count, right);
        }
    }

    delete_block(c->used_blocks, &c->used_count, used_block);

    Memory_Block *freed_block = add_free_block(c, freed_data, freed_size);

    assert_context_makes_sense(c);

    return freed_block;
}

static Memory_Block *resize_block(Memory_Context *context, Memory_Block *used_block, u64 new_size)
// Return the resized block if success, or NULL if there isn't room in a contiguous free block; in that case the caller will have to call alloc_block and dealloc_block.
{
    Memory_Context *c = context;

    assert(in_range(c->used_blocks+1, used_block, c->used_blocks+c->used_count-1));

    // Don't bother shrinking. (Maybe one day.)
    if (new_size <= used_block->size)  return used_block;

    Memory_Block *next_used = used_block + 1;

    u8 *end_of_used_block = used_block->data + used_block->size;
    u64 size_avail_after  = next_used->data - end_of_used_block;

    // Return NULL if there's not enough room after the block.
    // |Todo: Maybe also check if there's room *before* the used block. If so, it would probably be
    // better than telling the caller to reallocate. We'd have to pass the unit size to this function
    // or just be super conservative about alignment.
    if (used_block->size + size_avail_after < new_size)  return NULL;

    // We can expand this block.
    Memory_Block *free_neighbour = find_free_block(c, size_avail_after, end_of_used_block);
    assert(free_neighbour);

    u64 extra_needed    = new_size - used_block->size;
    u64 remaining_after = free_neighbour->size - extra_needed;

    used_block->size = new_size;
    u8 *new_end_of_used_block = used_block->data + new_size;

    delete_block(c->free_blocks, &c->free_count, free_neighbour);

    if (remaining_after)  add_free_block(c, new_end_of_used_block, remaining_after);

    assert_context_makes_sense(c);

    return used_block;
}

void *alloc(Memory_Context *context, s64 count, u64 unit_size)
{
    Memory_Context *c = context;

    assert(count);
    assert(unit_size);

    u64 size = count * unit_size;

    if (!context) {
        void *memory = malloc(size);
        if (!memory)  Fatal("malloc failed.");
        return memory;
    }

    u64 alignment = get_alignment(unit_size);

    // See if there's an already-free block of the right size.
    for (s64 i = get_free_block_index(c, size, NULL); i < c->free_count; i++) {
        Memory_Block *free_block = &c->free_blocks[i];
        Memory_Block *used_block = alloc_block(c, free_block, size, alignment);

        if (used_block)  return used_block->data;
    }

    // We weren't able to find a block big enough in the free list.
    // We need to add a new buffer to the context.
    Memory_Block *free_block = grow_context(context, size);

    return alloc_block(c, free_block, size, alignment)->data;
}

void *zero_alloc(Memory_Context *context, s64 count, u64 unit_size)
{
    void *data = alloc(context, count, unit_size);

    memset(data, 0, count*unit_size);

    return data;
}

void dealloc(Memory_Context *context, void *data)
{
    assert(data);

    if (!context) {
        free(data);
        return;
    }

    Memory_Block *used_block = find_used_block(context, data);
    assert(used_block);

    dealloc_block(context, used_block);
}

void *resize(Memory_Context *context, void *data, s64 new_limit, u64 unit_size)
{
    Memory_Context *c = context;

    assert(data);
    assert(new_limit);
    assert(unit_size);

    u64 new_size = new_limit * unit_size;

    if (!context) {
        void *new_data = realloc(data, new_size);
        if (!new_data)  Fatal("realloc failed.");
        return new_data;
    }

    Memory_Block *used_block = find_used_block(c, data);
    assert(used_block);

    Memory_Block *resized = resize_block(context, used_block, new_size);
    if (resized)  return resized->data;

    // We can't resize the block in place. We'll have to move it.

    s64 old_index = used_block - c->used_blocks;

    void *new_data = alloc(context, new_limit, unit_size);

    // `alloc` may have made an unknown number of allocations or reallocations. Which means the used
    // block's index might have changed and the whole array of used blocks might have moved. We need
    // to find the old used block in this case so we can deallocate it.
    used_block = &c->used_blocks[old_index];
    if ((u8 *)data < used_block->data) {
        do used_block -= 1;  while (data != used_block->data);
    } else if ((u8 *)data > used_block->data) {
        do used_block += 1;  while (data != used_block->data);
    }

    u64 copy_size = Min(used_block->size, new_size);
    memcpy(new_data, data, copy_size);

    dealloc_block(context, used_block);

    return new_data;
}

Memory_Context *new_context(Memory_Context *parent)
{
    Memory_Context *context = New(Memory_Context, parent);

    context->parent = parent;

    return context;
}

void free_context(Memory_Context *context)
{
    Memory_Context *c = context;

    // This automatically frees all child contexts because they all allocated from this parent.

    for (s64 i = 0; i < c->buffer_count; i++) {
        if (c->buffers[i].data)  dealloc(c->parent, c->buffers[i].data);
    }

    if (c->buffers)      dealloc(c->parent, c->buffers);
    if (c->free_blocks)  dealloc(c->parent, c->free_blocks);
    if (c->used_blocks)  dealloc(c->parent, c->used_blocks);

    dealloc(c->parent, c);
}

void reset_context(Memory_Context *context)
{
    Memory_Context *c = context;

    c->free_count = 0;
    c->used_count = 0;

    for (s64 i = 0; i < c->buffer_count; i++) {
        u8 *data = c->buffers[i].data;
        u64 size = c->buffers[i].size;

        // Add the sentinels.
        add_used_block(c, data,        1)->sentinel = true;
        add_used_block(c, data+size-1, 1)->sentinel = true;

        add_free_block(c, data+1, size-2);
    }

    assert_context_makes_sense(c);
}

char *copy_string(char *source, Memory_Context *context)
{
    int length = strlen(source);
    char *copy = alloc(context, length+1, sizeof(char));
    memcpy(copy, source, length);
    copy[length] = '\0';
    return copy;
}
