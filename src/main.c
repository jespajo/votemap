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

    // Draw the Voronoi map polygons.
    {
        char *query = load_text_file("queries/voronoi.sql", ctx)->data;

        Postgres_result *result = query_database(db, query, &params, ctx);
        if (!result->rows.count)  Fatal("A query for polygons returned no results.");

        int booth_id_column = *Get(&result->columns, "booth_id");
        int polygons_column = *Get(&result->columns, "polygon");
        int fraction_column = *Get(&result->columns, "fraction_of_votes");
        int colour_column   = *Get(&result->columns, "colour");
        if (booth_id_column < 0)  Fatal("Couldn't find a \"booth_id\" column in the results.");
        if (polygons_column < 0)  Fatal("Couldn't find a \"polygon\" column in the results.");
        if (fraction_column < 0)  Fatal("Couldn't find a \"fraction_of_votes\" column in the results.");
        if (colour_column < 0)    Fatal("Couldn't find a \"colour\" column in the results.");

        //
        // Draw the polygons.
        //
        for (s64 i = 0; i < result->rows.count; i++) {
            u8_array *booth_id_cell = &result->rows.data[i].data[booth_id_column];
            u8_array *polygons_cell = &result->rows.data[i].data[polygons_column];
            u8_array *fraction_cell = &result->rows.data[i].data[fraction_column];
            u8_array *colour_cell   = &result->rows.data[i].data[colour_column];

            u32 booth_id = get_u32_from_cell(booth_id_cell); //|Debug

            Polygon_array polygons = {.context = ctx};
            {
                assert(polygons_cell->count > 0);

                u8 *end_data = NULL;
                parse_polygons(polygons_cell->data, &polygons, &end_data);

                assert(end_data == &polygons_cell->data[polygons_cell->count]);
            }

            Vector3 colour;
            if (colour_cell->count) {
                u32   parsed_colour   = get_u32_from_cell(colour_cell);
                float parsed_fraction = get_float_from_cell(fraction_cell);//|Incomplete: Use this!

                //|Todo: Interpolate in HSL at least.
                int r = (parsed_colour & 0xff0000) >> 16;
                int g = (parsed_colour & 0x00ff00) >> 8;
                int b = (parsed_colour & 0x0000ff);

                colour = lerp_rgb((Vector3){1,1,1}, (Vector3){r,g,b}, parsed_fraction);
            } else {
                colour = (Vector3){0.5, 0.5, 0.5};
            }

            for (s64 j = 0; j < polygons.count; j++)  draw_polygon(&polygons.data[j], colour, verts);
        }
    }

    // Draw some rivers.
    {
        char *query =
            " select st_asbinary(                         "
            "     st_collectionextract(                   "
            "       st_makevalid(                         "
            "         st_force2d(                         "
            "           st_simplify(way, $1::float)       "
            "         )                                   "
            "       ), 3                                  "
            "     )                                       "
            "   ) as polygon                              "
            " from planet_osm_polygon                     "
            " where (                                     "
            "     waterway in ('dock', 'riverbank')       "
            "     or landuse in ('reservoir', 'basin')    "
            "     or \"natural\" in ('water', 'glacier')  "
            "   )                                         "
            "   and building is null                      "
            "   and way_area > ($1::float * $1::float)    "
            "   and way && st_setsrid(                    "
            "     st_makebox2d(                           "
            "       st_point($2::float, $3::float),       "
            "       st_point($4::float, $5::float)        "
            "     ),                                      "
            "     3577                                    "
            "   )                                         ";

        Postgres_result *result = query_database(db, query, &params, ctx);

        int polygons_column = *Get(&result->columns, "polygon");
        if (polygons_column < 0)  Fatal("Couldn't find a \"polygon\" column in the results.");

        for (s64 i = 0; i < result->rows.count; i++) {
            u8_array *polygons_cell = &result->rows.data[i].data[polygons_column];

            if (!polygons_cell->count)  continue;

            Polygon_array polygons = {.context = ctx};
            {
                u8 *end_data = NULL;
                parse_polygons(polygons_cell->data, &polygons, &end_data);

                assert(end_data == &polygons_cell->data[polygons_cell->count]);
            }

            Vector3 colour = {0.1, 0.1, 0.1};

            for (s64 j = 0; j < polygons.count; j++)  draw_polygon(&polygons.data[j], colour, verts);
        }
    }

    // Draw some roads.
    if (upp < 10) {
        char *query =
        "  select st_asbinary(                                  "
        "      st_makevalid(                                    "
        "        st_force2d(st_simplify(way, $1::float))        "
        "      )                                                "
        "    ) as path                                          "
        "  from planet_osm_line                                 "
        "    where (name is not null or ref is not null)        "
        "    and (highway is not null or railway is not null)   "
        "    and way && st_setsrid(                             "
        "      st_makebox2d(                                    "
        "        st_point($2::float, $3::float),                "
        "        st_point($4::float, $5::float)                 "
        "      ),                                               "
        "      3577                                             "
        "    )                                                  ";

        Path_array *paths = query_paths(db, query, &params, ctx);

        for (s64 i = 0; i < paths->count; i++) {
            Vector3 colour = {0.3, 0.3, 0.3};
            float line_width = 1*upp;

            draw_path(&paths->data[i], line_width, colour, verts);
        }
    } else if (upp < 200) {
        char *query =
        "  select st_asbinary(                                  "
        "      st_makevalid(                                    "
        "        st_force2d(st_simplify(way, $1::float))        "
        "      )                                                "
        "    ) as path                                          "
        "  from planet_osm_roads                                "
        "    where (name is not null or ref is not null)        "
        "    and (highway is not null or railway is not null)   "
        "    and way && st_setsrid(                             "
        "      st_makebox2d(                                    "
        "        st_point($2::float, $3::float),                "
        "        st_point($4::float, $5::float)                 "
        "      ),                                               "
        "      3577                                             "
        "    )                                                  ";

        Path_array *paths = query_paths(db, query, &params, ctx);

        for (s64 i = 0; i < paths->count; i++) {
            Vector3 colour = {0.3, 0.3, 0.3};
            float line_width = 1*upp;

            draw_path(&paths->data[i], line_width, colour, verts);
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
            "       and (left_face != 0 and right_face != 0)                                        "
            "   ) t                                                                                 ";

        Path_array *paths = query_paths(db, query, &params, ctx);

        for (s64 i = 0; i < paths->count; i++) {
            Vector3 colour = {0, 0, 0};

            float line_width = 2*upp;

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

    assert(*Get(&result->columns, "json") == 0);
    assert(result->rows.count == 1);
    u8_array *json = &result->rows.data[0].data[0];

    Response response = {200, .body = json->data, .size = json->count};

    response.headers = (string_dict){.context = ctx};
    *Set(&response.headers, "content-type") = "application/json";

    return response;
}

//|Todo: move to json.h
JSON_object **set_json_object(JSON_object **parent, char *key)
{
    JSON_value *parent_value = (JSON_value *)((u8 *)parent - offsetof(JSON_value, object));
    assert(parent_value->type == JSON_OBJECT);

    JSON_value *value = Set(*parent, key);
    value->type = JSON_OBJECT;
    return &value->object;
}
char_array **set_json_string(JSON_object **parent, char *key)
{
    JSON_value *parent_value = (JSON_value *)((u8 *)parent - offsetof(JSON_value, object));
    assert(parent_value->type == JSON_OBJECT);

    JSON_value *value = Set(*parent, key);
    value->type = JSON_STRING;
    return &value->string;
}

Response serve_parties(Request *request, Memory_context *context)
{
    Memory_context *ctx = context;

    PGconn *db = connect_to_database(DATABASE_URL);

    //|Temporary. Some things to remember about this query.
    //| - It groups by multiple columns, which we assume are all dependent on party_id.
    //|   So if we gave any independent a colour, it would return an extra row for that independent.
    //|   Currently the independents are grouped because they all have the same colour.
    //| - For independents, it returns empty cells for party_id, party_name and party_code.
    //| - It only returns parties that have a 2CP candidate in some electorate.
    char *query =
        " select party_id, party_name, party_code, colour, count(*) "
        " from candidates_22 c                                      "
        "   join results_house_22 r on r.candidate_id = c.id        "
        " where r.vote_type = '2CP'                                 "
        " group by party_id, party_name, party_code, colour         "
        " order by count desc                                       ";

    Postgres_result *result = query_database(db, query, NULL, ctx);

    int party_id_column   = *Get(&result->columns, "party_id");
    int party_name_column = *Get(&result->columns, "party_name");
    int party_code_column = *Get(&result->columns, "party_code");
    int colour_column     = *Get(&result->columns, "colour");
    assert(party_id_column >= 0);
    assert(party_name_column >= 0);
    assert(party_code_column >= 0);
    assert(colour_column >= 0);

    JSON_value json = {.type = JSON_OBJECT};
    json.object = NewDict(json.object, ctx);

    JSON_object **parties = set_json_object(&json.object, "parties");
    *parties = NewDict(*parties, ctx);

    for (s64 row_index = 0; row_index < result->rows.count; row_index++) {
        u8_array2 *row = &result->rows.data[row_index];

        char party_id[32]; {
            u8_array *cell = &row->data[party_id_column];
            int value = (cell->count) ? (int)get_u32_from_cell(cell) : -1;

            int r = snprintf(party_id, sizeof(party_id), "%d", value);
            assert(0 < r && r < sizeof(party_id));
        }

        JSON_object **party = set_json_object(parties, party_id);
        *party = NewDict(*party, ctx);

        *set_json_string(party, "code") = copy_char_array_from_cell(&row->data[party_code_column], ctx);
        *set_json_string(party, "name") = copy_char_array_from_cell(&row->data[party_name_column], ctx);

        u32 colour = get_u32_from_cell(&row->data[colour_column]);
        *set_json_string(party, "colour") = get_string(ctx, "#%06x", colour);
    }

    char_array *body = get_json_printed(&json, ctx);

    Response response = {200, .body = body->data, .size = body->count};

    response.headers = (string_dict){.context = ctx};
    *Set(&response.headers, "content-type") = "application/json";

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
    add_route(server, GET, "/parties",         &serve_parties);
    add_route(server, GET, "/.*",              &serve_file_insecurely);

    start_server(server);


    PQfinish(database);
    free_context(top_context);
    return 0;
}
