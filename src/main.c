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
typedef Array(Linestring) Polygon; // We follow the convention where the first ring is the outer ring. Subsequent rings are holes.

enum WKBByteOrder {
    WKB_BIG_ENDIAN    = 0,
    WKB_LITTLE_ENDIAN = 1,
};

enum WKBGeometryType {
    WKB_POINT              = 1,
    WKB_LINESTRING         = 2,
    WKB_POLYGON            = 3,
    WKB_MULTIPOINT         = 4,
    WKB_MULTILINESTRING    = 5,
    WKB_MULTIPOLYGON       = 6,
    WKB_GEOMETRYCOLLECTION = 7,
};

Polygon *polygon_from_wkb_geometry(char *geometry, Memory_Context *context)
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

PGresult *pq(PGconn *conn, char *query)
// A simplified function for making a database query and returning the results as binary.
{
    PGresult *result = PQexecParams(conn, query, 0, NULL, NULL, NULL, NULL, 1);
    if (PQresultStatus(result) != PGRES_TUPLES_OK)  Error("Query failed: %s", PQerrorMessage(conn));

    return result;
}

int main()
{
    Memory_Context *ctx = new_context(NULL);

    PGconn *conn = PQconnectdb("postgres://postgres:postgisclarity@osm.tal/gis");
    if (PQstatus(conn) != CONNECTION_OK)  Error("Database connection failed: %s", PQerrorMessage(conn));

    Polygon *polygon; // @Todo: Get all of em.
    {
        PGresult *query = pq(conn, "SELECT ST_AsBinary(ST_PolygonFromText('POLYGON((0 300, 20 250, 100 200, 50 0, 0 300))')) AS polygon");
        //PGresult *query = pq(conn, "SELECT ST_AsBinary(way) AS polygon, ST_GeometryType(way) as type FROM simplified_water_polygons ORDER BY RANDOM() LIMIT 1");
        if (!query)  Error("Query error.");

        int num_tuples = PQntuples(query);
        assert(num_tuples >= 1);

        int polygon_column = 0;
        assert(PQfnumber(query, "polygon") == polygon_column);

        char *geometry = PQgetvalue(query, 0, polygon_column);

        polygon = polygon_from_wkb_geometry(geometry, ctx); // @Todo: Check that the number of bytes parsed by this function is equal to the Postgres tuple size (not sure of the PQ function to check this but there must be one).

        PQclear(query);
    }

    PQfinish(conn);

    free_context(ctx);
    return 0;
}
