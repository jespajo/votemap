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

Response serve_vertices(Request *request, Memory_context *context)
{
    Memory_context *ctx = context;

    PGconn *db = connect_to_database(DATABASE_URL);

    Vertex_array *verts = NewArray(verts, ctx);

    //
    // Parse the floats in the query string.
    //
    float upp, x0, y0, x1, y1;
    {
        char  *keys[] = {"upp", "x0", "y0", "x1", "y1"};
        float *nums[] = {&upp, &x0, &y0, &x1, &y1};

        for (s64 i = 0; i < countof(keys); i++) {
            char *num_string = *Get(request->query, keys[i]);
            if (!num_string) {
                char_array *error = get_string(ctx, "Missing query parameter: '%s'\n", keys[i]);
                return (Response){400, .body = error->data, .size = error->count};
            }

            char *end = NULL;
            float num = strtof(num_string, &end);
            if (*num_string == '\0' || *end != '\0') {
                char_array *error = get_string(ctx, "Unexpected value for '%s' query parameter: '%s'\n", keys[i], num_string);
                return (Response){400, .body = error->data, .size = error->count};
            }
            // We parsed the whole string. We're ignoring ERANGE errors. We should do something about NAN and (-)INFINITY. |Robustness

            *(nums[i]) = num;
        }
    }

    // Prepare the parameters to our SQL queries (they are the same for all queries below).
    // Negate the Y values to convert the map units of the browser to the database's coordinate reference system.
    string_array params = {.context = ctx};

    *Add(&params) = get_string(ctx, "%f", upp)->data;
    *Add(&params) = get_string(ctx, "%f", x0)->data;
    *Add(&params) = get_string(ctx, "%f", -y0)->data;
    *Add(&params) = get_string(ctx, "%f", x1)->data;
    *Add(&params) = get_string(ctx, "%f", -y1)->data;

    // Draw the Voronoi map polygons.
    {
        char *query =
            " select st_asbinary(st_collectionextract(geom, 3)) as polygon                          "
            " from (                                                                                "
            "     select st_makevalid(st_snaptogrid(new_voronoi, $1::float)) as geom                "
            "     from booths_22                                                                    "
            "     where new_voronoi is not null                                                     "
            "       and new_voronoi && st_setsrid(                                                  "
            "         st_makebox2d(st_point($2::float, $3::float), st_point($4::float, $5::float)), "
            "         3577                                                                          "
            "       )                                                                               "
            "   ) t                                                                                 ";

        Polygon_array *polygons = query_polygons(db, query, &params, ctx);

        for (s64 i = 0; i < polygons->count; i++) {
            Vector4 colour = {0.5+0.3*frand(), 0.5+0.5*frand(), 0.5+0.4*frand(), 1.0};

            draw_polygon(&polygons->data[i], colour, verts);
        }
    }

    // Draw the electorate boundaries as lines.
    {
        char *query =
            " select st_asbinary(t.geom) as path                                                    "
            " from (                                                                                "
            "     select st_simplify(geom, $1::float) as geom                                       "
            "     from electorates_22_topo.edge_data                                                "
            "     where geom && st_setsrid(                                                         "
            "         st_makebox2d(st_point($2::float, $3::float), st_point($4::float, $5::float)), "
            "         3577                                                                          "
            "       )                                                                               "
            "   ) t                                                                                 ";

        Path_array *paths = query_paths(db, query, &params, ctx);

        for (s64 i = 0; i < paths->count; i++) {
            Vector4 colour = {0, 0, 0, 1.0};

            float line_width = 2*upp;

            draw_path(&paths->data[i], line_width, colour, verts);
        }
    }

    verts = copy_verts_in_the_box(verts, x0, y0, x1, y1, ctx);

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

    assert(*Get(result->columns, "json") == 0);
    assert(result->rows.count == 1);
    u8_array *json = &result->rows.data[0].data[0];

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
    add_route(server, GET, "/.*",              &serve_file_insecurely);

    start_server(server);


    PQfinish(database);
    free_context(top_context);
    return 0;
}
