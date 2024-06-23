#define _POSIX_C_SOURCE 199309L // For clock_gettime().

#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>

// We include <errno.h> and <string.h> so that we can do `strerror(errno)`. This is |Threadunsafe
// but it is the only pure-C99 option for getting errno as a string. Of course, to use threads,
// you have to venture away from C99. So we looked at `strerror_r`, but it was a headache. The
// suffixed variant has multiple signatures and you get the one you don't want unless you have
// certain macros defined during compilation. Yuck! We don't want to deal with that now.
#include <errno.h> //|Cleanup: Sort this out.
#include <string.h>

#include "array.h"
#include "map.h"
#include "strings.h"

typedef Dict(char *)  string_dict;

typedef struct Route    Route;
typedef struct Request  Request;
typedef struct Response Response;
typedef Response Request_handler(Request *, Memory_Context *);
typedef struct Pending_request Pending_request;

enum HTTP_method {
    INVALID_HTTP_METHOD,
    GET,
    POST,
};

struct Route {
    enum HTTP_method     method;
    char                *path; // Just a string because we expect to define these statically in code.
    Request_handler     *handler;
};

struct Request {
    enum HTTP_method     method;
    char_array           path;
    string_dict         *query;
    //string_dict       *headers; |Todo
};

struct Response {
    int               status;
    string_dict      *headers;

    u8               *body;
    s64               size; // The number of bytes in the body.
};

struct Pending_request {
    Memory_Context       *context;

    u8_array              message;         // A buffer for storing the all bytes we've received.
    s64                   start_time;      // When we accepted the connection.

    s32                   socket_no;       // The socket file descriptor.

    enum Request_phase {
        PARSING=1,
        READY_TO_CLOSE, // The request has completed (not necessarily successfully). The socket needs to be closed and memory freed.
    }                     phase;

    //Array(int) *offsets; |Todo: When we can't parse the request straight away (maybe because it hasn't all come in yet), we should keep an array of the offsets of the parts we did understand on the first attempt, e.g. the URI and each header
    //Request    *result;
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
    char const static body[] = "Bad request. Bad you.\n";

    return (Response){
        .status  = 400,
        .headers = NULL,
        .body    = (u8 *)body,
        .size    = lengthof(body),
    };
}

Response handle_404(Request *request, Memory_Context *context)
{
    char const static body[] = "Can't find it.\n";

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

Request *parse_request(u8 *data, s64 size, Memory_Context *context)
// On failure, return NULL only if the data doesn't look like a request at all.
// As long as we can parse a method and a path, return what we've got.
{
    Memory_Context *ctx = context; // Just shorthand.
    u8 *d = data;                  // A pointer to advance as we parse.

    Request *result = New(Request, ctx);

    if (size < 5) {
        Log("The request was too short.");
        Breakpoint();
        return NULL;
    }

    if (starts_with((char *)d, "GET ")) {
        result->method = GET;
        d += 4;
    } else if (starts_with((char *)d, "POST ")) {
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
        char_array key   = {.context = ctx};
        char_array value = {.context = ctx};

        bool is_query = false; // Will be true when we're parsing the query string.
        bool is_value = false; // Only meaningful if is_query is true. If so, false means we're parsing a key and true means we're parsing a value.
        bool success  = false;

        while (d-data < size) {
            // Each pass, we read a character from the request. If it's alphanumeric, in the ALLOWED array,
            // or a hex-encoded byte, we add it to our present target. At first our target is `path` because
            // we're just reading the path. But if we come to a query string, our target alternates between
            // two arrays: we copy the pending key into one and the pending value into the other.
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
                if (!is_query)       *Add(path)   = c;
                else if (!is_value)  *Add(&key)   = c;
                else                 *Add(&value) = c;
            } else if (*d == '?') {
                if (is_query)  break;
                is_query = true;
            } else if (*d == '=') {
                if (!is_query)   break;
                if (is_value)    break;
                if (!key.count)  break;
                is_value = true;
            } else if (*d == '&' || *d == ' ') {
                // Add the previous key/value we were building.
                if (is_query && key.count) {
                    *Add(&key) = '\0';
                    if (value.count) {
                        *Add(&value) = '\0';
                        *Set(query, key.data) = value.data;
                    } else {
                        *Set(query, key.data) = ""; //  ?x&y&z  ->  x, y and z get empty strings for values.
                    }
                }
                if (*d == ' ') {
                    success = true;
                    break;
                }
                key   = (char_array){.context = ctx};
                value = (char_array){.context = ctx};
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
        while (*d != ' ' && d-data < size)  d += 1;
    }

    if (!(d-data+5 < size && starts_with((char *)d, " HTTP/"))) {
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

void set_blocking(int file_no, bool blocking)
// file_no is an open file descriptor.
{
#if OS != LINUX
    #error "set_blocking() is only implemented for Linux at the moment."
#endif
    int flags = fcntl(file_no, F_GETFL, 0);

    if (flags == -1)  Fatal("fcntl failed (%s).", strerror(errno));

    if (blocking)  flags &= ~O_NONBLOCK;
    else           flags |= O_NONBLOCK;

    bool success = !fcntl(file_no, F_SETFL, flags);

    if (!success)  Fatal("fcntl failed (%s).", strerror(errno));
}

#include <time.h>
s64 get_monotonic_time()
// In milliseconds.
{
#if OS != LINUX
    #error "get_monotonic_time() is only implemented for Linux at the moment."
#endif
    struct timespec time;
    bool success = !clock_gettime(CLOCK_MONOTONIC, &time);

    if (!success)  Fatal("clock_gettime failed (%s).", strerror(errno));

    s64 milliseconds = 1000*time.tv_sec + time.tv_nsec/1000;

    return milliseconds;
}

//|Temporary: Move to array.h.
void array_unordered_remove_by_index_(void *data, s64 *count, u64 unit_size, s64 index_to_remove)
// Decrements *count.
{
    assert(0 <= index_to_remove && index_to_remove < *count);

    u8 *item_to_remove = (u8 *)data + index_to_remove*unit_size;
    u8 *last_item      = (u8 *)data + (*count-1)*unit_size;

    memcpy(item_to_remove, last_item, unit_size);

    memset(last_item, 0, unit_size);

    *count -= 1;
}
#define array_unordered_remove_by_index(ARRAY, INDEX) \
    array_unordered_remove_by_index_((ARRAY)->data, &(ARRAY)->count, sizeof((ARRAY)->data), INDEX)

int main()
{
    // Call the old main function so we can serve the files it creates. |Temporary.
    int once_and_future_main();
    once_and_future_main();

    u32  const ADDR    = 0xac1180e0; // 172.17.128.224 |Todo: Use getaddrinfo().
    u16  const PORT    = 6008;
    bool const VERBOSE = true;
    s64  const REQUEST_TIMEOUT = 1000*5; // How many milliseconds we allow for each request to come through.

    Route routes[] = {
        {GET, "/",          &handle_request},
        {GET, "/web/.*",    &serve_file},
        {GET, "/bin/.*",    &serve_file},
        {GET, "/fonts/.*",  &serve_file},
    };

    Memory_Context *top_context = new_context(NULL);

    // The first two members of the pollfds array are special because they never get removed: pollfds.data[0] will be
    // standard input and pollfds.data[1] will be the main socket listening for connections.
    // Additional members of the array will be open client connections.
    Array(struct pollfd) pollfds = {.context = top_context};

    *Add(&pollfds) = (struct pollfd){.fd = STDIN_FILENO, .events = POLLIN};

    int my_socket_no = socket(AF_INET, SOCK_STREAM, 0);
    {
        // Set SO_REUSEADDR because we want to run this program frequently during development. Otherwise
        // the Linux kernel holds onto our address/port combo for a while after our program finishes.
        if (setsockopt(my_socket_no, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int)) < 0) {
            Error("Couldn't set socket options (%s).", strerror(errno));
        }

        set_blocking(my_socket_no, false);

        struct sockaddr_in socket_addr = {
            .sin_family   = AF_INET,
            .sin_port     = htons(PORT),
            .sin_addr     = htonl(ADDR),
        };

        if (bind(my_socket_no, (struct sockaddr const *)&socket_addr, sizeof(socket_addr)) < 0) {
            Error("Couldn't bind socket (%s).", strerror(errno));
        }

        int queue_length = 32;
        if (listen(my_socket_no, queue_length) < 0)  Error("Couldn't listen on socket (%s).", strerror(errno));

        Log("Listening on http://%d.%d.%d.%d:%d...", ADDR>>24, ADDR>>16&0xff, ADDR>>8&0xff, ADDR&0xff, PORT);

        *Add(&pollfds) = (struct pollfd){.fd = my_socket_no, .events = POLLIN};
    }

    Map(s32, Pending_request) *pending_requests = NewMap(pending_requests, top_context);

    bool server_should_stop = false;

    while (!server_should_stop)
    {
        s64 current_time = get_monotonic_time();

        if (VERBOSE)  Log("Polling %ld open file descriptors.", pollfds.count);

        // If there aren't any open connections, wait indefinitely. If there are connections, poll
        // once per second so we can check if any connection has expired.
        int timeout_ms = (pollfds.count-2 > 0) ? 1000 : -1;

        int num_events = poll(pollfds.data, pollfds.count, timeout_ms);

        if (num_events < 0)  Fatal("poll failed (%s).", strerror(errno));

        // If poll() timed out without any events occurring, skip trying to process requests.
        if (!num_events)  goto cleanup;
        // Otherwise, num_events is a positive number.

        for (s64 pollfd_index = 0; pollfd_index < pollfds.count; pollfd_index += 1) {
            struct pollfd *pollfd = &pollfds.data[pollfd_index];

            if (!(pollfd->revents & POLLIN))  continue; // We're only interested in knowing when we can read data.

            if (pollfd->fd == STDIN_FILENO) {
                if (VERBOSE)  Log("There's something to read on standard input.");
                // We should exit gracefully if it says "q" or something. |Temporary: Exit on any input from stdin.
                server_should_stop = true;
                goto cleanup;
            } else if (pollfd->fd == my_socket_no) {
                // A new connection has occurred.
                struct sockaddr_in client_socket_addr; // This will be initialised by accept().
                socklen_t client_socket_addr_size = sizeof(client_socket_addr);

                int client_socket_no = accept(my_socket_no, (struct sockaddr *)&client_socket_addr, &client_socket_addr_size);
                if (client_socket_no < 0)  Fatal("poll() said we could read from our main socket, but we couldn't get a new connection (%s).", strerror(errno));

                set_blocking(client_socket_no, false);

                if (VERBOSE)  Log("Adding a new client (socket %d).", client_socket_no);

                struct pollfd pollfd = {.fd = client_socket_no, .events = POLLIN};
                // Also set .revents so that we'll try receiving from this socket (after the others) instead of waiting to poll them all again. |Hack
                pollfd.revents = POLLIN;

                *Add(&pollfds) = pollfd;

                Pending_request pending_request = {0};

                pending_request.context    = new_context(top_context);
                pending_request.message    = (u8_array){.context = pending_request.context};
                pending_request.start_time = current_time;
                pending_request.socket_no  = client_socket_no;
                pending_request.phase      = PARSING;

                *Set(pending_requests, client_socket_no) = pending_request;
            } else {
                // There's something to read on a client socket.
                int client_socket_no = pollfd->fd;

                if (VERBOSE)  Log("Socket %d has something to say!!", client_socket_no);

                Pending_request *pending_request = Get(pending_requests, client_socket_no);

                u8_array *message = &pending_request->message; // We assume it was been initialised when the request was accepted.

                while (true) {
                    u8 *free_data      = message->data + message->count;
                    s64 num_free_bytes = message->limit - message->count - 1; // Reserve space for a null byte.
                    if (num_free_bytes <= 0) {
                        array_reserve(message, round_up_pow2(message->limit+1));
                        continue;
                    }

                    if (VERBOSE)  Log("We're about to read socket %d.", client_socket_no);

                    int flags = 0;
                    s64 recv_count = recv(client_socket_no, free_data, num_free_bytes, flags);

                    if (recv_count < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            // There's nothing more to read now.
                            if (VERBOSE)  Log("There was nothing to read.");
                            break;
                        } else {
                            // There was an actual error.
                            Fatal("We failed to read from socket %d (%s).", client_socket_no, strerror(errno));
                        }
                    } else if (recv_count == 0) {
                        // The client has disconnected.
                        if (VERBOSE)  Log("Socket %d has disconnected.", client_socket_no);
                        pending_request->phase = READY_TO_CLOSE;
                        break; // |Fixme: Now we know the client has disconnected, we should free up resources for the socket and start looking at the next request ASAP.
                    } else {
                        // We have successfully received some bytes.
                        if (VERBOSE)  Log("Read %ld bytes from socket %d.", recv_count, client_socket_no);

                        message->count += recv_count;
                        assert(message->count < message->limit);
                        message->data[message->count] = '\0';
                    }
                }
            }
        }

        //
        // We've read everything we can from our sockets. Now try parsing the messages.
        //

        for (s64 request_index = 0; request_index < pending_requests->count; request_index += 1) {
            s32 client_socket_no = pending_requests->keys[request_index];
            Pending_request *pending_request = &pending_requests->vals[request_index];

            Memory_Context *ctx = pending_request->context;
            u8_array *message = &pending_request->message;

            if (!message->count)  continue;

            //|Speed: We shouldn't try to parse again unless we received more bytes when we polled.

            Request *request = parse_request(message->data, message->count, ctx);

            if (!request) {
                if (VERBOSE) {
                    char_array out = {.context = ctx};
                    print_string(&out, "We couldn't parse this message from socket %d:\n", client_socket_no);
                    print_string(&out, "---\n");
                    print_string(&out, "%s\n", message->data);
                    print_string(&out, "---\n");
                    Log(out.data);
                }
                continue;
            }

            if (VERBOSE)  Log("Successfully parsed a message from socket %d.", client_socket_no);

            Request_handler *handler = NULL;
            for (s64 route_index = 0; route_index < countof(routes); route_index += 1) {
                Route *route = &routes[route_index];

                if (route->method != request->method)            continue;
                if (!is_match(request->path.data, route->path))  continue;

                handler = route->handler;
            }

            if (!handler) {
                if (VERBOSE)  Log("We couldn't find a handler for this request, so we're returning a 404.");

                handler = &handle_404;
            }

            Response response = (*handler)(request, ctx);
            {
                char *method = request->method == GET ? "GET" : request->method == POST ? "POST" : "UNKNOWN!!";
                char *query = request->query ? encode_query_string(request->query, ctx)->data : "";

                Log("[%d] %s %s%s", response.status, method, request->path.data, query);
            }

            char_array reply = {.context = ctx}; //|Inconsistent: The reply buffer is char_array but the request buffer is u8_array for no apparent reason. In fact, our usual assumption, that char_arrays are null-terminated whereas u8_arrays aren't, is actually reversed here.

            // Print the response headers.
            print_string(&reply, "HTTP/1.1 %d\n", response.status); //|Fixme: Version??
            if (response.headers) {
                string_dict *h = response.headers;
                for (s64 i = 0; i < h->count; i++)  print_string(&reply, "%s: %s\n", h->keys[i], h->vals[i]);
            }
            print_string(&reply, "\n");

            // Copy the response body. |Speed
            if (reply.count + response.size > reply.limit) {
                array_reserve(&reply, reply.count + response.size);
            }
            memcpy(&reply.data[reply.count], response.body, response.size);
            reply.count += response.size;

            int flags = 0;
            s64 num_bytes_sent = send(client_socket_no, reply.data, reply.count, flags);

            assert(num_bytes_sent == reply.count); // |Fixme: You can trigger this assert by spamming F5 in the browser; sometimes we send less than the full buffer.

            pending_request->phase = READY_TO_CLOSE;
        }

cleanup:
        // Remove sockets that have timed out or that are marked "ready to close".
        for (s64 request_index = 0; request_index < pending_requests->count; request_index++) {
            Pending_request *pending_request = &pending_requests->vals[request_index];

            // Filter out the sockets that we shouldn't close, unless the server should stop, in which
            // case we will disconnect everyone.
            if (!server_should_stop) {
                enum Request_phase phase = pending_request->phase;
                if (phase != READY_TO_CLOSE) {
                    assert(phase == PARSING);
                    s64 age = current_time - pending_request->start_time;
                    if (age < REQUEST_TIMEOUT)  continue;
                }
            }

            s32 socket_no = pending_request->socket_no;

            bool closed = !close(socket_no);
            if (!closed)  Fatal("We couldn't close a client socket (%s).", strerror(errno));

            s64 pollfd_index = -1;
            for (s64 i = 2; i < pollfds.count; i++) {
                if (pollfds.data[i].fd == socket_no) {
                    pollfd_index = i;
                    break;
                }
            }
            assert(pollfd_index >= 0);

            array_unordered_remove_by_index(&pollfds, pollfd_index);
            Delete(pending_requests, socket_no);

            if (VERBOSE)  Log("Closed and deleted socket %d.", socket_no);
        }
    }

    if (close(my_socket_no) < 0)  Error("We couldn't close our own socket (%s).", strerror(errno));

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
