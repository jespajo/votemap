#include "draw.h"
#include "http.h"
#include "json.h"
#include "map.h"
#include "pg.h"
#include "strings.h"

#define DATABASE_URL "postgres://postgres:postgisclarity@osm.tal/gis"

float frand()
{
    return rand()/(float)RAND_MAX;
}

Vertex_array *clip_triangle_verts(Vertex_array *verts, float min_x, float min_y, float max_x, float max_y, Memory_context *context)
//|Temporary. Misnamed (doesn't clip to a clean rectangle), misplaced (should go in draw.h), probably stupid. Also should operate on triangles instead of verts.
{
    Vertex_array *result = NewArray(result, context);

    assert(verts->count % 3 == 0);

    for (s64 i = 0; i < verts->count; i += 3) {
        Vertex *v = &verts->data[i];

        if (v[0].x < min_x && v[1].x < min_x && v[2].x < min_x)  continue;
        if (v[0].y < min_y && v[1].y < min_y && v[2].y < min_y)  continue;
        if (v[0].x > max_x && v[1].x > max_x && v[2].x > max_x)  continue;
        if (v[0].y > max_y && v[1].y > max_y && v[2].y > max_y)  continue;

        *Add(result) = v[0];
        *Add(result) = v[1];
        *Add(result) = v[2];
    }

    return result;
}

Response serve_vertices(Request *request, Memory_context *context)
{
    Memory_context *ctx = context;

    PGconn *db = connect_to_database(DATABASE_URL);

    bool show_voronoi = request->query && *Get(request->query, "voronoi");

    float metres_per_pixel = 8000.0;
    {
        char *upp = *Get(request->query, "upp");

        if (upp)  metres_per_pixel = strtof(upp, NULL); //|Todo: Error-handling.
    }

    Vertex_array *verts = NewArray(verts, ctx);

    if (show_voronoi) {
        // Draw the Voronoi map polygons.
        char *query =
        "   select st_asbinary(st_collectionextract(geom, 3)) as polygon     "
        "   from (                                                           "
        "       select st_makevalid(st_snaptogrid(topo, $1::float)) as geom  "
        "       from booths_22                                               "
        "       where topo is not null                                       "
        "     ) t                                                            ";

        string_array params = {.context = ctx};

        *Add(&params) = get_string(ctx, "%f", metres_per_pixel)->data;

        Polygon_array *polygons = query_polygons(db, query, &params, ctx);

        for (s64 i = 0; i < polygons->count; i++) {
            Vector4 colour = {0.5+0.3*frand(), 0.5+0.5*frand(), 0.5+0.4*frand(), 1.0};

            draw_polygon(&polygons->data[i], colour, verts);
        }
    } else {
        // Draw electorate boundaries as polygons.
        char *query =
          " select st_asbinary(st_buildarea(topo)) as polygon  "
          " from (                                             "
          "     select st_simplify(topo, $1::float) as topo    "
          "     from electorates_22                            "
          "   ) t                                              ";

        string_array params = {.context = ctx};

        *Add(&params) = get_string(ctx, "%f", metres_per_pixel)->data;

        Polygon_array *polygons = query_polygons(db, query, &params, ctx);

        for (s64 i = 0; i < polygons->count; i++) {
            float shade = frand();
            Vector4 colour = {0.9*shade, 0.4*shade, 0.8*shade, 1.0};

            draw_polygon(&polygons->data[i], colour, verts);
        }
    }

    // Draw electorate boundaries as lines.
    {
        char *query =
          " select st_asbinary(t.geom) as path                 "
          " from (                                             "
          "     select st_simplify(geom, $1::float) as geom    "
          "     from electorates_22_topo.edge_data             "
          "   ) t                                              ";

        string_array params = {.context = ctx};

        *Add(&params) = get_string(ctx, "%f", metres_per_pixel)->data;

        Path_array *paths = query_paths(db, query, &params, ctx);

        for (s64 i = 0; i < paths->count; i++) {
            Vector4 colour = {0, 0, 0, 1.0};

            float line_width = metres_per_pixel;

            draw_path(&paths->data[i], line_width, colour, verts);
        }
    }

    // Clip the vertices to the box if one is given.
    {
        char *x0 = *Get(request->query, "x0");
        char *y0 = *Get(request->query, "y0");
        char *x1 = *Get(request->query, "x1");
        char *y1 = *Get(request->query, "y1");

        if (x0 && y0 && x1 && y1) {
            //|Todo: Error-check this float parsing.
            float min_x = strtof(x0, NULL);
            float min_y = strtof(y0, NULL);
            float max_x = strtof(x1, NULL);
            float max_y = strtof(y1, NULL);

            verts = clip_triangle_verts(verts, min_x, min_y, max_x, max_y, ctx);
        }
    }

    Response response = {200, .body = verts->data, .size = verts->count*sizeof(Vertex)};

    response.headers = NewDict(response.headers, ctx);

    *Set(response.headers, "content-type") = "application/octet-stream";

    return response;
}

Response serve_labels(Request *request, Memory_context *context)
{
    Memory_context *ctx = context;

    PGconn *db = connect_to_database(DATABASE_URL);

    char *query =
      " select jsonb_build_object(                                                               "
      "     'labels', jsonb_agg(                                                                 "
      "         jsonb_build_object(                                                              "
      "             'text', upper(name),                                                         "
      "             'pos', jsonb_build_array(round(st_x(centroid)), round(-st_y(centroid)))      "
      "           )                                                                              "
      "       )                                                                                  "
      "   )::text as json                                                                        "
      " from (                                                                                   "
      "     select name,                                                                         "
      "       st_centroid(geom) as centroid                                                      "
      "     from electorates_22                                                                  "
      "     order by st_area(geom) desc                                                          "
      "   ) t;                                                                                   ";

    Postgres_result *result = query_database(db, query, NULL, ctx);

    u8_array *json = *Get(result->data[0], "json");

    Response response = {200, .body = json->data, .size = json->count};

    response.headers = NewDict(response.headers, ctx);

    *Set(response.headers, "content-type") = "application/json";

    return response;
}

int main()
{
    u32  const ADDR    = 0xac1180e0; // 172.17.128.224 |Todo: Use getaddrinfo().
    u16  const PORT    = 6008;
    bool const VERBOSE = true;

    Memory_context *top_context = new_context(NULL);

    PGconn *database = connect_to_database(DATABASE_URL);

    Server *server = create_server(ADDR, PORT, VERBOSE, top_context);

    add_route(server, GET, "/bin/vertices",    &serve_vertices);
    add_route(server, GET, "/bin/labels.json", &serve_labels);
    add_route(server, GET, "/.*",              &serve_file_NEW);

    start_server(server);


    PQfinish(database);
    free_context(top_context);
    return 0;
}
