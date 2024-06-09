#include <string.h>

#include "draw.h"
#include "pg.h"
#include "map.h"
#include "strings.h"

int main()
{
    Memory_Context *ctx = new_context(NULL);

    PGconn *db = PQconnectdb("postgres://postgres:postgisclarity@osm.tal/gis");
    if (PQstatus(db) != CONNECTION_OK)  Error("Database connection failed: %s", PQerrorMessage(db));

    Vertex_array *verts = NewArray(verts, ctx);

    float tolerance = 2000.0; // "Pixel width in metres".
    //float tolerance = 50.0; // No longer fails!

    // Draw electorate boundaries as polygons.
    {
        char *query =
        "   select st_asbinary(st_buildarea(topo)) as polygon               "
        "   from (                                                          "
        "       select st_simplify(topo, $1::float) as topo                 "
        "       from electorates_22                                         "
        "     ) t                                                           ";

        string_array *params = NewArray(params, ctx);

        *Add(params) = get_string(ctx, "%f", tolerance)->data;

        Polygon_array *polygons = query_polygons(db, query, params, ctx);

        for (s64 i = 0; i < polygons->count; i++) {
            Vector4 colour = {frand(), 0.3, 0.5*frand(), 1.0};

            Vertex_array *polygon_verts = draw_polygon(&polygons->data[i], colour, ctx);

            add_verts(verts, polygon_verts);
        }
    }

//    // Draw electorate boundaries as lines.
//    {
//        char *query =
//        "  select st_asbinary(t.geom) as path                      "
//        "  from (                                                  "
//        "      select st_simplify(geom, $1::float) as geom         "
//        "      from electorates_22_topo.edge_data                  "
//        "    ) t                                                   ";
//
//        string_array *params = NewArray(params, ctx);
//
//        *Add(params) = get_string(ctx, "%f", tolerance)->data;
//
//        Path_array *paths = query_paths(db, query, params, ctx);
//
//        for (s64 i = 0; i < paths->count; i++) {
//            Vector4 colour = {0.9, 0.9, 0.9, 1.0};
//            float   width  = tolerance/8;
//
//            Vertex_array *path_verts = draw_path(&paths->data[i], width, colour, ctx);
//
//            add_verts(verts, path_verts);
//        }
//    }

    write_array_to_file(verts, "/home/jpj/src/webgl/bin/vertices");

    PQfinish(db);
    free_context(ctx);

    return 0;
}
