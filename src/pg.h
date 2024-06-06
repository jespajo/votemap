#ifndef PG_H_INCLUDED
#define PG_H_INCLUDED

#include "libpq-fe.h"

#include "map.h"
#include "shapes.h"

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

typedef Dict(u8_array *)    Result_row;
typedef Array(Result_row *) Postgres_result;

Polygon_array *query_polygons(PGconn *db, Memory_Context *context, char *query);
Path_array *query_paths(PGconn *db, Memory_Context *context, char *query);
Postgres_result *query_database_cached(PGconn *db, char *query, Memory_Context *context);

#endif // PG_H_INCLUDED
