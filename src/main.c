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

    char_array *query = load_text_file("queries/booths.sql", ctx);

    Polygon_array *polygons = query_polygons(db, ctx, query->data);

    for (s64 i = 0; i < polygons->count; i++) {
        float   shade  = frand();
        Vector4 colour = {0.1*shade, shade, 0.4*shade, 1.0};

        Vertex_array *polygon_verts = draw_polygon(&polygons->data[i], colour, ctx);

        add_verts(verts, polygon_verts);
    }

    write_array_to_file(verts, "/home/jpj/src/webgl/bin/vertices");

    PQfinish(db);
    free_context(ctx);

    return 0;
}
