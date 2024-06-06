#ifndef MAP_H_INCLUDED
#define MAP_H_INCLUDED

#include "context.h"

struct Hash_Bucket {
    u64 hash;
    s64 index;
};
typedef struct Hash_Bucket Hash_Bucket;

#define Map(KEY_TYPE, VAL_TYPE)    \
    struct {                       \
        KEY_TYPE  *keys;           \
        VAL_TYPE  *vals;           \
        s64        count;          \
        s64        limit;          \
                                   \
        Hash_Bucket *buckets;      \
        s64          num_buckets;  \
                                   \
        Memory_Context *context;   \
                                   \
        bool string_mode;          \
        u16  key_size;             \
        u16  val_size;             \
        s64  i;                    \
    }

#define Dict(VAL_TYPE)  Map(char *, VAL_TYPE)

u64 hash_bytes(void *p, u64 size);
u64 hash_string(char *string);
void *new_map(Memory_Context *context, u64 key_size, u64 val_size, bool string_mode);
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

#endif // MAP_H_INCLUDED
