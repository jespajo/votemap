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

typedef Array(Vector2)  Path;
typedef Array(Path)     Path_array;
typedef Array(Path)     Polygon; // We follow the convention where the first ring of a polygon is the outer ring. Subsequent rings are holes.
typedef Array(Polygon)  Polygon_array;

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

struct Vertex {
    float x, y;
    float r, g, b, a;
};
typedef struct Vertex  Vertex;
typedef Array(Vertex)  Vertex_array;

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

void write_array_to_file_(void *data, u64 unit_size, s64 count, char *file_name)
{
    FILE *file = fopen(file_name, "wb");

    u64 num_chars_written = fwrite(data, unit_size, count, file);
    assert(num_chars_written > 0);

    fclose(file);
}
#define write_array_to_file(ARRAY, FILE_NAME)  \
    write_array_to_file_((ARRAY)->data, sizeof((ARRAY)->data[0]), (ARRAY)->count, (FILE_NAME))

Polygon_array *query_polygons(PGconn *db, Memory_Context *context, char *query)
{
#define QueryError(...)  (Error(__VA_ARGS__), NULL)
    PGresult *result = PQexecParams(db, query, 0, NULL, NULL, NULL, NULL, 1);
    if (PQresultStatus(result) != PGRES_TUPLES_OK)  return QueryError("Query failed: %s", PQerrorMessage(db));

    int column = PQfnumber(result, "polygon");
    if (column < 0)  return QueryError("We couldn't find a \"Polygon\" column in the results.");

    int num_tuples = PQntuples(result);
    if (num_tuples == 0)  return QueryError("A query for polygons returned no results.");

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
        assert(wkb_type == WKB_POLYGON);

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
                *Add(&ring) = (Vector2){(float)x, (float)y};
            }
            *Add(&polygon) = ring;
        }
        *Add(polygons) = polygon;

        // Check that the number of bytes parsed by this function is equal to the Postgres reported size.
        s64 num_bytes_parsed = d - data;
        assert(num_bytes_parsed == PQgetlength(result, row, column));
    }

    PQclear(result);

    return polygons;
#undef QueryError
}

bool points_are_clockwise(Vector2 *points, s64 num_points)
// This function assumes that we're working with a bottom-left origin, with Y increasing upwards.
{
    if (num_points < 3) {
        Error("points_are_clockwise() needs at least 3 points. We only have %ld.", num_points);
    }
    float sum = 0;
    for (int i = 0; i < num_points-1; i++) {
        float *p = points[i].v;
        float *q = points[i+1].v;
        sum += (q[0] - p[0])/(q[1] + p[1]);
    }
    float *p = points[0].v;
    float *q = points[num_points-1].v;
    sum += (p[0] - q[0])/(p[1] + q[1]);
    return sum < 0;
}

bool same_point(Vector2 p, Vector2 q)
{
    if (p.v[0] != q.v[0])  return false;
    if (p.v[1] != q.v[1])  return false;
    return true;
}

bool is_triangle(Path triangle)
{
    // @Todo: Check the three points aren't colinear?
    if (triangle.count == 3)  return true;
    if (triangle.count == 4) {
        Vector2 first = triangle.data[0];
        Vector2 last  = triangle.data[3];
        if (same_point(first, last))  return true;
    }
    return false;
}

bool point_in_triangle(Vector2 point, Path triangle)
{
    assert(is_triangle(triangle));

    float *p  = point.v;
    float *t1 = triangle.data[0].v;
    float *t2 = triangle.data[1].v;
    float *t3 = triangle.data[2].v;

    float d1 = (p[0] - t1[0])*(t2[1] - t1[1]) - (p[1] - t1[1])*(t2[0] - t1[0]);
    float d2 = (p[0] - t2[0])*(t3[1] - t2[1]) - (p[1] - t2[1])*(t3[0] - t2[0]);
    float d3 = (p[0] - t3[0])*(t1[1] - t3[1]) - (p[1] - t3[1])*(t1[0] - t3[0]);

    bool negative = d1 < 0 || d2 < 0 || d3 < 0;
    bool positive = d1 > 0 || d2 > 0 || d3 > 0;

    return !(negative && positive);
}

Path_array *triangulate_polygon(Polygon *polygon, Memory_Context *ctx)
// Assumptions:
// - The polygon's first ring is its outer ring with points in counter-clockwise order.
// - Subsequent rings are holes with points in clockwise order.
// - @Todo: Do we assume that the first and last points are the same? Not the same? Either way?
{
    Path_array *triangles = NewArray(triangles, ctx);

    // @Todo: If the polygon has holes, turn it into one big ring.
    // But for now we'll just take the firth path, the outer ring.
    Path *ring = &polygon->data[0];

    // The points as a whole should go anticlockwise.
    assert(!points_are_clockwise(ring->data, ring->count));

    // This is like a circular linked list but all the links are stored in a separate array, and
    // instead of being pointers, they're the indices of the next points in the ring. This makes it
    // easy to iterate over all the points starting from any one: just call `i = next[i]` at the end
    // of each loop until you encouter the current point again. The other advantage is that, as we
    // "chop off ears" in the process of turning polygons into triangles, we just update the
    // preceding index to point to the one after the removed one.
    int *next = New(ring->count, int, ctx);
    for (int i = 0; i < ring->count-1; i++)  next[i] = i + 1;
    next[ring->count-1] = 0; // { 1, 2, 3, ..., 0 }

    int num_triangles = ring->count-2;
    {
        int i1 = 0;
        int i0 = 0;

        while (triangles->count < num_triangles) {
            int v1 = i1;
            int v2 = next[v1];
            int v3 = next[v2];

            bool remove = true;

            Path ear = {.context = ctx};

            *Add(&ear) = ring->data[v1];
            *Add(&ear) = ring->data[v2];
            *Add(&ear) = ring->data[v3];
            *Add(&ear) = ring->data[v1]; // Repeat the first point. @Speed, but might be useful for is_triangle verification??

            if (points_are_clockwise(ear.data, 3)) {
                // The ear is not on the outer hull.
                remove = false;
            } else {
                // The ear is on the outer hull. Check all other points to see if any of them is inside this ear.
                int i2 = next[v3];
                while (i2 != v1) {
                    if (point_in_triangle(ring->data[i2], ear)) {
                        remove = false; // There is another point inside the ear.
                        break;
                    }
                    i2 = next[i2];
                }
            }

            if (!remove) {
                i1 = v2;
                // Make sure we don't loop around the ring forever. If there are no triangles yet, we shouldn't be back at the start where i1 == 0.
                assert(triangles->count || i1);
                // If we have partially triangulated, return what we've got. @Todo: Control what to return if triangulation fails with a parameter?
                if (!(!triangles->count || i1 != i0)) {
                    //Error("Partial triangulation. Used %d out of %d vertices.", triangles->count, ring->count); // @Fixme.
                    return triangles;
                }
                continue;
            }

            // Off with the ear!

            *Add(triangles) = ear;

            next[v1] = v3;
            i1       = v3;
            i0       = v3;
        }
    }

    assert(triangles->count == num_triangles);
    return triangles;
}

Vertex_array *polygon_to_vertices(Polygon *polygon, Vector4 colour, Memory_Context *ctx)
{
    Vertex_array *vertices = NewArray(vertices, ctx);

    float r = colour.v[0];
    float g = colour.v[1];
    float b = colour.v[2];
    float a = colour.v[3];

    Path_array *triangles = triangulate_polygon(polygon, ctx);

    for (s64 i = 0; i < triangles->count; i++) {
        Path *triangle = &triangles->data[i];

        for (int j = 0; j < 3; j++) {
            float x = triangle->data[j].v[0];
            float y = triangle->data[j].v[1];

            *Add(vertices) = (Vertex){x, y, r, g, b, a};
        }
    }

    return vertices;
}

float frand()
{
    return rand()/(float)RAND_MAX;
}

int main()
{
    Memory_Context *ctx = new_context(NULL);

    PGconn *db = PQconnectdb("postgres://postgres:postgisclarity@osm.tal/gis");
    if (PQstatus(db) != CONNECTION_OK)  Error("Database connection failed: %s", PQerrorMessage(db));

    Polygon_array *polygons = query_polygons(db, ctx,
        //"SELECT ST_AsBinary(ST_GeomFromEWKB(way)) AS polygon FROM planet_osm_polygon WHERE name = 'Macquarie River' and ST_GeometryType(way) = 'ST_Polygon'");
        //"SELECT ST_AsBinary(ST_GeomFromEWKB(ST_ForcePolygonCW(way))) AS polygon FROM planet_osm_polygon WHERE ST_GeometryType(way) = 'ST_Polygon' LIMIT 500");

        "SELECT ST_AsBinary(ST_GeomFromEWKB(p1.way)) AS polygon         "
        "FROM planet_osm_polygon p1                                     "
        "  JOIN planet_osm_polygon p2 ON ST_Within(p1.way, p2.way)      "
        "WHERE p2.name = 'City of Melbourne'                            "
        "  AND ST_GeometryType(p1.way) = 'ST_Polygon'                   "
        "ORDER BY ST_Area(p1.way) DESC                                  "
        "LIMIT 500                                                      "
        );

    Vertex_array *vertices = NewArray(vertices, ctx);

    for (s64 i = 0; i < polygons->count; i++) {
        float   alpha  = 0.75;
        Vector4 colour = {frand(), frand(), frand(), alpha};

        Vertex_array *polygon_verts = polygon_to_vertices(&polygons->data[i], colour, ctx);

        for (s64 j = 0; j < polygon_verts->count; j++)  *Add(vertices) = polygon_verts->data[j];
    }

    // Write vertices to file.
    write_array_to_file(vertices, "/home/jpj/src/webgl/bin/vertices");

    PQfinish(db);
    free_context(ctx);

    return 0;
}
