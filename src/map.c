#include "map.h"

// The below work as long as 1 < BITS < the number of bits in UINT. Otherwise it's undefined behaviour.
#define RotateLeft(UINT, BITS)   (((UINT) << (BITS)) | ((UINT) >> (8*sizeof(UINT) - (BITS))))
#define RotateRight(UINT, BITS)  (((UINT) >> (BITS)) | ((UINT) << (8*sizeof(UINT) - (BITS))))

u64 hash_seed = 0x7071067811865475;

u64 hash_bytes(void *p, u64 size)
// Sean Barrett's version of SipHash. Plus it won't return zero.
{
    u64 seed = hash_seed;

    int siphash_c_rounds = 1;
    int siphash_d_rounds = 1;

    u8 *d = p;

    if (size == 4) {
        u32 hash = d[0] | ((u32)d[1] << 8) | ((u32)d[2] << 16) | ((u32)d[3] << 24);
        hash ^= seed;
        hash = (hash ^ 61) ^ (hash >> 16);
        hash = hash + (hash << 3);
        hash = hash ^ (hash >> 4);
        hash = hash * 0x27d4eb2d;
        hash ^= seed;
        hash = hash ^ (hash >> 15);
        u64 hash64 = (((u64)hash << 16 << 16) | hash) ^ seed;
        return (hash64) ? hash64 : 1;
    }
    if (size == 8 && sizeof(u64) == 8) {
        u64 hash = d[0] | ((u64)d[1] << 8) | ((u64)d[2] << 16) | ((u64)d[3] << 24);
        hash |= ((u64)d[4] | ((u64)d[5] << 8) | ((u64)d[6] << 16) | ((u64)d[7] << 24)) << 16 << 16;
        hash ^= seed;
        hash = (~hash) + (hash << 21);
        hash ^= RotateRight(hash, 24);
        hash *= 265;
        hash ^= RotateRight(hash, 14);
        hash ^= seed;
        hash *= 21;
        hash ^= RotateRight(hash, 28);
        hash += (hash << 31);
        hash = (~hash) + (hash << 18);
        return (hash) ? hash : 1;
    }

    u64 v0 = ((((u64)0x736f6d65 << 16) << 16) + 0x70736575) ^  seed;
    u64 v1 = ((((u64)0x646f7261 << 16) << 16) + 0x6e646f6d) ^ ~seed;
    u64 v2 = ((((u64)0x6c796765 << 16) << 16) + 0x6e657261) ^  seed;
    u64 v3 = ((((u64)0x74656462 << 16) << 16) + 0x79746573) ^ ~seed;

#define SipRound() \
    do { \
        v0 += v1;  v1 = RotateLeft(v1, 13);  v1 ^= v0;  v0 = RotateLeft(v0, 8*sizeof(u8)/2); \
        v2 += v3;  v3 = RotateLeft(v3, 16);  v3 ^= v2;                                       \
        v2 += v1;  v1 = RotateLeft(v1, 17);  v1 ^= v2;  v2 = RotateLeft(v2, 8*sizeof(u8)/2); \
        v0 += v3;  v3 = RotateLeft(v3, 21);  v3 ^= v0;                                       \
    } while (0)

    u64 data, i;
    for (i = 0; i+sizeof(u64) <= size; i+=sizeof(u64), d+=sizeof(u64)) {
        data = d[0] | ((u64)d[1] << 8) | ((u64)d[2] << 16) | ((u64)d[3] << 24);
        data |= ((u64)d[4] | ((u64)d[5] << 8) | ((u64)d[6] << 16) | ((u64)d[7] << 24)) << 16 << 16;
        v3 ^= data;
        for (int j = 0; j < siphash_c_rounds; j++)  SipRound();
        v0 ^= data;
    }
    data = size << (8*sizeof(u8)-8);

    switch (size - i) {
      case 7:  data |= ((u64)d[6] << 24) << 24; // fall through
      case 6:  data |= ((u64)d[5] << 20) << 20; // fall through
      case 5:  data |= ((u64)d[4] << 16) << 16; // fall through
      case 4:  data |= (d[3] << 24); // fall through
      case 3:  data |= (d[2] << 16); // fall through
      case 2:  data |= (d[1] << 8); // fall through
      case 1:  data |= d[0]; // fall through
      case 0:  break;
    }

    v3 ^= data;
    for (int j = 0; j < siphash_c_rounds; j++)  SipRound();
    v0 ^= data;
    v2 ^= 0xff;
    for (int j = 0; j < siphash_d_rounds; j++)  SipRound();
    v0 = v1^v2^v3;
    return (v0) ? v0 : 1;
}

u64 hash_string(char *string)
// Thomas Wang's mix function, via Sean Barrett. Also won't return zero.
{
    u64 seed = hash_seed;
    u64 hash = seed;
    while (*string)  hash = RotateLeft(hash, 9) + (u8)*string++;
    hash ^= seed;
    hash = (~hash) + (hash << 18);
    hash ^= hash ^ RotateRight(hash, 31);
    hash = hash * 21;
    hash ^= hash ^ RotateRight(hash, 11);
    hash += (hash << 6);
    hash ^= RotateRight(hash, 22);
    hash += seed;
    return (hash) ? hash : 1;
}

bool init_map_if_needed(void **keys, void **vals, s64 *count, s64 *limit, u64 key_size, u64 val_size, Hash_bucket **buckets, s64 *num_buckets, Memory_context *context)
// Return true if the map needed initting.
{
    if (*keys)  return false;

    // Initialise the map.
    s64 INITIAL_KV_LIMIT    = 8; // Limit on key/value pairs. Includes the first one which is reserved, accessed as keys[-1] and vals[-1].
    s64 INITIAL_NUM_BUCKETS = 8;

    *limit = INITIAL_KV_LIMIT;
    *keys  = alloc(*limit, key_size, context); //|Speed: We should allocate the keys and vals arrays together, always. The only minor annoyance is making sure the vals are properly aligned.
    *vals  = alloc(*limit, val_size, context); //|

    // Reserve the first key/value pair. keys[-1] will be used as temporary storage for a key we're operating on with Get(), Set() or Delete().
    // vals[-1] will store the default value for *Get() to return if the requested key is not present.
    //
    // This means that when you use *Get() with an incorrect key, the result is a zeroed-out value of the same type as map's values.
    // We rely on this behaviour in application code. Use SetDefault() to change the default value returned on a per-map basis.
    // The default default will always be the zeroed-out value.
    memset(*vals, 0, val_size);
    *keys = (u8 *)*keys + key_size;
    *vals = (u8 *)*vals + val_size;

    *num_buckets = INITIAL_NUM_BUCKETS;
    *buckets     = New(*num_buckets, Hash_bucket, context);

    return true;
}

void grow_map_if_needed(void **keys, void **vals, s64 *count, s64 *limit, u64 key_size, u64 val_size, Hash_bucket **buckets, s64 *num_buckets, Memory_context *context)
{
    bool is_new_map = init_map_if_needed(keys, vals, count, limit, key_size, val_size, buckets, num_buckets, context);

    if (is_new_map)  return;

    if (*count >= *limit-1) {
        // We're out of room in the key/value arrays.
        *keys = (u8 *)*keys - key_size;
        *vals = (u8 *)*vals - val_size;
        *keys = resize(*keys, 2*(*limit), key_size, context);
        *vals = resize(*vals, 2*(*limit), val_size, context);
        *keys = (u8 *)*keys + key_size;
        *vals = (u8 *)*vals + val_size;

        *limit *= 2;
    }

    if (*count >= *num_buckets/4*3) {
        // More than 3/4 of the buckets are used.
        Hash_bucket *new_buckets = New(2*(*num_buckets), Hash_bucket, context);

        for (s64 old_index = 0; old_index < *num_buckets; old_index++) {
            if (!(*buckets)[old_index].hash)  continue;

            s64 new_index = (*buckets)[old_index].hash % (2*(*num_buckets));
            while (new_buckets[new_index].hash) {
                new_index -= 1;
                if (new_index < 0)  new_index += 2*(*num_buckets);
            }
            new_buckets[new_index] = (*buckets)[old_index];
        }
        dealloc(*buckets, context);
        *buckets = new_buckets;
        *num_buckets *= 2;
    }
}

s64 set_key(void *keys, s64 *count, u64 key_size, Hash_bucket *buckets, s64 num_buckets, Memory_context *context, bool binary_mode)
// Assume the key to set is stored in map->keys[-1]. Add the key to the map's hash table if it
// wasn't already there and return the key's index in the map->keys array.
{
    assert(context);
    assert(*count < num_buckets); // Assume empty buckets exist so the while loops below aren't infinite loops.

    if (!binary_mode) {
        // The key is a C-style string.
        char *key = ((char **)keys)[-1];
        u64  hash = hash_string(key);
        s64 bucket_index = hash % num_buckets;
        while (true) {
            if (!buckets[bucket_index].hash) {
                // The bucket is empty. We'll take it.
                s64 kv_index = *count;
                ((char **)keys)[kv_index] = copy_string(key, context);
                buckets[bucket_index].hash  = hash;
                buckets[bucket_index].index = kv_index;
                *count += 1;
                return kv_index;
            }
            if (buckets[bucket_index].hash == hash) {
                // The hashes match. Make sure the keys match.
                s64 kv_index = buckets[bucket_index].index;
                if (!strcmp(key, ((char **)keys)[kv_index]))  return kv_index;
            }
            bucket_index -= 1;
            if (bucket_index < 0)  bucket_index += num_buckets;
        }
    } else {
        // The key is binary.
        void *key = (u8 *)keys - key_size; // key = map->keys[-1]
        u64  hash = hash_bytes(key, key_size);
        s64 bucket_index = hash % num_buckets;
        while (true) {
            if (!buckets[bucket_index].hash) {
                // The bucket is empty. We'll take it.
                s64 kv_index = *count;
                memcpy((u8 *)keys + kv_index*key_size, key, key_size);
                buckets[bucket_index].hash  = hash;
                buckets[bucket_index].index = kv_index;
                *count += 1;
                return kv_index;
            }
            if (buckets[bucket_index].hash == hash) {
                // The hashes match. Make sure the keys match.
                s64 kv_index = buckets[bucket_index].index;
                if (!memcmp(key, (u8 *)keys + kv_index*key_size, key_size))  return kv_index;
            }
            bucket_index -= 1;
            if (bucket_index < 0)  bucket_index += num_buckets;
        }
    }
}

s64 get_bucket_index(void *keys, u64 key_size, Hash_bucket *buckets, s64 num_buckets, bool binary_mode)
{
    if (!binary_mode) {
        // The key is a C-style string.
        char *key = ((char **)keys)[-1];
        u64  hash = hash_string(key);
        s64 bucket_index = hash % num_buckets;
        while (true) {
            if (!buckets[bucket_index].hash)  return -1;

            if (buckets[bucket_index].hash == hash) {
                // The hashes match. Make sure the keys match.
                s64 kv_index = buckets[bucket_index].index;
                if (!strcmp(key, ((char **)keys)[kv_index]))  return bucket_index;
            }
            bucket_index -= 1;
            if (bucket_index < 0)  bucket_index += num_buckets;
        }
    } else {
        // The key is binary.
        void *key = (u8 *)keys - key_size; // key = map->keys[-1]
        u64  hash = hash_bytes(key, key_size);
        s64 bucket_index = hash % num_buckets;
        while (true) {
            if (!buckets[bucket_index].hash)  return -1;

            if (buckets[bucket_index].hash == hash) {
                // The hashes match. Make sure the keys match.
                s64 kv_index = buckets[bucket_index].index;
                if (!memcmp(key, (u8 *)keys + kv_index*key_size, key_size))  return bucket_index;
            }
            bucket_index -= 1;
            if (bucket_index < 0)  bucket_index += num_buckets;
        }
    }
}

bool delete_key(void *keys, void *vals, s64 *count, u64 key_size, u64 val_size, Hash_bucket *buckets, s64 num_buckets, Memory_context *context, bool binary_mode)
// Return true if the key existed.
{
    s64 bucket_index = get_bucket_index(keys, key_size, buckets, num_buckets, binary_mode);
    if (bucket_index < 0)  return false;

    s64 kv_index = buckets[bucket_index].index;

    // Delete the bucket. This is algorithm 6.4R from Knuth volume 3. Note that the errata for the
    // second edition of this book correct a significant bug in this algorithm. Step R4 should end
    // with "return to step R1", not "return to step R2".
    {
        s64 i = bucket_index;
        while (true) {
            buckets[i] = (Hash_bucket){0};

            s64 j = i;
            while (true) {
                i -= 1;
                if (i < 0)  i += num_buckets;

                if (!buckets[i].hash)  goto bucket_deleted;

                s64 r = buckets[i].hash % num_buckets;

                if (i <= r && r < j)  continue;
                if (r < j && j < i)   continue;
                if (j < i && i <= r)  continue;

                buckets[j] = buckets[i];
                break;
            }
        }
    }
bucket_deleted:

    // Delete the key and value.
    {
        // If it's a string-mode map, delete the copy we made of the key.
        if (!binary_mode)  dealloc(((char **)keys)[kv_index], context);

        if (kv_index < *count-1) {
            // Copy the final kv pair into the places of the pair we're deleting.
            memcpy((u8 *)keys+key_size*kv_index, (u8 *)keys+key_size*(*count-1), key_size);
            memcpy((u8 *)vals+val_size*kv_index, (u8 *)vals+val_size*(*count-1), val_size);

            // Update the hash table with the new index of the pair that we moved.
            u64 hash = (!binary_mode)
                ? hash_string(((char **)keys)[*count-1])
                : hash_bytes((u8 *)keys+key_size*(*count-1), key_size);

            s64 bucket_index = hash % num_buckets;
            while (true) {
                if (buckets[bucket_index].index == *count-1) {
                    buckets[bucket_index].index = kv_index;
                    break;
                }
                assert(buckets[bucket_index].hash);
                bucket_index -= 1;
                if (bucket_index < 0)  bucket_index += num_buckets;
            }
        }
        // Delete the final kv pair.
        memset((u8 *)keys+key_size*(*count-1), 0, key_size);
        memset((u8 *)vals+val_size*(*count-1), 0, val_size);
    }

    *count -= 1;

    return true;
}
