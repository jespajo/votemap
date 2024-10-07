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

    // Get the election boundaries to draw from the query.
    {
        char **year = Get(request->query, "year");
        if (!*year) {
            char_array *error = get_string(ctx, "Missing query parameter 'year'.\n");
            return (Response){400, .body = error->data, .size = error->count};
        }

        if (!strcmp(*year, "2010"))       *Add(&params) = "15508";
        else if (!strcmp(*year, "2013"))  *Add(&params) = "17496";
        else if (!strcmp(*year, "2016"))  *Add(&params) = "20499";
        else if (!strcmp(*year, "2019"))  *Add(&params) = "24310";
        else if (!strcmp(*year, "2022"))  *Add(&params) = "27966";
        else {
            char_array *error = get_string(ctx, "Unexpected value for 'year': '%s'\n", *year);
            return (Response){400, .body = error->data, .size = error->count};
        }
    }

    // Draw the election boundaries.
    {
        char *query =
        " select name, st_asbinary(st_collectionextract(st_makevalid(st_snaptogrid(bounds, $1::float)), 3)) as polygon       "
        " from electorates                                                                                                   "
        " where bounds && st_setsrid(st_makebox2d(st_point($2::float, $3::float), st_point($4::float, $5::float)), 3577)     "
        "   and election_id = $6::int                                                                                        ";

        Postgres_result *result = query_database(db, query, &params, ctx);

        int polygons_column = *Get(&result->columns, "polygon");
        int name_column     = *Get(&result->columns, "name");
        if (polygons_column < 0)  Fatal("Couldn't find a \"polygon\" column in the results.");
        if (name_column < 0)      Fatal("Couldn't find a \"name\" column in the results.");

        for (s64 i = 0; i < result->rows.count; i++) {
            u8_array *polygons_cell = &result->rows.data[i].data[polygons_column];
            u8_array *name_cell     = &result->rows.data[i].data[name_column];

            if (!polygons_cell->count)  continue;

            Polygon_array polygons = {.context = ctx};
            {
                u8 *end_data = NULL;
                parse_polygons(polygons_cell->data, &polygons, &end_data);

                assert(end_data == &polygons_cell->data[polygons_cell->count]);
            }

            char_array *name = copy_char_array_from_cell(name_cell, ctx);

            u64 hash = hash_string(name->data);
            float r = 0.5 + 0.5*((float)(hash & 0xff)/255);
            float g = 0.5 + 0.5*((float)(hash>>8 & 0xff)/255);
            float b = 0.5 + 0.5*((float)(hash>>16 & 0xff)/255);
            Vector3 colour = {r, g, b};

            for (s64 j = 0; j < polygons.count; j++) {
                draw_polygon(&polygons.data[j], colour, verts);
            }
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
