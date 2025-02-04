#include <math.h> // isfinite()

#include "draw.h"
#include "http.h"
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

typedef struct Tile_info Tile_info;
struct Tile_info {
    bool    parse_success;
    char   *fail_reason;

    enum Tile_theme {
        NORMAL = 1,
        DARK,
        HIGHLIGHT_DISTRICT,
    }       theme;

    int     election_id;
    int     district_id; // If a district is highlighted.

    float   upp;
    float   x0;
    float   y0;
    float   x1;
    float   y1;
};

Tile_info parse_tile_request(Request *request)
{
    Tile_info result = {0};

    // Valid paths:
    //
    //      vertices/27966
    //      vertices/27966-dark
    //      vertices/27966-170

    char *p = request->path.data;
    // These should never fail because the route-matcher regex looked at the path before we got here.
    assert(starts_with(p, "/vertices/"));
    assert(request->path.count > lengthof("/vertices/"));
    p += lengthof("/vertices/");

    // Parse the election ID.
    long n = strtol(p, &p, 10);
    if (!(0 < n && n < INT32_MAX)) {
        result.fail_reason = "Could not parse an election ID.\n";
        return result;
    }
    result.election_id = n;

    if (*p != '\0') {
        // There is more in the path to parse.
        if (*p != '-') {
            result.fail_reason = "Unexpected character in path after the election ID.\n";
            return result;
        }
        p += 1;

        if (!memcmp(p, "dark", sizeof("dark"))) { // Note this also checks the '\0' at the end.
            result.theme = DARK;
        } else {
            // Otherwise, we expect the ID of a highlighted district.
            long n = strtol(p, &p, 10);
            if (!(0 < n && n < INT32_MAX)) {
                result.fail_reason = "Could not parse a district ID.\n";
                return result;
            }
            if (*p != '\0') {
                result.fail_reason = "Unexpected character in path after the district ID.\n";
                return result;
            }
            result.theme = HIGHLIGHT_DISTRICT;
            result.district_id = n;
        }
    } else {
        result.theme = NORMAL;
    }

    // Parse the floats in the query string.
    {
        char  *keys[] = {"upp", "x0", "y0", "x1", "y1"};
        float *nums[] = {&result.upp, &result.x0, &result.y0, &result.x1, &result.y1};

        for (s64 i = 0; i < countof(keys); i++) {
            char *num_string = *Get(&request->query, keys[i]);
            if (!num_string) {
                result.fail_reason = "The query string is missing at least one of the floats.\n";
                return result;
            }

            char *end = NULL;
            float num = strtof(num_string, &end);
            if (*num_string == '\0' || *end != '\0' || !isfinite(num)) {
                result.fail_reason = "Unexpected value for a float in the query string.\n";
                return result;
            }

            *(nums[i]) = num;
        }
    }

    result.parse_success = true;
    return result;
}

Response serve_vertices(Request *request, Memory_context *context)
{
    Memory_context *ctx = context;

    Vertex_array *verts = NewArray(verts, ctx);

    PG_client db = {DATABASE_URL, .use_cache = true, .keep_alive = true};

    Tile_info tile = parse_tile_request(request);
    if (!tile.parse_success) {
        return (Response){400, .body = tile.fail_reason, .size = strlen(tile.fail_reason)};
    }

    // Prepare the parameters to our SQL queries (they are the same for all queries below).
    // Negate the Y values to convert the map units of the browser to the database's coordinate reference system.
    string_array params = {.context = ctx};
    {
        *Add(&params) = get_string(ctx, "%f", tile.upp)->data;
        *Add(&params) = get_string(ctx, "%f", tile.x0)->data;
        *Add(&params) = get_string(ctx, "%f", -tile.y0)->data;
        *Add(&params) = get_string(ctx, "%f", tile.x1)->data;
        *Add(&params) = get_string(ctx, "%f", -tile.y1)->data;
        *Add(&params) = get_string(ctx, "%d", tile.election_id)->data;
    }

    // Draw the electorate districts as polygons.
    {
        // Order by the size of the face's bounding box. This is so that larger polygons don't cover smaller ones,
        // because we don't draw inner rings yet.
        char *query =
        "  select d.id as district_id, d.name, t.party_id, t.colour,                                                                                   "
        "    st_asbinary(st_collectionextract(st_makevalid(                                                                                            "
        "          st_snaptogrid(st_clipbybox2d(d.bounds_clipped, st_makeenvelope($2::float, $3::float, $4::float, $5::float, 3577)), $1::float        "
        "        )                                                                                                                                     "
        "      ), 3)) as polygon                                                                                                                       "
        "  from district d                                                                                                                             "
        "    left join (                                                                                                                               "
        "      select v.election_id, v.district_id, c.party_id as party_id, p.colour                                                                   "
        "      from contest_vote v                                                                                                                     "
        "        join candidate c on (c.election_id = v.election_id and c.district_id = v.district_id and v.candidate_id = c.id)                       "
        "        left join party p on (p.election_id = v.election_id and c.party_id = p.id)                                                            "
        "      where v.count_type = '2CP' and v.elected                                                                                                "
        "    ) t on (t.election_id = d.election_id and t.district_id = d.id)                                                                           "
        "  where d.bounds_clipped && st_makeenvelope($2::float, $3::float, $4::float, $5::float, 3577)                                                 "
        "    and d.election_id = $6::int                                                                                                               "
        " order by st_area(box2d(d.bounds_clipped)) desc                                                                                               "
        ;

        PG_result *result = query_database(&db, query, &params, ctx);

        int district_id_column = *Get(&result->columns, "district_id");  assert(district_id_column >= 0);
        int polygon_column     = *Get(&result->columns, "polygon");      assert(polygon_column >= 0);
        int name_column        = *Get(&result->columns, "name");         assert(name_column >= 0);
        int party_id_column    = *Get(&result->columns, "party_id");     assert(party_id_column >= 0);
        int colour_column      = *Get(&result->columns, "colour");       assert(colour_column >= 0);

        for (s64 i = 0; i < result->rows.count; i++) {
            u8_array *district_id_cell = &result->rows.data[i].data[district_id_column];
            u8_array *polygon_cell     = &result->rows.data[i].data[polygon_column];
            u8_array *name_cell        = &result->rows.data[i].data[name_column];
            u8_array *party_id_cell    = &result->rows.data[i].data[party_id_column];
            u8_array *colour_cell      = &result->rows.data[i].data[colour_column];

            if (!polygon_cell->count)  continue;

            Polygon_array polygons = {.context = ctx};
            {
                u8 *end_data = NULL;
                parse_wkb_polygons(polygon_cell->data, &polygons, &end_data);

                assert(end_data == &polygon_cell->data[polygon_cell->count]);
            }

            int district_id = (int)get_u32_from_cell(district_id_cell);
            char_array name = get_char_array_from_cell(name_cell);

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

            bool dark = (tile.theme == DARK);
            dark     |= (tile.theme == HIGHLIGHT_DISTRICT && tile.district_id != district_id);

            if (dark)  colour = lerp_rgb(colour, (Vector3){0}, 0.5);

            for (s64 j = 0; j < polygons.count; j++)  draw_polygon(&polygons.data[j], colour, verts);
        }
    }

    // Draw the boundary lines. |Speed: Other than the coastline, boundaries are shared by two districts, and as a result we draw them twice.
    {
        char *query =
        " select id, st_asbinary(st_collectionextract(st_makevalid(                                         "
        "     st_clipbybox2d(                                                                               "
        "       st_simplify(geom, $1::float),                                                               "
        "       st_makeenvelope($2::float, $3::float, $4::float, $5::float, 3577)                           "
        "     )                                                                                             "
        "   ), 2)) as path                                                                                  "
        " from (                                                                                            "
        "     select id, st_collect(st_exteriorring(geom)) as geom                                          "
        "     from (                                                                                        "
        "         select id, (st_dump(bounds_clipped)).geom as geom                                         "
        "         from district                                                                             "
        "         where election_id = $6::int                                                               "
        "           and bounds_clipped && st_makeenvelope($2::float, $3::float, $4::float, $5::float, 3577) "
        "       ) t                                                                                         "
        "     group by id                                                                                   "
        "   ) t                                                                                             "
        ;

        PG_result *result = query_database(&db, query, &params, ctx);

        int id_column   = *Get(&result->columns, "id");    assert(id_column >= 0);
        int path_column = *Get(&result->columns, "path");  assert(path_column >= 0);

        Path_array paths = {.context = ctx};

        for (s64 row = 0; row < result->rows.count; row++) {
            u8_array *id_cell   = &result->rows.data[row].data[id_column];
            u8_array *path_cell = &result->rows.data[row].data[path_column];

            if (path_cell->count == 0)  continue;

            u8 *end_data = NULL;
            parse_wkb_paths(path_cell->data, &paths, &end_data);
            assert(end_data == path_cell->data + path_cell->count);

            Vector3 colour = {0.8, 0.8, 0.8};

            float line_width = 1.5*tile.upp;
            if (tile.theme == HIGHLIGHT_DISTRICT) {
                u32 id = get_u32_from_cell(id_cell);
                if (id == tile.district_id)  line_width = 4*tile.upp;
            }

            for (s64 i = 0; i < paths.count; i++) {
                draw_path(&paths.data[i], line_width, colour, verts);
            }

            paths.count = 0; // So we can reuse the array for the next loop.
        }
    }

    Response response = {200, .body = verts->data, .size = verts->count*sizeof(Vertex)};

    response.headers = (string_dict){.context = ctx};
    *Set(&response.headers, "content-type") = "application/octet-stream";

    close_database(&db);
    return response;
}

Response serve_districts(Request *request, Memory_context *context)
{
    Memory_context *ctx = context;

    PG_client db = {DATABASE_URL, .use_cache = true};

    string_array params = {.context = ctx};

    // Get the election ID from the request path and add it to the SQL query parameters.
    {
        char *election = request->path_params.data[0];
        assert(election);

        *Add(&params) = election;
    }

    char *query =
    " select jsonb_object_agg(                 "
    "     id, jsonb_build_object(              "
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
    "     select id,                           "
    "       name,                              "
    "       st_centroid(bounds_clipped) as centroid,   "
    "       box2d(bounds_clipped) as box               "
    "     from district                        "
    "     where election_id = $1::int          "
    "     order by st_area(bounds_clipped) desc        "
    "   ) t                                    "
    ;

    PG_result *result = query_database(&db, query, &params, ctx);

    assert(*Get(&result->columns, "json") == 0);
    assert(result->rows.count == 1);
    u8_array *json = &result->rows.data[0].data[0];

    Response response = {200, .body = json->data, .size = json->count};

    response.headers = (string_dict){.context = ctx};
    *Set(&response.headers, "content-type") = "application/json";

    return response;
}

Response serve_seats_won(Request *request, Memory_context *context)
// Serve a summary of the number of seats won by each party for a particular election, for drawing the "Seats won" chart.
{
    Memory_context *ctx = context;

    PG_client db = {DATABASE_URL, .use_cache = true};

    char *query =
    " select jsonb_agg(to_jsonb(t.*))::text as json                    "
    " from (                                                           "
    "     select p.short_code as \"shortCode\",                        "
    "       count(*)                                                   "
    "     from contest_vote v                                          "
    "     join candidate c                                             "
    "       on c.election_id = v.election_id and c.id = v.candidate_id "
    "     left join party p                                            "
    "       on p.election_id = v.election_id and p.id = c.party_id     "
    "     where v.count_type = '2CP' and v.elected                     "
    "       and v.election_id = $1::int                                "
    "     group by p.short_code                                        "
    "   ) t                                                            "
    ;

    string_array params = {.context = ctx};
    {
        char *election = request->path_params.data[0];  assert(election);
        *Add(&params) = election;
    }

    PG_result *result = query_database(&db, query, &params, ctx);

    assert(*Get(&result->columns, "json") == 0);
    assert(result->rows.count == 1); //|Bug: There may be 0 rows if the election ID in the request path does not exist. Currently in this case there's a segfault.

    u8_array *json = &result->rows.data[0].data[0];

    Response response = {200, .body = json->data, .size = json->count};

    response.headers = (string_dict){.context = ctx};
    *Set(&response.headers, "content-type") = "application/json";

    return response;
}

Response serve_contest_votes(Request *request, Memory_context *context)
{
    Memory_context *ctx = context;

    PG_client db = {DATABASE_URL, .use_cache = true};

    char *query =
    " select jsonb_object_agg(\"countType\", c)::text as json           "
    " from (                                                            "
    "     select \"countType\",                                         "
    "       jsonb_agg(to_jsonb(t.*) - 'countType') as c                 "
    "     from (                                                        "
    "         select c.first_name               as \"firstName\",       "
    "           c.last_name                     as \"lastName\",        "
    "           coalesce(p.name, 'Independent') as \"partyName\",       "
    "           coalesce(p.short_code, 'IND')   as \"partyCode\",       "
    "           coalesce('#'||lpad(to_hex(p.colour),6,'0'),             "
    "             '#555555')                    as \"colour\",          "
    "           v.total                         as \"numVotes\",        "
    "           (case when v.count_type = '2CP' then 'tcp'              "
    "             else 'fp' end)                as \"countType\",       "
    "           v.ballot_position               as \"ballotPosition\"   "
    "         from contest_vote v                                       "
    "           join candidate c on c.election_id = v.election_id       "
    "           and c.id = v.candidate_id                               "
    "           left join party p on p.election_id = c.election_id      "
    "           and p.id = c.party_id                                   "
    "         where v.election_id = $1::int                             "
    "           and v.district_id = $2::int                             "
    "       ) t                                                         "
    "     group by \"countType\"                                        "
    "   ) t                                                             "
    ;

    string_array params = {.context = ctx};
    {
        char *election = request->path_params.data[0];  assert(election);
        *Add(&params) = election;

        char *district = request->path_params.data[1];  assert(district);
        *Add(&params) = district;
    }

    PG_result *result = query_database(&db, query, &params, ctx);

    assert(*Get(&result->columns, "json") == 0);
    assert(result->rows.count == 1); //|Bug: There may be 0 rows if the election ID in the request path does not exist. Currently in this case there's a segfault.
    u8_array *json = &result->rows.data[0].data[0];

    Response response = {200, .body = json->data, .size = json->count};

    response.headers = (string_dict){.context = ctx};
    *Set(&response.headers, "content-type") = "application/json";

    return response;
}

int main(int argc, char **argv)
{
    u32  address = 0;       // 0.0.0.0  |Todo: Make this configurable with getaddrinfo().
    u16  port    = 6008;

    // If there is a command-line argument, take it as a port.
    if (argc > 1) {
        char *end = NULL;
        port = strtol(argv[1], &end, 10);
        assert(end > argv[1]);
        assert(0 <= port && port <= UINT16_MAX);
    }

    Memory_context *top_context = new_context(NULL);

    Server *server = create_server(address, port, top_context);

    add_route(server, GET, "/vertices/(.+)",                                &serve_vertices);
    add_route(server, GET, "/elections/(\\d+)/districts.json",              &serve_districts);
    add_route(server, GET, "/elections/(\\d+)/seats-won.json",              &serve_seats_won);
    add_route(server, GET, "/elections/(\\d+)/contests/(\\d+)/votes.json",  &serve_contest_votes);
    add_route(server, GET, "/.*",                                           &serve_files);

    start_server(server);


    free_context(top_context);
    return 0;
}
