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

    // Draw land.
    Polygon_array *land_polygons = query_polygons(db, ctx,
        "   SELECT ST_AsBinary(ST_ForcePolygonCCW(ST_Simplify(ST_Union(geom), 3000))) AS polygon    "
        "   FROM aust_coast                                                                         "
        "   WHERE feat_code = 'mainland'                                                            "
        );
    for (s64 i = 0; i < land_polygons->count; i++) {
        Vector4 colour = {0.95, 0.95, 0.95, 1.0}; // Land colour: off-white.

        Vertex_array *polygon_verts = draw_polygon(&land_polygons->data[i], colour, ctx);

        add_verts(verts, polygon_verts);
    }

    // Draw lakes.
    Polygon_array *water_polygons = query_polygons(db, ctx,
        "   SELECT *                                                                        "
        "   FROM (                                                                          "
        "       SELECT ST_AsBinary(ST_ForcePolygonCCW(ST_Simplify(way, 3000))) AS polygon   "
        "       FROM planet_osm_polygon                                                     "
        "       WHERE \"natural\" = 'water'                                                 "
        "         AND way_area > 1000000                                                    "
        "     ) t                                                                           "
        "   WHERE polygon IS NOT NULL                                                       "
        );
    for (s64 i = 0; i < water_polygons->count; i++) {
        Vector4 colour = {0.75, 0.75, 0.75, 1.0}; // Water colour: light grey.

        Vertex_array *polygon_verts = draw_polygon(&water_polygons->data[i], colour, ctx);

        add_verts(verts, polygon_verts);
    }

    // Draw rivers.
    Path_array *river_paths = query_paths(db, ctx,
        " select *                                               "
        " from (                                                 "
        "     select st_asbinary(st_simplify(way, 3000)) as path "
        "     from planet_osm_line                               "
        "     where waterway = 'river'                           "
        "   ) t                                                  "
        " where path is not null                                 "
        );
    for (s64 i = 0; i < river_paths->count; i++) {
        Vector4 colour = {0.75, 0.75, 0.75, 1.0}; // Water colour: light grey.

        Vertex_array *path_verts = draw_path(&river_paths->data[i], 100, colour, ctx);

        add_verts(verts, path_verts);
    }

    write_array_to_file(verts, "/home/jpj/src/webgl/bin/vertices");

    PQfinish(db);
    free_context(ctx);

    return 0;
}
