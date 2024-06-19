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

typedef Dict(char *)  string_dict;

typedef struct Route    Route;
typedef struct Request  Request;
typedef struct Response Response;
typedef Response Request_handler(Request *, Memory_Context *);

enum Method {GET=1, POST};

struct Route {
    enum Method      method;
    char            *path; // Just a string because we expect to define these statically in code.
    Request_handler *handler;
};

struct Request {
    enum Method  method;
    char_array   path;
    string_dict *query;
    //string_dict *headers; |Todo
};

struct Response {
    int          status;
    string_dict *headers;
    u8          *body;
    s64          size; // The number of bytes in the body.
};

Response handle_request(Request *request, Memory_Context *context)
// Testing the Request_handler typedef.
{
    int status = 200;

    string_dict *headers = NewDict(headers, context);

    *Set(headers, "content-type") = "text/html";

    char_array *body = NewArray(body, context);

    if (request->query) {
        char *name = *Get(request->query, "name");

        if (name)  print_string(body, "Hello, %s.\n", name);
        else       print_string(body, "What?\n");
    } else {
        print_string(body, "Hello, person.\n");
    }

    return (Response){status, headers, (u8 *)body->data, body->count};
}

Response handle_400(Request *request, Memory_Context *context)
{
    char body[] = "Bad request. Bad you.";

    return (Response){
        .status  = 400,
        .headers = NULL,
        .body    = (u8 *)body,
        .size    = lengthof(body),
    };
}

Response handle_404(Request *request, Memory_Context *context)
{
    char body[] = "Can't find it.";

    return (Response){
        .status  = 404,
        .headers = NULL,
        .body    = (u8 *)body,
        .size    = lengthof(body),
    };
}

static bool is_alphanum(char c) //|Temporary: Move.
{
    if ('a' <= c && c <= 'z')  return true;
    if ('A' <= c && c <= 'Z')  return true;
    if ('0' <= c && c <= '9')  return true;

    return false;
}

// |Temporary move
bool string_contains_char(char const *string, s64 length, char c)
{
    for (s64 i = 0; i < length; i++) {
        if (string[i] == c)  return true;
    }
    return false;
}
#define Contains(STATIC_STRING, CHAR)  string_contains_char((STATIC_STRING), lengthof(STATIC_STRING), (CHAR))


static bool is_hex(char c)
// Check whether a character is an ASCII hexadecimal number.
{
    c |= 0x20; // OR-ing with 0x20 makes ASCII letters lowercase and doesn't affect ASCII numbers.

    return ('0' <= c && c <= '9') || ('a' <= c && c <= 'f');
}

static char hex_to_char(char c1, char c2)
// Assumes you've already validated the characters with is_hex().
{
    assert(is_hex(c1) && is_hex(c2));

    c1 |= 0x20; // OR-ing with 0x20 makes ASCII letters lowercase and doesn't affect ASCII numbers.
    c2 |= 0x20;

    u8 x1 = c1 <= '9' ? c1-'0' : c1-'a'+10;
    u8 x2 = c2 <= '9' ? c2-'0' : c2-'a'+10;

    return (char)((x1 << 4) | x2);
}

Request *parse_request(char *data, s64 size, Memory_Context *context)
{
    Memory_Context *ctx = context; // Just for shorthand.
    char *d = data;                // A pointer to advance as we parse.

  #define Fail(...)  (Log("Failed to parse a request. " __VA_ARGS__), NULL)

    Request *result = New(Request, ctx);

    if (size < 4)  return Fail("The request was too short.");

    if (starts_with(d, "GET")) {
        result->method = GET;
        d += 3;
    } else if (starts_with(d, "POST")) {
        result->method = POST;
        d += 4;
    } else {
        return Fail("Expected GET or POST. Got: '%c%c%c%c...'", d[0],d[1],d[2],d[3]);
    }

    if (*d != ' ') return Fail("Expected a space after the method. Got '%c'.", *d);
    d += 1;

    result->path = (char_array){.context = ctx};
    char_array *path = &result->path;

    bool there_is_a_query = false;

    char const ALLOWED[] = "-_./,"; // Other than alphanumeric characters, these are the only characters we don't treat specially in paths and query strings.

    // Parse the path.
    while (d-data < size && *d != ' ') {
        if (is_alphanum(*d) || Contains(ALLOWED, *d)) {
            *Add(path) = *d;
            d += 1;
        } else if (*d == '%') {
            // We are being cautious not to read past the end of the request data. (We always make
            // sure the request data has a terminating '\0', so this is probably overly cautious.)
            if (d-data+2 < size && is_hex(d[1]) && is_hex(d[2])) {
                *Add(path) = hex_to_char(d[1], d[2]);
                d += 3;
            } else {
                return Fail("Expected %% in path to be followed by two hexadecimal digits.");
            }
        } else if (*d == '?') {
            there_is_a_query = true;
            d += 1;
            break;
        }
        // We couldn't parse the next character.
        else  return Fail("Unexpected character in path: 0x%0x.", *d); //|Todo: Print the character itself if it's printable.
    }
    if (path->count == 0)  return Fail("Expected a path.");

    // Make sure the path ends with a trailing zero.
    *Add(path)   = '\0';
    path->count -= 1;

    if (there_is_a_query) {
        string_dict *query = NewDict(query, ctx);

        char key[256]  = {0};
        char val[256]  = {0};
        int  key_index = 0;
        int  val_index = 0;
        bool is_key    = true; // This will be false when we're parsing a value.
        bool success   = true;

        while (d-data < size) {
            if (*d == '&' || *d == ' ') {
                // Add the previous key/val we were building.
                if (key_index) {
                    if (val_index)  *Set(query, key) = copy_string(val, ctx);
                    else            *Set(query, key) = "";
                }
                if (*d == ' ')  break;
                memset(key, 0, key_index);
                memset(val, 0, val_index);
                key_index = val_index = 0;
                is_key = true;
                d += 1;
            } else if (*d == '=') {
                // If we fail to parse the query string, we'll just break out---then handle the request as though there was no query string.
                // |Todo: We should probably pass the query string to the application as a raw string in this case.
                if (!key_index) {
                    Fail("Expected a query-string key. Got '='.");
                    success = false;
                    break;
                } else {
                    is_key = false;
                    d += 1;
                }
            } else if (is_alphanum(*d) || Contains(ALLOWED, *d)) {
                if (is_key) {
                    key[key_index] = *d;
                    key_index += 1;
                    if (key_index > lengthof(key)) {
                        Fail("A query-string key is too long.");
                        success = false;
                        break;
                    }
                } else {
                    val[val_index] = *d;
                    val_index += 1;
                    if (val_index > lengthof(val)) {
                        Fail("A query-string value is too long.");
                        success = false;
                        break;
                    }
                }
                d += 1;
            } else if (*d == '%' && (d-data+2 < size) && is_hex(d[1]) && is_hex(d[2])) {
                if (is_key) {
                    key[key_index] = hex_to_char(d[1], d[2]);
                    key_index += 1;
                    if (key_index > lengthof(key)) {
                        Fail("A query-string key is too long.");
                        success = false;
                        break;
                    }
                } else {
                    val[val_index] = hex_to_char(d[1], d[2]);
                    val_index += 1;
                    if (val_index > lengthof(val)) {
                        Fail("A query-string value is too long."); // |Cleanup
                        success = false;
                        break;
                    }
                }
                d += 3;
            }
            else {
                Fail("Unexpected character in query string: '%c'.", *d);
                success = false;
                break;
            }
        }

        // If we broke out of the above loop because of a failure to parse the query string, we
        // still want to return what we have.Advance the data pointer to the next 0x20 space
        // character after the URI so that we can keep parsing.
        if (!success) {
            while (*d != ' ' && d-data < size)  d += 1;
        } else {
            // Also let request->query remain NULL unless we set some key/value pairs.
            if (query->count)  result->query = query;
        }
    }

    if (*d != ' ')  return Fail("Expected a space after the URI. Got 0x%x.", *d);
    d += 1;

    if (!(d-data+5 < size && starts_with(d, "HTTP/")))  return Fail("Expected 'HTTP/' after the path.");

  #undef Fail
    return result;
}

int main()
{
    u32 const ADDR = 0xac1180e0; // 172.17.128.224
    u16 const PORT = 6008;

    Route routes[] = {
        {GET, "/",  &handle_request},
    };

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

        Request_handler *handler = NULL;

        if (!request) {
            handler = &handle_400; // Bad request.
        } else {
            Log("Parsed a request!");
            Log(" Method: %s", request->method == GET ? "GET" : request->method == POST ? "POST" : "UNKNOWN!!");
            Log(" Path:   %s", request->path.data);
            if (request->query) {
                string_dict *q = request->query;
                Log(" Query parameters:");
                for (s64 i = 0; i < q->count; i++)  Log("   %8s:%8s", q->keys[i], q->vals[i]);
            }

            for (s64 i = 0; i < countof(routes); i++) {
                Route *r = &routes[i];

                if (r->method != request->method)                             continue;
                if (strlen(r->path) != request->path.count)                   continue;
                if (memcmp(r->path, request->path.data, request->path.count)) continue;

                handler = r->handler;
                break;
            }
        }

        if (!handler)  handler = &handle_404;

        {
            Response response = (*handler)(request, ctx);

            char_array *buffer = NewArray(buffer, ctx);

            print_string(buffer, "HTTP/1.1 %d\n", response.status);

            if (response.headers) {
                string_dict *h = response.headers;
                for (s64 i = 0; i < h->count; i++)  print_string(buffer, "%s: %s\n", h->keys[i], h->vals[i]);
            }

            print_string(buffer, "\n");

            // |Speed: We copy the response body to the buffer.
            if (buffer->limit < buffer->count + response.size)  array_reserve(buffer, buffer->count + response.size);
            memcpy(&buffer->data[buffer->count], response.body, response.size);
            buffer->count += response.size;

            int flags = 0;
            s64 num_bytes_sent = send(peer_socket_num, buffer->data, buffer->count, flags);
            assert(num_bytes_sent == buffer->count);
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
