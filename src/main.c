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

Response serve_file(Request *request, Memory_Context *context)
{
    char *path = request->path.data;
    s64 path_size = request->path.count;

    // If the path starts with a '/', "remove" it by advancing the pointer by 1 byte.
    if (*path == '/') {
        path      += 1;
        path_size -= 1;
    }

    u8_array *file = load_binary_file(path, context);

    if (!file)  return (Response){404};

    char *file_extension = NULL;

    for (s64 i = path_size-1; i >= 0; i--) {
        if (path[i] == '/')  break;
        if (path[i] == '.') {
            if (i < path_size-1)  file_extension = &path[i+1];
            break;
        }
    }

    char *content_type = NULL;

    if (file_extension) {
        if (!strcmp(file_extension, "html"))       content_type = "text/html";
        else if (!strcmp(file_extension, "js"))    content_type = "text/javascript";
        else if (!strcmp(file_extension, "json"))  content_type = "application/json";
        else if (!strcmp(file_extension, "ttf"))   content_type = "font/ttf";
    }

    string_dict *headers = NewDict(headers, context);

    if (content_type)  *Set(headers, "content-type") = content_type;

    return (Response){200, headers, file->data, file->count};
}

static bool is_alphanum(char c)
{
    if ('a' <= c && c <= 'z')  return true;
    if ('A' <= c && c <= 'Z')  return true;
    if ('0' <= c && c <= '9')  return true;

    return false;
}

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
// On failure, return NULL only if the data doesn't look like a request at all.
// As long as we can parse a method and a path, return what we've got.
{
    Memory_Context *ctx = context; // Just for shorthand.
    char *d = data;                // A pointer to advance as we parse.

    Request *result = New(Request, ctx);

    if (size < 5) {
        Log("The request was too short.");
        Breakpoint();
        return NULL;
    }

    if (starts_with(d, "GET ")) {
        result->method = GET;
        d += 4;
    } else if (starts_with(d, "POST ")) {
        result->method = POST;
        d += 5;
    } else {
        Log("Expected 'GET ' or 'POST ' (note the space). Got: '%c%c%c%c...'", d[0],d[1],d[2],d[3]);
        return NULL;
    }

    // Other than alphanumeric characters, these are the only characters we don't treat specially
    // in paths and query strings. They're RFC 3986's unreserved characters plus a few.
    char const ALLOWED[] = "-._~/,+"; //|Cleanup: Defined twice.

    char_array  *path  = NewArray(path, ctx);
    string_dict *query = NewDict(query, ctx);

    // Parse the path and query string.
    {
        char key_buffer[256] = {0};
        char val_buffer[256] = {0};
        int  key_index = 0;
        int  val_index = 0;

        bool is_query = false; // Will be true when we're parsing the query string.
        bool is_value = false; // Only meaningful if is_query is true. If so, false means we're parsing a key and true means we're parsing a value.
        bool success  = false;

        while (d-data < size) {
            // Each pass, we read a character from the request. If it's alphanumeric, in the ALLOWED array,
            // or a hex-encoded byte, we add it to our present target. At first our target is `path` because
            // we're just reading the path. But if we come to a query string, our target alternates between
            // two static buffers: we copy the pending key into one and the pending value into the other.
            if (is_alphanum(*d) || Contains(ALLOWED, *d) || *d == '%') {
                // Set c to *d, or, if d points to a %-encoded byte, the value of that byte.
                char c = *d;
                if (c == '%') {
                    // We are being cautious not to read past the end of the request data. (We always make
                    // sure the request data has a terminating '\0', so this is probably overly cautious.)
                    if (d-data+2 < size && is_hex(d[1]) && is_hex(d[2])) {
                        // It's a hex-encoded byte.
                        c = hex_to_char(d[1], d[2]);
                        d += 2;
                    } else {
                        // The path or query string has a '%' that's not followed by two hexadecimal digits.
                        break;
                    }
                }
                if (!is_query) {
                    *Add(path) = c;
                } else if (!is_value) {
                    key_buffer[key_index] = c;
                    key_index += 1;
                    if (key_index > lengthof(key_buffer))  break;
                } else {
                    val_buffer[val_index] = c;
                    val_index += 1;
                    if (key_index > lengthof(val_buffer))  break;
                }
            } else if (*d == '?') {
                if (is_query)  break;
                is_query = true;
            } else if (*d == '=') {
                if (!is_query)   break;
                if (is_value)    break;
                if (!key_index)  break;
                is_value = true;
            } else if (*d == '&' || *d == ' ') {
                // Add the previous key/value we were building.
                if (is_query && key_index) {
                    if (val_index)  *Set(query, key_buffer) = copy_string(val_buffer, ctx);
                    else            *Set(query, key_buffer) = ""; //  ?x&y&z  ->  x, y and z get empty strings for values.
                }
                if (*d == ' ') {
                    success = true;
                    break;
                }
                memset(key_buffer, 0, key_index);
                memset(val_buffer, 0, val_index);
                key_index = val_index = 0;
                is_value = false;
            } else {
                // There was an unexpected character.
                break;
            }
            d += 1;
        }

        // Return NULL if we didn't even parse a path.
        if (!success && !is_query)  return NULL;

        *Add(path) = '\0';
        path->count -= 1;

        result->path = *path;

        // Only add the query string if we fully parsed one.
        if (success && query->count)   result->query = query;

        // If we broke out of the above loop because of a failure to parse the query string, we
        // still want to return what we have. Advance the data pointer to the next 0x20 space
        // character after the URI so that we can keep parsing the request after that.
        if (!success) {
            while (*d != ' ' && d-data < size)  d += 1;
        }
    }

    if (!(d-data+5 < size && starts_with(d, " HTTP/"))) {
        Log("Expected ' HTTP/' (note the space) after the path.");
        return NULL;
    }

    return result;
}

char_array *encode_query_string(string_dict *query, Memory_Context *context)
{
    Memory_Context *ctx = context;

    char_array *result = NewArray(result, ctx);

    *Add(result) = '?';

    // The only characters we won't encode (other than alphanumeric) are RFC 3986 unreserved characters.
    char const ALLOWED[] = "-._~"; //|Cleanup: Why is this different from the other ALLOWED?

    for (s64 i = 0; i < query->count; i++) {
        if (i)  *Add(result) = '&';

        char *key = query->keys[i];
        s64 key_len = strlen(key);

        for (s64 j = 0; j < key_len; j++) {
            char c = key[j];
            if (is_alphanum(c) || Contains(ALLOWED, c))  *Add(result) = c;
            else  print_string(result, "%%%02x", c);
        }

        char *val = query->vals[i];
        s64 val_len = strlen(val);

        if (!val_len)  continue;

        *Add(result) = '=';

        for (s64 j = 0; j < val_len; j++) {
            char c = val[j];
            if (is_alphanum(c) || Contains(ALLOWED, c))  *Add(result) = c;
            else  print_string(result, "%%%02x", c);
        }
    }

    *Add(result) = '\0';
    result->count -= 1;

    return result;
}

int main()
{
    // Call the old main function so we can serve the files it creates. |Temporary.
    int once_and_future_main();
    once_and_future_main();

    u32 const ADDR = 0xac1180e0; // 172.17.128.224
    u16 const PORT = 6008;

    Route routes[] = {
        {GET, "/",          &handle_request},
        {GET, "/web/.*",    &serve_file},
        {GET, "/bin/.*",    &serve_file},
        {GET, "/fonts/.*",  &serve_file},
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

    int queue_length = 100;
    if (listen(socket_num, queue_length) < 0)  Error("Couldn't listen on socket (%s).", strerror(errno));

    Log("Listening on http://%d.%d.%d.%d:%d...", ADDR>>24, ADDR>>16&0xff, ADDR>>8&0xff, ADDR&0xff, PORT);

    Memory_Context *ctx = new_context(top_context);
    while (true) {
        reset_context(ctx);

        struct sockaddr_in peer_socket_addr; // Will be initialised by accept().
        socklen_t peer_socket_addr_size = sizeof(peer_socket_addr);

        int peer_socket_num = accept(socket_num, (struct sockaddr *)&peer_socket_addr, &peer_socket_addr_size);

        Request *request = NULL;
        char  *error_msg = NULL;
        {
            u64 BUFFER_SIZE = 1<<12;

            char_array *buffer = NewArray(buffer, ctx);
            array_reserve(buffer, BUFFER_SIZE);

            int flags = 0;
            buffer->count = recv(peer_socket_num, buffer->data, buffer->limit, flags);
            if (buffer->count == 0) {
                error_msg = "The request was empty.";
            } else if (buffer->count >= buffer->limit) {
                error_msg = "The request was larger than our buffer size.";
            } else {
                buffer->data[buffer->count] = '\0';

                request = parse_request(buffer->data, buffer->count, ctx);

                if (!request)  error_msg = "We couldn't parse the request.";
            }
        }
        assert(!!request ^ !!error_msg); // One or the other; not both.

        Request_handler *handler = NULL;
        if (request) {
            // We parsed a request. Look in the routes for an appropriate handler.
            for (s64 i = 0; i < countof(routes); i++) {
                if (routes[i].method == request->method) {
                    if (is_match(request->path.data, routes[i].path)) {
                        handler = routes[i].handler;
                        break;
                    }
                }
            }
        } else {
            // We failed to parse a request, so it was probably a bad request. Return a 400.
            handler = &handle_400;
        }
        if (!handler)  handler = &handle_404;

        {
            Response response = (*handler)(request, ctx);

            if (request) {
                char *method = request->method == GET ? "GET" : request->method == POST ? "POST" : "UNKNOWN!";

                char *query = request->query ? encode_query_string(request->query, ctx)->data : "";

                Log("[%d] %s %s%s", response.status, method, request->path.data, query);
            } else {
                Log("[%d] %s", response.status, error_msg);
            }

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
            assert(num_bytes_sent == buffer->count); // |Fixme: You can trigger this assert by spamming F5 in the browser---sometimes we send less than the full buffer.
        }

        if (close(peer_socket_num) < 0)  Error("Couldn't close the peer socket (%s).", strerror(errno));
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

    float metres_per_pixel = 4000.0;

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
