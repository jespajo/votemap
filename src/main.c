#include <stdio.h>
#include <string.h>

#include "draw.h"
#include "pg.h"

void add_verts(Vertex_array *array, Vertex_array *appendage)
{
    for (s64 i = 0; i < appendage->count; i++) {
        *Add(array) = appendage->data[i];
    }
}

int main()
{
    Memory_Context *ctx = new_context(NULL);

    PGconn *db = PQconnectdb("postgres://postgres:postgisclarity@osm.tal/gis");
    if (PQstatus(db) != CONNECTION_OK)  Error("Database connection failed: %s", PQerrorMessage(db));

    Vertex_array *verts = NewArray(verts, ctx);

    Polygon_array *polygons = query_polygons(db, ctx,
        "   SELECT ST_AsBinary(ST_ForcePolygonCCW(swp.way)) AS polygon              "
        "   FROM simplified_water_polygons swp                                      "
        "     JOIN planet_osm_polygon pop ON TRUE                                   "
        "   WHERE pop.name = 'Australia'                                            "
        "   ORDER BY ST_Distance(swp.way, ST_Centroid(pop.way))                     "
        "   LIMIT 300                                                               "
        );
    for (s64 i = 0; i < polygons->count; i++) {
        float   alpha  = 0.75;
        Vector4 colour = {frand(), frand(), frand(), alpha};

        Vertex_array *polygon_verts = draw_polygon(&polygons->data[i], colour, ctx);

        add_verts(verts, polygon_verts);
    }

    Path_array *paths = query_paths(db, ctx,
            "   SELECT ST_AsBinary(way) AS path   "
            "   FROM planet_osm_roads             "
            "   WHERE highway = 'primary'         "
        );
    for (s64 i = 0; i < paths->count; i++) {
        Vector4 colour = {1.0, 1.0, 1.0, 1.0};
        float   width  = 20;

        Vertex_array *path_verts = draw_path(&paths->data[i], width, colour, ctx);

        add_verts(verts, path_verts);
    }

    write_array_to_file(verts, "/home/jpj/src/webgl/bin/vertices");

    PQfinish(db);
    free_context(ctx);

    return 0;
}
