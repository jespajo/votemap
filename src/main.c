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

struct Vertex {
    float x, y;
    float r, g, b, a;
};
typedef struct Vertex  Vertex;
typedef Array(Vertex)  Vertex_array;

Vertex_array *vertices_from_delaunay_triangles(char *geometry_collection, Vector4 colour, Memory_Context *context)
// The Postgis ST_DelaunayTriangles() function returns a WKB GeometryCollection. Every member of the
// collection is a polygon. Each polygon is a triangle represented by four points---the first and
// last points are the same. This function turns this data into an array of vertex attributes.
{
    Vertex_array *vertices = NewArray(vertices, context);

    char *data = geometry_collection;

    u8 byte_order;  memcpy(&byte_order, data, sizeof(u8));
    data += sizeof(u8);
    assert(byte_order == WKB_LITTLE_ENDIAN);

    u32 wkb_type;  memcpy(&wkb_type, data, sizeof(u32));
    data += sizeof(u32);
    assert(wkb_type == WKB_GEOMETRYCOLLECTION);

    u32 num_triangles;  memcpy(&num_triangles, data, sizeof(u32));
    data += sizeof(u32);

    for (int i = 0; i < num_triangles; i++) {
        u8 byte_order;  memcpy(&byte_order, data, sizeof(u8));
        data += sizeof(u8);
        assert(byte_order == WKB_LITTLE_ENDIAN);

        u32 wkb_type;  memcpy(&wkb_type, data, sizeof(u32));
        data += sizeof(u32);
        assert(wkb_type == WKB_POLYGON);

        u32 num_rings;  memcpy(&num_rings, data, sizeof(u32));
        data += sizeof(u32);
        assert(num_rings == 1);

        u32 num_points;  memcpy(&num_points, data, sizeof(u32));
        data += sizeof(u32);
        assert(num_points == 4);
        
        for (int j = 0; j < 3; j++) {
            double x;  memcpy(&x, data, sizeof(double));
            data += sizeof(double);

            double y;  memcpy(&y, data, sizeof(double));
            data += sizeof(double);

            float r = colour.v[0];
            float g = colour.v[1];
            float b = colour.v[2];
            float a = colour.v[3];

            *Add(vertices) = (Vertex){(float)x, (float)y, r, g, b, a};
        }

        // Skip the fourth point, which should be the same as the fourth.
        // @Todo: Assert that it's the same?
        data += sizeof(double);
        data += sizeof(double);
    }

    return vertices;
}

Vertex_array *merge_vertex_arrays(Vertex_array **arrays, s64 num_arrays, Memory_Context *context)
// @Speed! Slow because it copies all the data.
// This is also something we may want to make generic later and use a macro for e.g. Merge() or Flatten().
{
    Vertex_array *merged = NewArray(merged, context);

    for (int i = 0; i < num_arrays; i++) {
        Vertex_array *array = arrays[i];

        for (int j = 0; j < array->count; j++)  *Add(merged) = array->data[j];
    }

    return merged;
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

void write_array_to_file_(void *data, u64 unit_size, s64 count, char *file_name)
{
    FILE *file = fopen(file_name, "wb");

    u64 num_chars_written = fwrite(data, unit_size, count, file);
    assert(num_chars_written > 0);

    fclose(file);
}
#define write_array_to_file(ARRAY, FILE_NAME)  \
    write_array_to_file_((ARRAY)->data, sizeof((ARRAY)->data[0]), (ARRAY)->count, (FILE_NAME))

int main()
{
    Memory_Context *ctx = new_context(NULL);

    PGconn *db = PQconnectdb("postgres://postgres:postgisclarity@osm.tal/gis");
    if (PQstatus(db) != CONNECTION_OK) {
        Error("Database connection failed: %s", PQerrorMessage(db));
    }

    Vertex_array *vertices = NULL;
    {
        //char *query = "SELECT ST_AsBinary(ST_DelaunayTriangles(swp.way)) AS collection FROM simplified_water_polygons swp JOIN (SELECT * FROM planet_osm_polygon WHERE name = 'Australia') AS australia ON ST_Within(swp.way, australia.way)";
        //char *query = " select ST_AsBinary(ST_DelaunayTriangles(way)) AS collection from planet_osm_polygon where name = 'Tasmania' and place = 'island' ";
        char *query = "SELECT ST_AsBinary(ST_DelaunayTriangles(way)) AS collection FROM ( select ST_PolygonFromText('POLYGON((0 300, 20 250, 100 200, 50 0, 0 300))') as way) t";

        PGresult *result = query_database(query, db);

        int num_tuples = PQntuples(result);
        assert(num_tuples >= 1);

        int collection_column = 0;
        assert(PQfnumber(result, "collection") == collection_column);

        Array(Vertex_array *) *arrays = NewArray(arrays, ctx);

        for (int i = 0; i < num_tuples; i++) {
            char *collection = PQgetvalue(result, 0, collection_column);

            Vector4 colour = {0.5, 0.9, 0.3, 1.0};

            *Add(arrays) = vertices_from_delaunay_triangles(collection, colour, ctx);
        }

        vertices = merge_vertex_arrays(arrays->data, arrays->count, ctx);

        PQclear(result);
    }

    // Write vertices to file.
    write_array_to_file(vertices, "/home/jpj/src/webgl/bin/vertices");


    PQfinish(db);
    free_context(ctx);

    return 0;
}
