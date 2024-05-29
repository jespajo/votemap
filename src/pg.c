#include <string.h>

#include "pg.h"

#define QueryError(...)  (Error(__VA_ARGS__), NULL)

Polygon_array *query_polygons(PGconn *db, Memory_Context *context, char *query)
{
    PGresult *result = PQexecParams(db, query, 0, NULL, NULL, NULL, NULL, 1);
    if (PQresultStatus(result) != PGRES_TUPLES_OK)  return QueryError("Query failed: %s", PQerrorMessage(db));

    int num_rows = PQntuples(result);
    if (num_rows == 0)  return QueryError("A query for polygons returned no results.");

    int column = PQfnumber(result, "polygon");
    if (column < 0)  return QueryError("We couldn't find a \"polygon\" column in the results.");

    Polygon_array *polygons = NewArray(polygons, context);

    for (int row = 0; row < num_rows; row++) {
        int num_bytes = PQgetlength(result, row, column);
        if (!num_bytes)  continue;

        char *data = PQgetvalue(result, row, column);
        char *d = data;

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
                    continue;
                case WKB_POLYGON:;
                    Polygon polygon = {.context = context};

                    u32 num_rings;  memcpy(&num_rings, d, sizeof(u32));
                    d += sizeof(u32);

                    for (u32 ring_index = 0; ring_index < num_rings; ring_index += 1) {
                        Path ring = {.context = context};

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

                        *Add(&polygon) = ring;
                    }

                    *Add(polygons) = polygon;
                    break;
                default:
                    Error("Unexpected wkb_type: %d.", wkb_type);
            }
        }

        // Check that the number of bytes parsed by this function is equal to the Postgres reported size.
        assert(d - data == num_bytes);
    }

    PQclear(result);

    return polygons;
}

Path_array *query_paths(PGconn *db, Memory_Context *context, char *query)
// Old @Copypasta from query_polygons().
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
