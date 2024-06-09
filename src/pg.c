#include <string.h>

#include "pg.h"
#include "strings.h"

#define QueryError(...)  (Error(__VA_ARGS__), NULL)

static u64 hash_query(char *query, string_array *params)
{
    u64 hash = hash_string(query);

    if (params) {
        for (int i = 0; i < params->count; i++)  hash ^= hash_string(params->data[i]);
    }

    return hash;
}

static void add_s32(u8_array *array, s32 number)
// Append a 32-bit integer to an array of bytes.
{
    s64 new_count = array->count + sizeof(s32);
    if (array->limit < new_count) {
        array_reserve(array, round_up_to_power_of_two(new_count));
    }
    memcpy(array->data + array->count, &number, sizeof(s32));
    array->count = new_count;
}

void parse_polygons(u8 *data, Polygon_array *result, u8 **end_data)
// data is a pointer to EWKB geometries. Like strtod, if end_data is not NULL, it is the address
// where this function will save a pointer to the byte after the last byte of data parsed.
{
    Memory_Context *ctx = result->context;
    u8 *d = data;

    s64 num_geometries = 1;

    while (num_geometries > 0) {
        num_geometries -= 1;

        u8 byte_order = *d;
        d += sizeof(u8);
        assert(byte_order == WKB_LITTLE_ENDIAN);

        u32 wkb_type;  memcpy(&wkb_type, d, sizeof(u32));
        d += sizeof(u32);

        switch (wkb_type) {
            case WKB_GEOMETRYCOLLECTION:
            case WKB_MULTIPOLYGON:;
                // We treat GeometryCollections and MultiPolygons the same: we just add to the
                // number of extra geometries to expect in the results. This means we'll accept
                // technically invalid data such as GeometryCollections inside MultiPolygons.
                // Since we're just consuming this data from Postgres, I don't think we need to
                // be strict about making sure it's well-formed.
                u32 num_extra;  memcpy(&num_extra, d, sizeof(u32));
                d += sizeof(u32);

                num_geometries += num_extra;
                break;
            case WKB_POLYGON:;
                Polygon polygon = {.context = ctx};

                u32 num_rings;  memcpy(&num_rings, d, sizeof(u32));
                d += sizeof(u32);

                // Ignore polygons without any rings.
                if (!num_rings)  break;

                for (u32 ring_index = 0; ring_index < num_rings; ring_index += 1) {
                    Path ring = {.context = ctx};

                    u32 num_points;  memcpy(&num_points, d, sizeof(u32));
                    d += sizeof(u32);

                    for (u32 point_index = 0; point_index < num_points; point_index += 1) {
                        double x;  memcpy(&x, d, sizeof(double));
                        d += sizeof(double);

                        double y;  memcpy(&y, d, sizeof(double));
                        d += sizeof(double);

                        // Cast doubles to floats.
                        *Add(&ring) = (Vector2){
                            (float)x,
                            -(float)y // Flip the y-axis.
                        };
                    }

                    // Ensure the first ring is counter-clockwise and subsequent rings are clockwise.
                    if (!ring_index ^ !points_are_clockwise(ring.data, ring.count))  reverse_array(&ring);

                    *Add(&polygon) = ring;
                }

                *Add(result) = polygon;
                break;
            default:
                Error("Unexpected wkb_type: %d.", wkb_type);
        }
    }

    if (end_data)  *end_data = d;
}

Polygon_array *query_polygons(PGconn *db, char *query, string_array *params, Memory_Context *context)
{
    Postgres_result *rows = query_database(db, query, params, context);
    if (!rows->count)  return QueryError("A query for polygons returned no results.");

    Polygon_array *result = NewArray(result, context);

    for (s64 i = 0; i < rows->count; i++) {
        Result_row *row = rows->data[i];

        u8_array *cell = *Get(row, "polygon");
        if (!cell)  return QueryError("Couldn't find a \"polygon\" column in the results.");

        u8 *end_data = NULL;
        parse_polygons(cell->data, result, &end_data);

        assert(end_data - cell->data == cell->count);
    }

    return result;
}

//
// Some code for visualising Postgres_result structs:
//
//   #include <stdio.h>
//   #include <ctype.h>
//
//     for (s64 i = 0; i < result->count; i++) {
//         Result_row *row = result->data[i];
//
//         printf("{");
//         for (s64 j = 0; j < row->count; j++) {
//             char    *field = row->keys[j];
//             u8_array *cell = row->vals[j];
//
//             if (j)  printf(", ");
//
//             printf("%s: \"", field);
//             for (s64 k = 0; k < cell->count; k++) {
//                 int c = cell->data[k];
//                 if (isprint(c))  printf("%c",    c);
//                 else             printf("\\%#x", c);
//             }
//             printf("\"");
//         }
//         printf("}\n");
//     }
//

void parse_paths(u8 *data, Path_array *result, u8 **end_data)
// data is a pointer to EWKB geometries. Like strtod, if end_data is not NULL, it is the address
// where this function will save a pointer to the byte after the last byte of data parsed.
{
    Memory_Context *ctx = result->context;
    u8 *d = data;

    {
        Path path = {.context = ctx};

        u8 byte_order = *d;
        d += sizeof(u8);
        assert(byte_order == WKB_LITTLE_ENDIAN);

        u32 wkb_type;  memcpy(&wkb_type, d, sizeof(u32));
        d += sizeof(u32);
        assert(wkb_type == WKB_LINESTRING); // @Todo: Be more flexible. Polygons can be parsed as paths. Only points can't.

        u32 num_points;  memcpy(&num_points, d, sizeof(u32));
        d += sizeof(u32);

        while (num_points--) {
            double x;  memcpy(&x, d, sizeof(double));
            d += sizeof(double);

            double y;  memcpy(&y, d, sizeof(double));
            d += sizeof(double);

            // Cast doubles to floats.
            *Add(&path) = (Vector2){
                (float)x,
                -(float)y // Flip the y-axis.
            };
        }

        *Add(result) = path;
    }

    if (end_data)  *end_data = d;
}

Path_array *query_paths(PGconn *db, char *query, string_array *params, Memory_Context *context)
{
    Postgres_result *rows = query_database(db, query, params, context);

    if (!rows->count)  return QueryError("A query for paths returned no results.");

    Path_array *result = NewArray(result, context);

    for (s64 i = 0; i < rows->count; i++) {
        Result_row *row = rows->data[i];

        u8_array *cell = *Get(row, "path");
        if (!cell)  return QueryError("Couldn't find a \"path\" column in the results.");
        if (!cell->count)  continue;

        u8 *end_data = NULL;
        parse_paths(cell->data, result, &end_data);

        assert(end_data - cell->data == cell->count);
    }

    return result;
}

static Postgres_result *query_database_uncached(PGconn *db, char *query, string_array *params, Memory_Context *context)
// Actually query the database and parse the result into a Postgres_result.
// This is called by query_database.
{
    Memory_Context *ctx = context;

    Postgres_result *result = NewArray(result, ctx);

    // Make the query.
    PGresult *query_result; {
        int num_params = 0;
        char const *const *param_data = NULL;
        if (params) {
            num_params = params->count;
            param_data = (char const *const *)params->data;
        }
        query_result = PQexecParams(db, query, num_params, NULL, param_data, NULL, NULL, 1);

        if (PQresultStatus(query_result) != PGRES_TUPLES_OK)  return QueryError("Query failed: %s", PQerrorMessage(db));
    }

    // Parse the result.
    int num_rows    = PQntuples(query_result);
    int num_columns = PQnfields(query_result);

    string_array *fields = NewArray(fields, ctx);
    for (int i = 0; i < num_columns; i++)  *Add(fields) = PQfname(query_result, i);

    for (int i = 0; i < num_rows; i++) {
        Result_row *row = NewDict(row, ctx);

        for (int j = 0; j < num_columns; j++) {
            char *field = fields->data[j];
            char *data  = PQgetvalue(query_result, i, j);
            int   size  = PQgetlength(query_result, i, j);

            u8_array *cell = NewArray(cell, ctx);

            for (int k = 0; k < size; k++)  *Add(cell) = (u8)data[k];

            *Set(row, field) = cell;
        }

        *Add(result) = row;
    }

    PQclear(query_result);

    return result;
}

Postgres_result *query_database(PGconn *db, char *query, string_array *params, Memory_Context *context)
// Parameters are string literals. Cast them in your queries: SELECT $1::int;
// This function is mostly concerned with caching the results of query_database_uncached().
{
    char cache_dir[]    = "/tmp"; // @Todo: Create our own directory for cache files.
    char magic_number[] = "PG$$";

    Memory_Context *ctx = context;

    Postgres_result *result = New(Postgres_result, ctx);

    u64 query_hash = hash_query(query, params);

    char *cache_file_name = get_string(ctx, "%s/%x.pgcache", cache_dir, query_hash)->data;

    u8_array *cache_file = load_binary_file(cache_file_name, ctx);

    if (cache_file) {
        Log("Found cache file %s.", cache_file_name);
        //
        // Read the cache file.
        //
        u8 *d = cache_file->data;

        if (memcmp(d, magic_number, lengthof(magic_number))) {
            return QueryError("We did not find the magic number in %s.", cache_file_name);
        }
        d += lengthof(magic_number);

        s32 query_length;  memcpy(&query_length, d, sizeof(s32));
        d += sizeof(s32);

        if (query_length != strlen(query) || memcmp(query, d, query_length)) {
            // There has been a hash collision resulting in two different queries with the same cache file name.
            // This is unlikely. For now, throw an error. Deal with it later if it ever happens.
            return QueryError("The current query does not match the one in %s.", cache_file_name);
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

        s32 num_columns;  memcpy(&num_columns, d, sizeof(s32));
        d += sizeof(s32);

        string_array *fields = NewArray(fields, ctx);

        for (int i = 0; i < num_columns; i++) {
            s32 num_chars;  memcpy(&num_chars, d, sizeof(s32));
            d += sizeof(s32);

            // Just take a pointer to the file data for now. We don't need to copy the name because
            // fields doesn't survive this function.
            *Add(fields) = (char *)d;
            d += num_chars;

            assert(*d == '\0'); // The field names are zero-terminated.
            d += 1;
        }

        s32 num_rows;  memcpy(&num_rows, d, sizeof(s32));
        d += sizeof(s32);

        for (int i = 0; i < num_rows; i++) {
            Result_row *row = NewDict(row, ctx);

            for (s32 j = 0; j < num_columns; j++) {
                char *field = fields->data[j];

                u8_array *cell = NewArray(cell, ctx);

                s32 cell_size;  memcpy(&cell_size, d, sizeof(s32));
                d += sizeof(s32);

                for (int k = 0; k < cell_size; k++)  *Add(cell) = d[k];
                d += cell_size;

                *Set(row, field) = cell;
            }

            *Add(result) = row;
        }

        s64 num_bytes_parsed = d - cache_file->data;
        assert(num_bytes_parsed == cache_file->count);

        return result;
    }

    Log("No cache file found. Making query.");

    // Make the query and parse the result.
    result = query_database_uncached(db, query, params, ctx);

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

    s32 num_columns = result->data[0]->count;
    add_s32(cache_file, num_columns);

    for (int i = 0; i < num_columns; i++) {
        char *field = result->data[0]->keys[i];
        int  length = strlen(field);

        add_s32(cache_file, length);

        for (int j = 0; j < length; j++)  *Add(cache_file) = field[j];
        *Add(cache_file) = '\0';
    }

    s32 num_rows = result->count;
    add_s32(cache_file, num_rows);

    for (int i = 0; i < num_rows; i++) {
        Result_row *row = result->data[i];

        for (s32 j = 0; j < num_columns; j++) {
            char *field = result->data[i]->keys[j];

            u8_array *cell = *Get(row, field);

            add_s32(cache_file, (s32)cell->count);

            for (int k = 0; k < cell->count; k++)  *Add(cache_file) = cell->data[k];
        }
    }

    write_array_to_file(cache_file, cache_file_name);

    // Return the result.
    return result;
}

#undef QueryError
