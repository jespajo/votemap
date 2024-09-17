//|Todo: Move documentation from new_map() to this header file.

#ifndef MAP_H_INCLUDED
#define MAP_H_INCLUDED

#include "context.h"

struct Hash_bucket {
    u64 hash;
    s64 index;
};
typedef struct Hash_bucket Hash_bucket;

#define Map(KEY_TYPE, VAL_TYPE)    \
    struct {                       \
        KEY_TYPE  *keys;           \
        VAL_TYPE  *vals;           \
        s64        count;          \
        s64        limit;          \
                                   \
        Hash_bucket *buckets;      \
        s64          num_buckets;  \
                                   \
        Memory_context *context;   \
                                   \
        bool string_mode;          \
        u16  key_size;             \
        u16  val_size;             \
        s64  i;                    \
    }

#define Dict(VAL_TYPE)  Map(char *, VAL_TYPE)

typedef Dict(char *)  string_dict;

u64 hash_bytes(void *p, u64 size);
u64 hash_string(char *string);
void *new_map(Memory_context *context, u64 key_size, u64 val_size, bool string_mode);
s64 set_key(void *map);
s64 get_bucket_index(void *map);
bool delete_key(void *map);

#define NewMap(MAP, CONTEXT) \
    (new_map((CONTEXT), sizeof((MAP)->keys[0]), sizeof((MAP)->vals[0]), false))

#define NewDict(MAP, CONTEXT) \
    (new_map((CONTEXT), sizeof((MAP)->keys[0]), sizeof((MAP)->vals[0]), true))

#define Set(MAP, KEY) \
    ((MAP)->keys[-1] = (KEY), \
     (MAP)->i = set_key(MAP), \
     &(MAP)->vals[(MAP)->i])

#define Get(MAP, KEY) \
    ((MAP)->keys[-1] = (KEY), \
     (MAP)->i = get_bucket_index(MAP), \
     &(MAP)->vals[(MAP)->i < 0 ? -1 : (MAP)->buckets[(MAP)->i].index])

#define Delete(MAP, KEY) \
    ((MAP)->keys[-1] = (KEY), \
     delete_key(MAP))

#define SetDefault(MAP, VALUE)  ((MAP)->vals[-1] = (VALUE))

#define IsSet(MAP, KEY) \
    ((MAP)->keys[-1] = (KEY), \
     get_bucket_index(MAP) >= 0)

#endif // MAP_H_INCLUDED
