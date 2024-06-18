#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

// We include <errno.h> and <string.h> so that we can do `strerror(errno)`. This is |Threadunsafe
// but it is the only pure-C99 option for getting errno as a string. Of course, to use threads,
// you have to venture away from C99. So we looked at `strerror_r`, but it was a headache. The
// suffixed variant has multiple signatures and you get the one you don't want unless you have
// certain macros defined during compilation. Yuck! We don't want to deal with that now.
#include <errno.h>
#include <string.h>

#include "array.h"
#include "map.h"
#include "strings.h"

//typedef Dict(char *)  string_dict;

//typedef struct Route    Route;
typedef struct Request  Request;
//typedef struct Response Response;
//typedef Response *Request_handler(Request);

enum Method {GET=1, POST};

//struct Route {
//    enum Method      method;
//    char            *path; // Just a string because we expect to define these statically in code.
//    Request_handler *handler;
//};

struct Request {
    enum Method  method;
    char_array   path;
    //string_dict *query;
};

//struct Response {
//    int        status;
//    char_array body;
//};

//| Temporary: Move this to basic.h next to trim_left().
bool starts_with_(char *string, char *match, s64 match_length)
{
    for (s64 i = 0; i < match_length; i++) {
        if (string[i] != match[i])  return false;
    }
    return true;
}
#define starts_with(DATA, STATIC_STRING) \
    starts_with_((DATA), (STATIC_STRING), lengthof(STATIC_STRING))

#define ParseError(...)  (Error(__VA_ARGS__), NULL)
Request *parse_request(char *text, s64 length, Memory_Context *context)
{
    Request *request = New(Request, context);

    char *d = text;

    // 10 is an arbitrary number so we don't need to check for null bytes in first part of the text.
    if (length < 10)  return ParseError("This request was too short to parse:\n%s", text);

    if (starts_with(d, "GET")) {
        request->method = GET;
        d += 3;
    } else if (starts_with(d, "POST")) {
        request->method = POST;
        d += 4;
    }

    if (*d != ' ' || !request->method)  return ParseError("Could not parse the method from this request:\n%s", text);
    d += 1;

    request->path = (char_array){.context = context};
    do {
        if (*d == ' ')  break;
        *Add(&request->path) = *d;
        d += 1;
    } while (d - text < length);
    *Add(&request->path) = '\0';
    request->path.count -= 1;

    if (*d != ' ' || !request->path.count)  return ParseError("Could not parse the path from this request:\n%s", text);
    d += 1;

    if (!starts_with(d, "HTTP/"))  return ParseError("Expected 'HTTP/' after the path in this request:\n%s", text);

    return request;
}
#undef ParseError

int main()
{
    u32 const ADDR = 0xac1180e0; // 172.17.128.224
    u16 const PORT = 6008;

    Memory_Context *top_context = new_context(NULL);

    int socket_num = socket(AF_INET, SOCK_STREAM, 0);

    // Set SO_REUSEADDR because we want to run this program frequently during development. Otherwise
    // the Linux kernel holds onto our address/port combo for a while after our program finishes.
    if (setsockopt(socket_num, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int)) < 0) {
        Error("Couldn't set socket options (%s).", strerror(errno));
    }

    struct sockaddr_in socket_addr = {
        .sin_family   = AF_INET,
        .sin_port     = htons(PORT),
        .sin_addr     = htonl(ADDR),
    };

    if (bind(socket_num, (struct sockaddr const *)&socket_addr, sizeof(socket_addr)) < 0) {
        Error("Couldn't bind socket (%s).", strerror(errno));
    }

    if (listen(socket_num, 100) < 0)  Error("Couldn't listen on socket (%s).", strerror(errno));

    Log("Listening on http://%d.%d.%d.%d:%d...", ADDR>>24, ADDR>>16&0xff, ADDR>>8&0xff, ADDR&0xff, PORT);

    while (true) {
        Memory_Context *ctx = new_context(top_context);

        struct sockaddr_in peer_socket_addr; // Will be initialised by accept().
        socklen_t peer_socket_addr_size = sizeof(peer_socket_addr);

        int peer_socket_num = accept(socket_num, (struct sockaddr *)&peer_socket_addr, &peer_socket_addr_size);

        Request *request; {
            u64 BUFFER_SIZE = 1<<13;

            char_array *buffer = NewArray(buffer, ctx);
            array_reserve(buffer, BUFFER_SIZE);

            int flags = 0;
            buffer->count = recv(peer_socket_num, buffer->data, buffer->limit, flags);
            if (buffer->count < buffer->limit) {
                buffer->data[buffer->count] = '\0';
            } else {
                Error("The request is larger than our buffer size!");
                break; //|Temporary
            }

            request = parse_request(buffer->data, buffer->count, ctx);
        }

        if (!request)  break;

        Log("Parsed a request!");
        Log(" Method: %s", request->method == GET ? "GET" : request->method == POST ? "POST" : "UNKNOWN!!");
        Log(" Path:   %s", request->path.data);

        {
            char_array *response = NewArray(response, ctx);

            print_string(response, "HTTP/1.1 200 OK\n");
            print_string(response, "content-type: text/html\n");
            print_string(response, "\n");
            print_string(response, "[Tim Rogers voice] ...HELLO.\n");

            int flags = 0;
            s64 num_bytes_sent = send(peer_socket_num, response->data, response->count, flags);
            assert(num_bytes_sent == response->count);
        }

        if (close(peer_socket_num) < 0)  Error("Couldn't close the peer socket (%s).", strerror(errno));

        free_context(ctx);
    }

    if (close(socket_num) < 0)  Error("Couldn't close our own socket (%s).", strerror(errno));

    free_context(top_context);
    return 0;
}


#include <string.h>

#include "draw.h"
#include "json.h"
#include "map.h"
#include "pg.h"
#include "strings.h"

#include "grab.h" // |Leak

int once_and_future_main()
{
    Memory_Context *ctx = new_context(NULL);

    PGconn *db = PQconnectdb("postgres://postgres:postgisclarity@osm.tal/gis");
    if (PQstatus(db) != CONNECTION_OK)  Error("Database connection failed: %s", PQerrorMessage(db));

    Vertex_array *verts = NewArray(verts, ctx);

    float metres_per_pixel = 2000.0;

    bool show_voronoi = true;

    if (show_voronoi) {
        // Draw the Voronoi map polygons.
        char *query = Grab(/*
            select st_asbinary(st_buildarea(topo)) as polygon
            from (
                select st_simplify(topo, $1::float) as topo
                from booths_22
                where topo is not null
              ) t
        */);

        string_array *params = NewArray(params, ctx);
        *Add(params) = get_string(ctx, "%f", metres_per_pixel)->data;

        Polygon_array *polygons = query_polygons(db, query, params, ctx);

        for (s64 i = 0; i < polygons->count; i++) {
            Vector4 colour = {0.3*frand(), 0.8*frand(), 0.4*frand(), 1.0};

            draw_polygon(&polygons->data[i], colour, verts);
        }
    } else {
        // Draw electorate boundaries as polygons.
        char *query = Grab(/*
            select st_asbinary(st_buildarea(topo)) as polygon
            from (
                select st_simplify(topo, $1::float) as topo
                from electorates_22
              ) t
        */);

        string_array *params = NewArray(params, ctx);

        *Add(params) = get_string(ctx, "%f", metres_per_pixel)->data;

        Polygon_array *polygons = query_polygons(db, query, params, ctx);

        for (s64 i = 0; i < polygons->count; i++) {
            float shade = frand();
            Vector4 colour = {0.9*shade, 0.4*shade, 0.8*shade, 1.0};

            draw_polygon(&polygons->data[i], colour, verts);
        }
    }

    // Draw electorate boundaries as lines.
    {
        char *query = Grab(/*
            select st_asbinary(t.geom) as path
            from (
                select st_simplify(geom, $1::float) as geom
                from electorates_22_topo.edge_data
              ) t
        */);

        string_array *params = NewArray(params, ctx);

        *Add(params) = get_string(ctx, "%f", metres_per_pixel)->data;

        Path_array *paths = query_paths(db, query, params, ctx);

        for (s64 i = 0; i < paths->count; i++) {
            Vector4 colour = {0, 0, 0, 1.0};

            float line_width_px = 5;
            float line_width    = line_width_px*metres_per_pixel;

            draw_path(&paths->data[i], line_width, colour, verts);
        }
    }

    write_array_to_file(verts, "/home/jpj/src/webgl/bin/vertices");

    // Output map labels as JSON.
    {
        char *query = Grab(/*
            select jsonb_build_object(
                'labels', jsonb_agg(
                    jsonb_build_object(
                        'text', upper(name),
                        'pos', jsonb_build_array(round(st_x(centroid)), round(-st_y(centroid)))
                      )
                  )
              )::text as json
            from (
                select name,
                  st_centroid(geom) as centroid
                from electorates_22
                order by st_area(geom) desc
              ) t;
        */);

        Postgres_result *result = query_database(db, query, NULL, ctx);

        u8_array *json = *Get(result->data[0], "json");

        write_array_to_file(json, "/home/jpj/src/webgl/bin/labels.json"); // |Temporary: Change directory structure.
    }

    PQfinish(db);
    free_context(ctx);

    return 0;
}
