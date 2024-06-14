#ifndef CONTEXT_H_INCLUDED
#define CONTEXT_H_INCLUDED

#include "basic.h"

typedef struct Memory_Block   Memory_Block;
typedef struct Memory_Context Memory_Context;

struct Memory_Context {
    Memory_Context *parent;

    // Backing memory the context has allocated from its parent, or if the parent is NULL,
    // directly from the operating system.
    Memory_Block   *buffers;
    s64             buffer_count;
    s64             buffer_limit;

    // Array of free memory blocks, sorted by size then address.
    Memory_Block   *free_blocks;
    s64             free_count;
    s64             free_limit;

    // Array of allocated memory blocks, sorted by address.
    Memory_Block   *used_blocks;
    s64             used_count;
    s64             used_limit;
};

// A Memory_Context tracks three kinds of Memory_Blocks: buffers, free blocks and used blocks.
// Buffers are the backing memory. They track the allocations the context makes from its parent.
// Every byte in a context's buffers is also tracked in either a free block or a used block
// depending on whether the byte is allocated. Free blocks are sorted by size so we can quickly find
// a best fit. Used blocks are sorted by address for speedy deallocation. On deallocation, a newly
// freed block coalesces with contiguous free blocks.
//
// The `sentinel` flag only appears in the used blocks array. Sentinels are blocks of one byte
// marking the first and last byte in each backing buffer. They help with deallocation because they
// let us infer the size of the free blocks "missing" from the used blocks array (otherwise we
// wouldn't know whether gaps between neighbouring used blocks occur because there's a free block in
// between or because the used blocks are in different buffers). Sentinels behave like used blocks
// except that everything will probably break if you try to deallocate one.
//
// One day we want to stop reserving any bytes for the sentinel blocks. Ideally we could also remove
// the `sentinel` flag. Maybe the sentinels could be zero-sized. But we could no longer rely on the
// assumption that no two used blocks have the same address. However this would improve memory
// fragmentation. |Speed |Memory
struct Memory_Block {
    u8  *data;
    u64  size:     8*sizeof(u64)-1;
    bool sentinel: 1;
};

void *double_if_needed(void *data, s64 *limit, s64 count, u64 unit_size, Memory_Context *context);
void *alloc(Memory_Context *context, s64 count, u64 unit_size);
void *zero_alloc(Memory_Context *context, s64 count, u64 unit_size);
void dealloc(Memory_Context *context, void *data);
void *resize(Memory_Context *context, void *data, s64 new_limit, u64 unit_size);
Memory_Context *new_context(Memory_Context *parent);
void free_context(Memory_Context *context);
void reset_context(Memory_Context *context);
char *copy_string(char *source, Memory_Context *context);

#define New2(TYPE, CONTEXT)         (TYPE *)zero_alloc((CONTEXT), 1, sizeof(TYPE))
#define New3(COUNT, TYPE, CONTEXT)  (TYPE *)zero_alloc((CONTEXT), (COUNT), sizeof(TYPE))
#define New_(A, B, C, D, ...)       D
#define New(...)                    New_(__VA_ARGS__,New3,New2)(__VA_ARGS__)

#endif // CONTEXT_H_INCLUDED
