#include <string.h>

#include "pg.h"

#define QueryError(...)  (Error(__VA_ARGS__), NULL)

Polygon_array *query_polygons(PGconn *db, Memory_Context *context, char *query)
{
    PGresult *result = PQexecParams(db, query, 0, NULL, NULL, NULL, NULL, 1);
    if (PQresultStatus(result) != PGRES_TUPLES_OK)  return QueryError("Query failed: %s", PQerrorMessage(db));

    int num_tuples = PQntuples(result);
    if (num_tuples == 0)  return QueryError("A query for polygons returned no results.");

    int column = PQfnumber(result, "polygon");
    if (column < 0)  return QueryError("We couldn't find a \"polygon\" column in the results.");

    Polygon_array *polygons = NewArray(polygons, context);

    for (int row = 0; row < num_tuples; row++) {
        char *data = PQgetvalue(result, row, column);
        char *d = data;

        Polygon polygon = {.context = context};

        u8 byte_order = *d;
        d += sizeof(u8);
        assert(byte_order == WKB_LITTLE_ENDIAN);

        u32 wkb_type;  memcpy(&wkb_type, d, sizeof(u32));
        d += sizeof(u32);

        // We want this function to work for both WKB_POLYGON and WKB_MULTIPOLYGON data.
        u32 num_polygons;
        if (wkb_type == WKB_POLYGON) {
            num_polygons = 1;
        } else if (wkb_type == WKB_MULTIPOLYGON) {
            memcpy(&num_polygons, d, sizeof(u32));
            d += sizeof(u32);
        } else {
            return QueryError("We expected WKB_POLYGON (3) or WKB_MULTIPOLYGON (6). Instead we got %d.", wkb_type);
        }

        while (num_polygons--) {
            if (wkb_type == WKB_MULTIPOLYGON) {
                // It's a multipolygon. We need to parse the byte order and data type again for each polygon.
                // This is ugly, particularly the reuse of the byte_order and wkb_type variables. @Cleanup.
                u8 byte_order = *d;
                d += sizeof(u8);
                assert(byte_order == WKB_LITTLE_ENDIAN);

                u32 wkb_type;  memcpy(&wkb_type, d, sizeof(u32));
                d += sizeof(u32);
                assert(wkb_type == WKB_POLYGON);
            }

            u32 num_rings;  memcpy(&num_rings, d, sizeof(u32));
            d += sizeof(u32);

            while (num_rings--) {
                Path ring = {.context = context};

                u32 num_points;  memcpy(&num_points, d, sizeof(u32));
                d += sizeof(u32);

                while (num_points--) {
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
                *Add(&polygon) = ring;
            }
            *Add(polygons) = polygon;
        }

        // Check that the number of bytes parsed by this function is equal to the Postgres reported size.
        s64 num_bytes_parsed = d - data;
        assert(num_bytes_parsed == PQgetlength(result, row, column));
    }

    PQclear(result);

    return polygons;
}

Path_array *query_paths(PGconn *db, Memory_Context *context, char *query)
// @Copypasta from query_polygons().
{
    PGresult *result = PQexecParams(db, query, 0, NULL, NULL, NULL, NULL, 1);
    if (PQresultStatus(result) != PGRES_TUPLES_OK)  return QueryError("Query failed: %s", PQerrorMessage(db));

    int num_tuples = PQntuples(result);
    if (num_tuples == 0)  return QueryError("A query for paths returned no results.");

    int column = PQfnumber(result, "path");
    if (column < 0)  return QueryError("We couldn't find a \"path\" column in the results.");

    Path_array *paths = NewArray(paths, context);

    for (int row = 0; row < num_tuples; row++) {
        char *data = PQgetvalue(result, row, column);
        char *d = data;

        Path path = {.context = context};

        u8 byte_order = *d;
        d += sizeof(u8);
        assert(byte_order == WKB_LITTLE_ENDIAN);

        u32 wkb_type;  memcpy(&wkb_type, d, sizeof(u32));
        d += sizeof(u32);
        assert(wkb_type == WKB_LINESTRING);

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
        *Add(paths) = path;

        // Check that the number of bytes parsed by this function is equal to the Postgres reported size.
        s64 num_bytes_parsed = d - data;
        assert(num_bytes_parsed == PQgetlength(result, row, column));
    }

    PQclear(result);

    return paths;
}

#undef QueryError
