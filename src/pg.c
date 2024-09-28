#include <arpa/inet.h> // For ntohl(), which we use in get_u32_from_cell().

#include "pg.h"
#include "strings.h"

#define QueryError(...)  (log_error(__VA_ARGS__), NULL)

enum WKB_Byte_Order {
    WKB_BIG_ENDIAN    = 0,
    WKB_LITTLE_ENDIAN = 1,
};

enum WKB_Geometry_Type {
    WKB_POINT              = 1,
    WKB_LINESTRING         = 2,
    WKB_POLYGON            = 3,
    WKB_MULTIPOINT         = 4,
    WKB_MULTILINESTRING    = 5,
    WKB_MULTIPOLYGON       = 6,
    WKB_GEOMETRYCOLLECTION = 7,
};

static u64 hash_query(char *query, string_array *params)
{
    u64 hash = hash_string(query);

    s64 num_params = (params) ? params->count : 0;
    for (s64 i = 0; i < num_params; i++)  hash ^= hash_string(params->data[i]);

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

PGconn *connect_to_database(char *url)
// An expedient static variable means we can call this function first from the function that's also
// responsible for closing the connection with PQfinish(PGconn *), and then from anywhere for the
// life of the connection.
// This function supports one database only; we don't bother checking the url after the first call.
{
    static PGconn *conn = NULL; //|Threadsafety

    if (!conn)  conn = PQconnectdb(url);

    if (PQstatus(conn) != CONNECTION_OK)  Fatal("Database connection failed: %s", PQerrorMessage(conn));

    return conn;
}

void parse_polygons(u8 *data, Polygon_array *result, u8 **end_data)
// data is a pointer to EWKB geometries. end_data is like the strtod() argument: if it's not NULL, it is the address
// where this function will save a pointer to the byte after the last byte of data parsed.
{
    assert(data);
    Memory_context *ctx = result->context;
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
                    if (!ring_index) {if (points_are_clockwise(ring.data, ring.count))      reverse_array(&ring);}
                    else             {if (points_are_anticlockwise(ring.data, ring.count))  reverse_array(&ring);}
                    // |Todo: What if the determinant is 0?

                    *Add(&polygon) = ring;
                }

                *Add(result) = polygon;
                break;
            default:
                Fatal("Unexpected wkb_type: %d.", wkb_type);
        }
    }

    if (end_data)  *end_data = d;
}

Polygon_array *query_polygons(PGconn *db, char *query, string_array *params, Memory_context *context) //|Deprecated
{
    Postgres_result *pg_result = query_database(db, query, params, context);

    s64 column_index = *Get(pg_result->columns, "polygon");
    if (column_index < 0)  return QueryError("Couldn't find a \"polygon\" column in the results.");

    u8_array3 *rows = &pg_result->rows;
    if (!rows->count)  return QueryError("A query for polygons returned no results.");

    Polygon_array *polygons = NewArray(polygons, context);

    for (s64 i = 0; i < rows->count; i++) {
        u8_array2 *row = &rows->data[i];
        u8_array *cell = &row->data[column_index];
        if (!cell->count)  continue;

        u8 *end_data = NULL;
        parse_polygons(cell->data, polygons, &end_data);

        assert(end_data == &cell->data[cell->count]);
    }

    return polygons;
}

void parse_paths(u8 *data, Path_array *result, u8 **end_data)
// data is a pointer to EWKB geometries. end_data is like the strtod() argument: if it's not NULL, it is the address
// where this function will save a pointer to the byte after the last byte of data parsed.
{
    Memory_context *ctx = result->context;
    u8 *d = data;

    {
        Path path = {.context = ctx};

        u8 byte_order = *d;
        d += sizeof(u8);
        assert(byte_order == WKB_LITTLE_ENDIAN);

        u32 wkb_type;  memcpy(&wkb_type, d, sizeof(u32));
        d += sizeof(u32);
        assert(wkb_type == WKB_LINESTRING); // |Todo: Be more flexible. Polygons can be parsed as paths. Only points can't.

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

Path_array *query_paths(PGconn *db, char *query, string_array *params, Memory_context *context)
{
    Postgres_result *pg_result = query_database(db, query, params, context);

    s64 column_index = *Get(pg_result->columns, "path");
    if (column_index < 0)  return QueryError("Couldn't find a \"path\" column in the results.");

    Path_array *paths = NewArray(paths, context);

    if (!pg_result->rows.count)  printf("A query for paths returned no results.");

    for (s64 i = 0; i < pg_result->rows.count; i++) {
        u8_array2 *row = &pg_result->rows.data[i];
        u8_array *cell = &row->data[column_index];
        if (!cell->count)  continue;

        u8 *end_data = NULL;
        parse_paths(cell->data, paths, &end_data);

        assert(end_data == &cell->data[cell->count]);
    }

    return paths;
}

static Postgres_result *query_database_uncached(PGconn *db, char *query, string_array *params, Memory_context *context)
// Actually query the database and parse the result into a Postgres_result.
// This is called by query_database.
{
    Memory_context *ctx = context;

    Postgres_result *result = New(Postgres_result, ctx);
    result->columns = NewDict(result->columns, ctx);
    result->rows    = (u8_array3){.context = ctx};
    SetDefault(result->columns, -1);

    // Make the query.
    PGresult *query_result; {
        int num_params = (params) ? params->count : 0;
        char const *const *param_data = (params) ? (char const *const *)params->data : NULL;

        query_result = PQexecParams(db, query, num_params, NULL, param_data, NULL, NULL, 1);

        if (PQresultStatus(query_result) != PGRES_TUPLES_OK) {
            return QueryError("Query failed: %s", PQerrorMessage(db));
        }
    }

    // Parse the result.
    int num_rows    = PQntuples(query_result);
    int num_columns = PQnfields(query_result);

    for (int i = 0; i < num_columns; i++) {
        char *column_name = PQfname(query_result, i);
        *Set(result->columns, column_name) = i;
    }

    for (int i = 0; i < num_rows; i++) {
        u8_array2 *row = Add(&result->rows);
        *row = (u8_array2){.context = ctx};

        for (int j = 0; j < num_columns; j++) {
            u8_array *cell = Add(row);
            *cell = (u8_array){.context = ctx};

            char *data = PQgetvalue(query_result, i, j);
            int   size = PQgetlength(query_result, i, j);

            if (size == 0)  continue;

            array_reserve(cell, size);
            memcpy(cell->data, data, size);
            cell->count = size;
        }
    }

    PQclear(query_result);

    return result;
}

Postgres_result *query_database(PGconn *db, char *query, string_array *params, Memory_context *context)
// Parameters are string literals. Cast them in your queries: `SELECT $1::int;`
// This function is mostly concerned with caching the results of query_database_uncached().
{
    char cache_dir[]    = "/tmp"; //|Todo: Create our own directory for cache files.
    char magic_number[] = "PG$$";

    Memory_context *ctx = context;

    u64 query_hash = hash_query(query, params);

    char cache_file_name[64]; {
        int r = snprintf(cache_file_name, sizeof(cache_file_name), "%s/%lx.pgcache", cache_dir, query_hash);
        assert(0 < r && r < sizeof(cache_file_name));
    }

    u8_array *cache_file = load_binary_file(cache_file_name, ctx);

    if (cache_file) {
        printf("Found cache file %s.\n", cache_file_name);

        Postgres_result *result = New(Postgres_result, ctx);
        result->columns = NewDict(result->columns, ctx);
        result->rows    = (u8_array3){.context = ctx};
        SetDefault(result->columns, -1);

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

        s64_dict *columns = result->columns;

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

                d += cell_size;
            }
        }

        s64 num_bytes_parsed = d - cache_file->data;
        assert(num_bytes_parsed == cache_file->count);

        return result;
    }

    printf("No cache file found. Making query.\n");

    // Make the query and parse the result.
    Postgres_result *result = query_database_uncached(db, query, params, ctx);

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

    s32 num_columns = result->columns->count;
    add_s32(cache_file, num_columns);

    for (int i = 0; i < num_columns; i++) {
        char *column_name = result->columns->keys[i];
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
        }
    }

    write_array_to_file(cache_file, cache_file_name);

    return result;
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

char_array *copy_char_array_from_cell(u8_array *cell, Memory_context *context)
//|Speed: This makes a copy of the string.
{
    char_array *result = NewArray(result, context);

    array_reserve(result, cell->count+1);

    memcpy(result->data, cell->data, cell->count);
    result->data[cell->count] = '\0';

    result->count = cell->count;

    return result;
}
