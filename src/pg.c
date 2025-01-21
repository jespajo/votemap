#include <arpa/inet.h> // For ntohl(), which we use in get_u32_from_cell().

#include "pg.h"
#include "strings.h"
#include "system.h"

static u64 hash_query(char *query, string_array *params)
{
//|Copypasta from map.c.
// The below work as long as 1 < BITS < the number of bits in UINT. Otherwise it's undefined behaviour.
#define RotateLeft(UINT, BITS)   (((UINT) << (BITS)) | ((UINT) >> (8*sizeof(UINT) - (BITS))))

    u64 hash = hash_string(query);

    s64 num_params = (params) ? params->count : 0;
    for (s64 i = 0; i < num_params; i++) {
        u64 param_hash = hash_string(params->data[i]);

        hash ^= RotateLeft(param_hash, i % 32 + 2);
    }

    return hash;
}

static void add_s32(u8_array *array, s32 number)
// Append a 32-bit integer to an array of bytes.
{
    s64 new_count = array->count + sizeof(s32);
    if (array->limit < new_count) {
        array_reserve(array, round_up_pow2(new_count));
    }
    memcpy(array->data + array->count, &number, sizeof(s32));
    array->count = new_count;
}

static PG_result *query_database_uncached(PG_client *client, char *query, string_array *params, Memory_context *context)
// Actually query the database and parse the result.
// This function is called by query_database().
// Parameters are string literals. Cast them in your queries: `SELECT $1::int;`
{
    Memory_context *ctx = context;

    PG_result *result = New(PG_result, ctx);
    result->columns = (int_dict){.context = ctx};
    result->rows    = (u8_array3){.context = ctx};
    SetDefault(&result->columns, -1);

    // Connect to the database.
    if (!client->conn)  client->conn = PQconnectdb(client->conn_string);
    else  assert(client->keep_alive);

    // Check the connection status.
    if (PQstatus(client->conn) != CONNECTION_OK) {
        log_error("Database connection failed: %s", PQerrorMessage(client->conn));
        result = NULL;
        goto done;
    }

    // Make the query.
    PGresult *query_result; {
        int num_params = (params) ? params->count : 0;
        char const *const *param_data = (params) ? (char const *const *)params->data : NULL;

        query_result = PQexecParams(client->conn, query, num_params, NULL, param_data, NULL, NULL, 1);

        if (PQresultStatus(query_result) != PGRES_TUPLES_OK) {
            log_error("Query failed: %s", PQerrorMessage(client->conn));
            result = NULL;
            goto done;
        }
    }

    // Parse the result.
    int num_rows    = PQntuples(query_result);
    int num_columns = PQnfields(query_result);

    for (int i = 0; i < num_columns; i++) {
        char *column_name = PQfname(query_result, i);
        *Set(&result->columns, column_name) = i;
    }

    for (int i = 0; i < num_rows; i++) {
        u8_array2 *row = Add(&result->rows);
        *row = (u8_array2){.context = ctx};

        for (int j = 0; j < num_columns; j++) {
            u8_array *cell = Add(row);
            *cell = (u8_array){.context = ctx};

            char *data = PQgetvalue(query_result, i, j);
            int   size = PQgetlength(query_result, i, j);

            array_reserve(cell, size+1);
            cell->data[size] = '\0';

            if (size == 0)  continue;

            memcpy(cell->data, data, size);
            cell->count = size;
        }
    }

    PQclear(query_result);

done:
    if (!client->keep_alive)  close_database(client);

    return result;
}

PG_result *query_database(PG_client *client, char *query, string_array *params, Memory_context *context)
// This function is mostly concerned with caching the results of query_database_uncached().
{
    Memory_context *ctx = context;

    if (!client->use_cache) {
        return query_database_uncached(client, query, params, ctx);
    }

    char cache_dir[]    = "/tmp"; //|Todo: Create our own directory for cache files.
    char magic_number[] = "PG$$";

    u64 query_hash = hash_query(query, params);

    char cache_file_name[64]; {
        int r = snprintf(cache_file_name, sizeof(cache_file_name), "%s/%lx.pgcache", cache_dir, query_hash);
        assert(0 < r && r < sizeof(cache_file_name));
    }

    u8_array *cache_file = load_binary_file(cache_file_name, ctx);

    if (cache_file) {
        printf("Found cache file %s.\n", cache_file_name);

        PG_result *result = New(PG_result, ctx);
        result->columns = (int_dict){.context = ctx};
        result->rows    = (u8_array3){.context = ctx};
        SetDefault(&result->columns, -1);

        //
        // Read the cache file.
        //
        u8 *d = cache_file->data;

        if (memcmp(d, magic_number, lengthof(magic_number))) {
            log_error("We did not find the magic number in %s.", cache_file_name);
            return NULL;
        }
        d += lengthof(magic_number);

        s32 query_length;  memcpy(&query_length, d, sizeof(s32));
        d += sizeof(s32);

        if (query_length != strlen(query) || memcmp(query, d, query_length)) {
            // There has been a hash collision resulting in two different queries with the same cache file name.
            // This is unlikely. For now, throw an error. Deal with it later if it ever happens. |Fixme
            log_error("The current query does not match the one in %s.", cache_file_name);
            return NULL;
        }
        d += query_length;

        s32 num_params;  memcpy(&num_params, d, sizeof(s32));
        d += sizeof(s32);
        if (params)  assert(params->count == num_params);
        else         assert(num_params == 0);

        for (int i = 0; i < num_params; i++) {
            s32 param_length;  memcpy(&param_length, d, sizeof(s32));
            d += sizeof(s32);

            assert(param_length == strlen(params->data[i]));
            assert(!memcmp(d, params->data[i], param_length));

            d += param_length + 1;
        }

        int_dict *columns = &result->columns;

        s32 num_columns;  memcpy(&num_columns, d, sizeof(s32));
        d += sizeof(s32);

        for (int i = 0; i < num_columns; i++) {
            s32 num_chars;  memcpy(&num_chars, d, sizeof(s32));
            d += sizeof(s32);

            *Set(columns, (char *)d) = i;
            d += num_chars;

            assert(*d == '\0'); // The field names are zero-terminated.
            d += 1;
        }

        u8_array3 *rows = &result->rows;

        s32 num_rows;  memcpy(&num_rows, d, sizeof(s32));
        d += sizeof(s32);

        for (int i = 0; i < num_rows; i++) {
            u8_array2 *row = Add(rows);
            *row = (u8_array2){.context = ctx};

            for (s32 j = 0; j < num_columns; j++) {
                s32 cell_size;  memcpy(&cell_size, d, sizeof(s32));
                d += sizeof(s32);

                // Rather than making a copy, we're going to take pointers into the file that we've loaded.
                // So we won't initialise the cell with a context, since it doesn't own its data.
                *Add(row) = (u8_array){.data = d, .count = cell_size};

                d += cell_size+1;
            }
        }

        s64 num_bytes_parsed = d - cache_file->data;
        assert(num_bytes_parsed == cache_file->count);

        return result;
    }

    printf("No cache file found. Making query.\n");

    // Make the query and parse the result.
    PG_result *result = query_database_uncached(client, query, params, ctx);

    //
    // Write the result to a cache file.
    //
    cache_file = NewArray(cache_file, ctx);

    for (int i = 0; i < lengthof(magic_number); i++)  *Add(cache_file) = magic_number[i];

    s32 query_length = strlen(query);
    add_s32(cache_file, query_length);

    for (int i = 0; i < query_length; i++)  *Add(cache_file) = query[i];

    s32 num_params = 0;
    if (params)  num_params = params->count;
    add_s32(cache_file, num_params);

    for (int i = 0; i < num_params; i++) {
        char *param = params->data[i];

        s32 param_length = strlen(param);
        add_s32(cache_file, param_length);

        for (int j = 0; j < param_length; j++)  *Add(cache_file) = param[j];
        *Add(cache_file) = '\0';
    }

    s32 num_columns = result->columns.count;
    add_s32(cache_file, num_columns);

    for (int i = 0; i < num_columns; i++) {
        char *column_name = result->columns.keys[i];
        int   name_length = strlen(column_name);

        add_s32(cache_file, name_length);

        for (int j = 0; j < name_length; j++)  *Add(cache_file) = column_name[j];
        *Add(cache_file) = '\0';
    }

    s32 num_rows = result->rows.count;
    add_s32(cache_file, num_rows);

    for (int i = 0; i < num_rows; i++) {
        u8_array2 *row = &result->rows.data[i];

        for (s32 j = 0; j < num_columns; j++) {
            u8_array *cell = &row->data[j];

            add_s32(cache_file, (s32)cell->count);

            for (int k = 0; k < cell->count; k++)  *Add(cache_file) = cell->data[k];
            *Add(cache_file) = '\0';
        }
    }

    write_array_to_file(cache_file, cache_file_name);

    return result;
}

void close_database(PG_client *client)
{
    if (client->conn) {
        PQfinish(client->conn);
        client->conn = NULL;
    } else {
        // The only way we could have not connected to the database is if we got the results from cache.
        assert(client->use_cache);
    }
}

u32 get_u32_from_cell(u8_array *cell)
// The cell must be 4 bytes.
{
    assert(cell->count == sizeof(u32));

    // Make sure the u32 we pass to ntohl() is aligned.
    u32 n;  memcpy(&n, cell->data, sizeof(u32));

    return ntohl(n);
}

float get_float_from_cell(u8_array *cell)
// The cell must be 4 bytes.
{
    u32 parsed_u32 = get_u32_from_cell(cell);

    float f;  memcpy(&f, &parsed_u32, sizeof(float));

    return f;
}

char_array get_char_array_from_cell(u8_array *cell)
// Don't copy the data. Just put the data pointer and count into a char_array.
{
    // Because we are using the cell's data as a string without making a copy, we're assuming it's already zero-terminated.
    assert(cell->data[cell->count] == 0);

    // Don't give the returned char_array a context or limit because it doesn't own its data.
    char_array result = {0};
    result.data  = (char *)cell->data;
    result.count = cell->count;

    return result;
}
