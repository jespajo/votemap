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

float lerp(float a, float b, float t)
{
    return (1-t)*a + t*b;
}

Vector3 lerp_rgb(Vector3 a, Vector3 b, float t)
//|Temporary: Interpolating in RGB is bad unless you're just going to black or white.
{
    float red   = lerp(a.v[0], b.v[0], t);
    float green = lerp(a.v[1], b.v[1], t);
    float blue  = lerp(a.v[2], b.v[2], t);

    return (Vector3){red, green, blue};
}

Vector3 get_colour_from_hash(u64 hash)
// Turn a hash into a colour. Useful for when you want things to be coloured randomly but consistently.
{
    float r = 0.3 + 0.5*((hash & 0xff)/255.0);
    float g = 0.3 + 0.5*((hash>>8 & 0xff)/255.0);
    float b = 0.3 + 0.5*((hash>>16 & 0xff)/255.0);

    return (Vector3){r, g, b};
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
            // We parsed the whole string. We're ignoring ERANGE errors. We should do something about NAN and (-)INFINITY. In fact setting upp to INFINITY causes a segmentation fault. |Bug!

            *(nums[i]) = num;
        }
    }

    // Prepare the parameters to our SQL queries (they are the same for all queries below).
    // Negate the Y values to convert the map units of the browser to the database's coordinate reference system.
    string_array params = {.context = ctx};
    {
        *Add(&params) = get_string(ctx, "%f", upp)->data;
        *Add(&params) = get_string(ctx, "%f", x0)->data;
        *Add(&params) = get_string(ctx, "%f", -y0)->data;
        *Add(&params) = get_string(ctx, "%f", x1)->data;
        *Add(&params) = get_string(ctx, "%f", -y1)->data;
    }

    // Get the election ID from the request and add it to the SQL query parameters.
    {
        char **election = Get(request->query, "election");
        if (!*election) {
            char_array *error = get_string(ctx, "Missing query parameter 'election'.\n");
            return (Response){400, .body = error->data, .size = error->count};
        }
        //|Todo: Validation of the election ID?

        *Add(&params) = *election;
    }

    // Draw the election boundaries.
    {
        char *query =
        " select d.name, t.party_id, t.colour,                                                                              "
        "   st_asbinary(st_makevalid(                                                                                       "
        "       st_clipbybox2d(                                                                                             "
        "         st_simplify(d.bounds_faces, $1::float),                                                                   "
        "         st_setsrid(st_makebox2d(st_point($2::float, $3::float), st_point($4::float, $5::float)), 3577)            "
        "       )                                                                                                           "
        "     )) as polygon                                                                                                 "
        " from district d                                                                                                   "
        "   left join (                                                                                                     "
        "     select v.election_id, v.district_id, p.id as party_id, p.colour                                               "
        "     from contest_vote v                                                                                           "
        "       inner join candidate c on (c.election_id = v.election_id and c.district_id = v.district_id and v.candidate_id = c.id) "
        "       inner join party p on (p.election_id = v.election_id and c.party_id = p.id)                                 "
        "     where v.count_type = '2CP' and v.elected                                                                      "
        "   ) t on (t.election_id = d.election_id and t.district_id = d.id)                                                 "
        " where d.bounds && st_setsrid(st_makebox2d(st_point($2::float, $3::float), st_point($4::float, $5::float)), 3577)  "
        "   and d.election_id = $6::int                                                                                     "
        // Order by the size of the face's bounding box. This is so that larger polygons don't cover smaller
        // ones, because we don't draw inner rings yet. |Todo
        " order by st_area(box2d(d.bounds)) desc                                                                            "
        ;

        Postgres_result *result = query_database(db, query, &params, ctx);

        int polygons_column = *Get(&result->columns, "polygon");
        int name_column     = *Get(&result->columns, "name");
        int party_id_column = *Get(&result->columns, "party_id");
        int colour_column   = *Get(&result->columns, "colour");
        if (polygons_column < 0)  Fatal("Couldn't find a \"polygon\" column in the results.");
        if (name_column < 0)      Fatal("Couldn't find a \"name\" column in the results.");
        if (party_id_column < 0)  Fatal("Couldn't find a \"party_id\" column in the results.");
        if (colour_column < 0)    Fatal("Couldn't find a \"colour\" column in the results.");

        for (s64 i = 0; i < result->rows.count; i++) {
            u8_array *polygons_cell = &result->rows.data[i].data[polygons_column];
            u8_array *name_cell     = &result->rows.data[i].data[name_column];
            u8_array *party_id_cell = &result->rows.data[i].data[party_id_column];
            u8_array *colour_cell   = &result->rows.data[i].data[colour_column];

            if (!polygons_cell->count)  continue;

            Polygon_array polygons = {.context = ctx};
            {
                u8 *end_data = NULL;
                parse_polygons(polygons_cell->data, &polygons, &end_data);

                assert(end_data == &polygons_cell->data[polygons_cell->count]);
            }

            char_array *name = copy_char_array_from_cell(name_cell, ctx);

            Vector3 colour;
            if (!party_id_cell->count) {
                // We don't know the winner. Make it grey.
                colour = (Vector3){0.5, 0.5, 0.5};
            } else {
                int party_id = (int)get_u32_from_cell(party_id_cell);

                if (party_id < 0) {
                    colour = (Vector3){0.3, 0.3, 0.3}; // Independents are dark grey.
                } else {
                    u32 colour_u32 = get_u32_from_cell(colour_cell);

                    if (colour_u32 == 0) {
                        // We know who won, but we haven't got a colour for this party in the database. Make it light grey.
                        colour = (Vector3){0.7, 0.7, 0.7};
                    } else {
                        // We know who won and we have a colour.
                        int r = colour_u32 >> 16 & 0xff;
                        int g = colour_u32 >> 8 & 0xff;
                        int b = colour_u32 & 0xff;

                        colour = (Vector3){r/255.0, g/255.0, b/255.0};
                    }
                }
            }

            for (s64 j = 0; j < polygons.count; j++)  draw_polygon(&polygons.data[j], colour, verts);
        }
    }

    // Draw the topology lines.
    {
        char *query =
        " select st_asbinary(st_simplify(geom, $1::float)) as path                          "
        " from district_topo.edge                                                           "
        " where edge_id in (                                                                "
        "     select unnest(edge_ids)                                                       "
        "     from district d                                                               "
        "     where election_id = $6::int                                                   "
        "   )                                                                               "
        "   and geom && st_setsrid(                                                         "
        "     st_makebox2d(st_point($2::float, $3::float), st_point($4::float, $5::float)), "
        "     3577                                                                          "
        "   )                                                                               "
        ;

        Path_array *paths = query_paths(db, query, &params, ctx);

        for (s64 i = 0; i < paths->count; i++) {
            Vector3 colour = {0.8, 0.8, 0.8};
            float line_width = 1.5*upp;

            draw_path(&paths->data[i], line_width, colour, verts);
        }
    }

    if (verts->count)  verts = copy_verts_in_the_box(verts, x0, y0, x1, y1, ctx);

    Response response = {200, .body = verts->data, .size = verts->count*sizeof(Vertex)};

    response.headers = (string_dict){.context = ctx};
    *Set(&response.headers, "content-type") = "application/octet-stream";

    return response;
}

Response serve_labels(Request *request, Memory_context *context)
{
    Memory_context *ctx = context;

    PGconn *db = connect_to_database(DATABASE_URL);

    string_array params = {.context = ctx};

    // Get the election ID from the request and add it to the SQL query parameters.
    {
        char **election = Get(request->query, "election");
        if (!*election) {
            char_array *error = get_string(ctx, "Missing query parameter 'election'.\n");
            return (Response){400, .body = error->data, .size = error->count};
        }
        //|Todo: Validation of the election ID?

        *Add(&params) = *election;
    }

    char *query =
    " select jsonb_agg(                        "
    "     jsonb_build_object(                  "
    "       'name', name,                      "
    "       'centroid', jsonb_build_object(    "
    "         'x', round(st_x(centroid)),      "
    "         'y', round(-st_y(centroid))      "
    "       ),                                 "
    "       'box', jsonb_build_array(          "
    "         jsonb_build_object(              "
    "           'x', st_xmin(box),             "
    "           'y', -st_ymax(box)             "
    "         ),                               "
    "         jsonb_build_object(              "
    "           'x', st_xmax(box),             "
    "           'y', -st_ymin(box)             "
    "         )                                "
    "       )                                  "
    "     )                                    "
    "   )::text as json                        "
    " from (                                   "
    "     select name,                         "
    "       st_centroid(bounds) as centroid,   "
    "       box2d(bounds) as box               "
    "     from district                        "
    "     where election_id = $1::int          "
    "     order by st_area(bounds) desc        "
    "   ) t                                    "
    ;

    Postgres_result *result = query_database(db, query, &params, ctx);

    assert(*Get(&result->columns, "json") == 0);
    assert(result->rows.count == 1);
    u8_array *json = &result->rows.data[0].data[0];

    Response response = {200, .body = json->data, .size = json->count};

    response.headers = (string_dict){.context = ctx};
    *Set(&response.headers, "content-type") = "application/json";

    return response;
}

int main()
{
    u32  const ADDR    = 0xac1180e0; // 172.17.128.224 |Todo: Use getaddrinfo().
    u16  const PORT    = 6008;
    bool const VERBOSE = false;

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
