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

    // Draw a Voronoi diagram of polling booth locations in Australia.
    {
        char *query = load_text_file("queries/booths.sql", ctx)->data;

        Polygon_array *polygons = query_polygons(db, query, NULL, ctx);

        for (s64 i = 0; i < polygons->count; i++) {
            float   shade  = frand();
            Vector4 colour = {0.1*shade, shade, 0.4*shade, 1.0};

            Vertex_array *polygon_verts = draw_polygon(&polygons->data[i], colour, ctx);

            add_verts(verts, polygon_verts);
        }
    }

    // Draw electorate boundaries.
    {
        char *query =
        "   select st_asbinary(st_force2d(t.geom)) as path"
        "   from ("
        "       select (st_dump(st_boundary(st_simplify(geom, $1::float)))).geom as geom"
        "       from federal_boundaries_2022"
        "     ) as t"
        ;

        string_array params = {.context = ctx};

        *Add(&params) = "100.0";

        Path_array *paths = query_paths(db, query, &params, ctx);

        for (s64 i = 0; i < paths->count; i++) {
            Vector4 colour = {0.9, 0.9, 0.9, 1.0};
            float   width  = 50;

            Vertex_array *path_verts = draw_path(&paths->data[i], 50, colour, ctx);

            add_verts(verts, path_verts);
        }
    }

    write_array_to_file(verts, "/home/jpj/src/webgl/bin/vertices");

    PQfinish(db);
    free_context(ctx);

    return 0;
}
