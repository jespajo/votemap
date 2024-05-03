#include <stdio.h>
#include <string.h>

#include "libpq-fe.h"

#include "map.h"
#include "array.h"

// @Todo move to vector.h.
typedef struct {float v[2];}    Vector2;
typedef struct {float v[3];}    Vector3;
typedef struct {float v[4];}    Vector4;
typedef struct {float m[4][4];} Matrix4;

typedef Array(Vector2)    Linestring;
typedef Array(Linestring) Polygon; // We follow the convention where the first ring of a polygon is the outer ring. Subsequent rings are holes.
typedef Array(Polygon)    Polygon_array;

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

Polygon *polygon_from_wkb_geometry(char *geometry, Memory_Context *context)
// @Todo: Check that the number of bytes parsed by this function is equal to the Postgres tuple size
// (not sure of the PQ function to check this but there must be one). We will have to pass the
// number to the function I guess?
{
    Polygon *polygon = NewArray(polygon, context);

    u8 byte_order = *geometry;
    geometry += sizeof(u8);
    assert(byte_order == WKB_LITTLE_ENDIAN);

    u32 wkb_type;  memcpy(&wkb_type, geometry, sizeof(u32));
    geometry += sizeof(u32);
    assert(wkb_type == WKB_POLYGON);

    u32 num_rings;  memcpy(&num_rings, geometry, sizeof(u32));
    geometry += sizeof(u32);

    while (num_rings--) {
        Linestring ring = {.context = context};

        u32 num_points;  memcpy(&num_points, geometry, sizeof(u32));
        geometry += sizeof(u32);

        while (num_points--) {
            double x;  memcpy(&x, geometry, sizeof(double));
            geometry += sizeof(double);

            double y;  memcpy(&y, geometry, sizeof(double));
            geometry += sizeof(double);

            *Add(&ring) = (Vector2){(float)x, (float)y};
        }

        *Add(polygon) = ring;
    }

    return polygon;
}

PGresult *query_database(char *query, PGconn *db)
// Make a database query and return the results as binary.
{
    PGresult *result = PQexecParams(db, query, 0, NULL, NULL, NULL, NULL, 1);
    if (PQresultStatus(result) != PGRES_TUPLES_OK) {
        Error("Query failed: %s", PQerrorMessage(db));
    }
    return result;
}

/*
int main()
{
    Memory_Context *ctx = new_context(NULL);

    PGconn *db = PQconnectdb("postgres://postgres:postgisclarity@osm.tal/gis");
    if (PQstatus(db) != CONNECTION_OK) {
        Error("Database connection failed: %s", PQerrorMessage(db));
    }

    Polygon_array *polygons = NewArray(polygons, ctx);
    {
        //char *query = "SELECT ST_AsBinary(ST_PolygonFromText('POLYGON((0 300, 20 250, 100 200, 50 0, 0 300))')) AS polygon";
        char *query = "SELECT ST_AsBinary(swp.way) AS polygon FROM simplified_water_polygons swp JOIN (SELECT * FROM planet_osm_polygon WHERE name = 'Australia') AS australia ON ST_Within(swp.way, australia.way)";

        PGresult *result = query_database(query, db);

        int num_tuples = PQntuples(result);
        assert(num_tuples >= 1);

        int polygon_column = 0;
        assert(PQfnumber(result, "polygon") == polygon_column);

        for (int i = 0; i < num_tuples; i++) {
            char *geometry = PQgetvalue(result, 0, polygon_column);

            *Add(polygons) = *polygon_from_wkb_geometry(geometry, ctx);
        }

        PQclear(result);
    }

    printf("Found %ld polygons!\n", polygons->count);

    PQfinish(db);
    free_context(ctx);

    return 0;
}
*/

struct Vertex {
    float x, y;
    float r, g, b, a;
};
typedef struct Vertex  Vertex;
typedef Array(Vertex)  Vertex_array;

int main()
{
    Memory_Context *ctx = new_context(NULL);

    Vertex_array *array = NewArray(array, ctx);

    *Add(array) = (Vertex){0.0, 0.0, 0.98, 0.34, 0.54, 1.0};
    *Add(array) = (Vertex){0.0, 1.0, 0.18, 0.74, 0.54, 1.0};
    *Add(array) = (Vertex){1.0, 1.0, 0.18, 0.34, 0.84, 1.0};

    *Add(array) = (Vertex){0.0, 0.0, 0.38, 0.94, 0.54, 1.0};
    *Add(array) = (Vertex){0.9, 0.0, 0.58, 0.24, 0.54, 1.0};
    *Add(array) = (Vertex){0.9, 0.9, 0.88, 0.34, 0.44, 1.0};

    //
    // Write to file.
    //

    char *file_name = "/home/jpj/src/webgl/bin/vertices";
    FILE *file      = fopen(file_name, "wb");

    u64 num_chars = fwrite(array->data, sizeof(array->data[0]), array->count, file);
    assert(num_chars > 0);

    free_context(ctx);
    return 0;
}
