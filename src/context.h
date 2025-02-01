#ifndef CONTEXT_H_INCLUDED
#define CONTEXT_H_INCLUDED

#include <pthread.h>

#include "basic.h"

typedef struct Memory_block   Memory_block;
typedef struct Memory_context Memory_context;

struct Memory_block {
    u8  *data;
    u64  size;
};

struct Memory_context {
    pthread_mutex_t mutex;

    Memory_context *parent;

    // Backing memory the context has allocated from its parent (or from the operating system if the parent is NULL).
    Memory_block   *buffers;
    s64             buffer_count;
    s64             buffer_limit;

    // Array of free memory blocks, sorted by size then address.
    Memory_block   *free_blocks;
    s64             free_count;
    s64             free_limit;

    // Array of allocated memory blocks, sorted by address.
    Memory_block   *used_blocks;
    s64             used_count;
    s64             used_limit;
};

void *alloc(s64 count, u64 unit_size, Memory_context *context);
void *zero_alloc(s64 count, u64 unit_size, Memory_context *context);
void *resize(void *data, s64 new_limit, u64 unit_size, Memory_context *context);
void dealloc(void *data, Memory_context *context);
Memory_context *new_context(Memory_context *parent);
void free_context(Memory_context *context);
void reset_context(Memory_context *context);
char *copy_string(char *source, Memory_context *context);
void check_context_integrity(Memory_context *context);

//
// The macro magic below sets up the New() macro so that the first argument is optional.
// This optional argument is a count specifier. If left out, it defaults to one.
//
//     int *numbers = New(100, int, context);    // Allocate 100 contiguous ints.
//
//     Vector3 *vector = New(Vector3, context);  // Allocate one Vector3.
//
#define New2(TYPE, CONTEXT)         (TYPE *)zero_alloc(1, sizeof(TYPE), (CONTEXT))
#define New3(COUNT, TYPE, CONTEXT)  (TYPE *)zero_alloc((COUNT), sizeof(TYPE), (CONTEXT))
#define New_(A, B, C, D, ...)       D
#define New(...)                    New_(__VA_ARGS__,New3,New2)(__VA_ARGS__)

#endif // CONTEXT_H_INCLUDED
