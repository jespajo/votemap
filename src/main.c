#include <stdio.h>
#include <string.h>

#include "draw.h"
#include "pg.h"

int main()
{
    Memory_Context *ctx = new_context(NULL);

    PGconn *db = PQconnectdb("postgres://postgres:postgisclarity@osm.tal/gis");
    if (PQstatus(db) != CONNECTION_OK)  Error("Database connection failed: %s", PQerrorMessage(db));

    Vertex_array *verts = NewArray(verts, ctx);

    Polygon_array *polygons = query_polygons(db, ctx,
        "   SELECT ST_AsBinary(ST_ForcePolygonCCW(ST_Simplify(ST_Union(geom), 3000))) AS polygon    "
        "   FROM aust_coast                                                                         "
        "   WHERE feat_code = 'mainland'                                                            "
        );
    for (s64 i = 0; i < polygons->count; i++) {
        float   alpha  = 0.75;
        Vector4 colour = {frand(), frand(), frand(), alpha};

        Vertex_array *polygon_verts = draw_polygon(&polygons->data[i], colour, ctx);

        add_verts(verts, polygon_verts);
    }

    write_array_to_file(verts, "/home/jpj/src/webgl/bin/vertices");

    PQfinish(db);
    free_context(ctx);

    return 0;
}
