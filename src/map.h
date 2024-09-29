#ifndef MAP_H_INCLUDED
#define MAP_H_INCLUDED

#include "context.h"

struct Hash_bucket {
    u64 hash;
    s64 index;
};

//
// The .keys and .vals members are both pointers to the *second* items in allocated arrays. The first items,
// .keys[-1] and .vals[-1], are both used for special purposes. .keys[-1] is temporary storage for whatever key
// we're currently operating on. .vals[-1] stores the default value that *Get() returns when the key is not found.
// We do it this way because explicit assignment to .keys[-1] and .vals[-1] is how the macros are type-safe.
//
// .limit refers to the number of items in each of the key-value arrays, including the skipped first member.
//
// .string_mode is true for dicts, which are maps where the keys are zero-terminated strings. Dicts are different
// from normal maps in two ways: they make an internal copy of their keys and they use a different hashing function.
//
// .i is used for temporary storage by two of our macros. There is a subtle difference in its two uses.
// Get() uses it to store the bucket index. Set() uses it to store the key-value index.
//
#define Map(KEY_TYPE, VAL_TYPE)         \
    struct {                            \
        KEY_TYPE       *keys;           \
        VAL_TYPE       *vals;           \
        s64             count;          \
        s64             limit;          \
                                        \
        Hash_bucket    *buckets;        \
        s64             num_buckets;    \
                                        \
        Memory_context *context;        \
                                        \
        bool            string_mode;    \
        s64             i;              \
    }

#define Dict(VAL_TYPE)  Map(char *, VAL_TYPE)

typedef struct Hash_bucket Hash_bucket;
typedef Dict(char *)       string_dict;
typedef Dict(int)          int_dict;

u64 hash_bytes(void *p, u64 size);
u64 hash_string(char *string);
bool init_map_if_needed(void **keys, void **vals, s64 *count, s64 *limit, u64 key_size, u64 val_size, Hash_bucket **buckets, s64 *num_buckets, Memory_context *context);
void grow_map_if_needed(void **keys, void **vals, s64 *count, s64 *limit, u64 key_size, u64 val_size, Hash_bucket **buckets, s64 *num_buckets, Memory_context *context);
s64 set_key(void *keys, s64 *count, u64 key_size, Hash_bucket *buckets, s64 num_buckets, Memory_context *context, bool string_mode);
s64 get_bucket_index(void *keys, u64 key_size, Hash_bucket *buckets, s64 num_buckets, bool string_mode);
bool delete_key(void *keys, void *vals, s64 *count, u64 key_size, u64 val_size, Hash_bucket *buckets, s64 num_buckets, Memory_context *context, bool string_mode);

#define NewMap(MAP, CONTEXT) \
    ((MAP) = zero_alloc(1, sizeof(*MAP), (CONTEXT)), \
     (MAP)->context = (CONTEXT), \
     (MAP))

#define NewDict(MAP, CONTEXT) \
    ((MAP) = zero_alloc(1, sizeof(*MAP), (CONTEXT)), \
     (MAP)->context = (CONTEXT), \
     (MAP)->string_mode = true, \
     (MAP))

#define Set(MAP, KEY) \
    (grow_map_if_needed((void**)&(MAP)->keys, (void**)&(MAP)->vals, &(MAP)->count, &(MAP)->limit, sizeof(*(MAP)->keys), sizeof(*(MAP)->vals), &(MAP)->buckets, &(MAP)->num_buckets, (MAP)->context), \
     (MAP)->keys[-1] = (KEY), \
     (MAP)->i = set_key((MAP)->keys, &(MAP)->count, sizeof(*(MAP)->keys), (MAP)->buckets, (MAP)->num_buckets, (MAP)->context, (MAP)->string_mode), \
     &(MAP)->vals[(MAP)->i])

#define Get(MAP, KEY) \
    (init_map_if_needed((void**)&(MAP)->keys, (void**)&(MAP)->vals, &(MAP)->count, &(MAP)->limit, sizeof(*(MAP)->keys), sizeof(*(MAP)->vals), &(MAP)->buckets, &(MAP)->num_buckets, (MAP)->context), \
     (MAP)->keys[-1] = (KEY), \
     (MAP)->i = get_bucket_index((MAP)->keys, sizeof(*(MAP)->keys), (MAP)->buckets, (MAP)->num_buckets, (MAP)->string_mode), \
     &(MAP)->vals[(MAP)->i < 0 ? -1 : (MAP)->buckets[(MAP)->i].index])

#define Delete(MAP, KEY) \
    (init_map_if_needed((void**)&(MAP)->keys, (void**)&(MAP)->vals, &(MAP)->count, &(MAP)->limit, sizeof(*(MAP)->keys), sizeof(*(MAP)->vals), &(MAP)->buckets, &(MAP)->num_buckets, (MAP)->context), \
     (MAP)->keys[-1] = (KEY), \
     delete_key((MAP)->keys, (MAP)->vals, &(MAP)->count, sizeof(*(MAP)->keys), sizeof(*(MAP)->vals), (MAP)->buckets, (MAP)->num_buckets, (MAP)->context, (MAP)->string_mode))

#define SetDefault(MAP, VALUE) \
    (init_map_if_needed((void**)&(MAP)->keys, (void**)&(MAP)->vals, &(MAP)->count, &(MAP)->limit, sizeof(*(MAP)->keys), sizeof(*(MAP)->vals), &(MAP)->buckets, &(MAP)->num_buckets, (MAP)->context), \
     (MAP)->vals[-1] = (VALUE))

#define IsSet(MAP, KEY) \
    (init_map_if_needed((void**)&(MAP)->keys, (void**)&(MAP)->vals, &(MAP)->count, &(MAP)->limit, sizeof(*(MAP)->keys), sizeof(*(MAP)->vals), &(MAP)->buckets, &(MAP)->num_buckets, (MAP)->context), \
     (MAP)->keys[-1] = (KEY), \
     get_bucket_index((MAP)->keys, sizeof(*(MAP)->keys), (MAP)->buckets, (MAP)->num_buckets, (MAP)->string_mode) >= 0)

#endif // MAP_H_INCLUDED
