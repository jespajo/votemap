// Factor everything out into a module!

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
typedef Array(int)    int_array;

typedef struct Route    Route;
typedef struct Request  Request;
typedef struct Response Response;
typedef Response Request_handler(Request *, Memory_Context *);
typedef struct Pending_request Pending_request;

enum HTTP_method {GET=1, POST};

struct Route {
    enum HTTP_method    method;
    char               *path;
    Request_handler    *handler;
};

struct Request {
    enum HTTP_method    method;
    char_array          path;
    string_dict        *query;
};

struct Response {
    int                 status;
    string_dict        *headers;
    void               *body;
    s64                 size; // The number of bytes in the body. If 0, body (if set) points to a zero-terminated string.
};

struct Pending_request {
    Memory_Context     *context;

    s64                 start_time;      // When we accepted the connection.
    s32                 socket_no;       // The client socket's file descriptor.

    enum Request_phase {
        PARSING_REQUEST=1,
        HANDLING_REQUEST,
        SENDING_REPLY,
        READY_TO_CLOSE,
    }                   phase;

    char_array          inbox;           // A buffer for storing bytes received.
    Request             request;

    Response            response;

    char_array          reply_header;    // Our response's header in raw text form.
    s64                 num_bytes_sent;  // The total number of bytes we've sent of our response. Includes both header and body.
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

    return (Response){status, headers, body->data, body->count};
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
//|Insecure!! This function will serve any file in your filesystem, even supporting '..' in paths to go up a directory.
{
    char *path = request->path.data;
    s64 path_size = request->path.count;

    // If the path starts with a '/', "remove" it by advancing the pointer 1 byte.
    if (*path == '/') {
        path      += 1;
        path_size -= 1;
    }

    u8_array *file = load_binary_file(path, context);

    if (!file)  return (Response){404, .body = "We couldn't find that file.\n"};

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

bool parse_request(Pending_request *pending_request)
// Parse the message in pending_request->inbox and save the result in pending_request->request.
// If we successfully parse a request, set pending_request->phase to HANDLING_REQUEST and return true.
// If the request is invalid, fill out pending_request->response, set the phase to SENDING_REPLY and
// return true. Return false only if the request looks fine but incomplete.
{
    Memory_Context     *ctx          = pending_request->context;
    enum Request_phase *phase        = &pending_request->phase;
    char_array         *inbox        = &pending_request->inbox;
    Request            *request      = &pending_request->request;
    Response           *response     = &pending_request->response;

    assert(*phase == PARSING_REQUEST);
    assert(inbox->count);

    char *data = inbox->data;
    s64   size = inbox->count;

    if (size < 4)  return false;

    char *d = data; // A pointer to advance as we parse.

    if (starts_with(d, "GET ")) {
        request->method = GET;
        d += 4;
    } else {
        *response = (Response){501, .body = "We only support GET requests!\n"};
        *phase = SENDING_REPLY;
        return true;
    }

    // Other than alphanumeric characters, these are the only characters we don't treat specially in paths and query strings.
    // They're RFC 3986's unreserved characters plus a few.
    char const ALLOWED[] = "-._~/,+"; //|Cleanup: Defined twice.

    //
    // Parse the path and query string.
    //
    {
        request->path = (char_array){.context = ctx};

        string_dict *query = NewDict(query, ctx); // We'll only add this to the request if we fully parse a query string.

        // At first we're reading the request path. If we come to a query string, our target alternates between a pending key and value.
        char_array *target = &request->path;

        char_array key   = {.context = ctx};
        char_array value = {.context = ctx};

        while (d-data < size) {
            if (is_alphanum(*d) || Contains(ALLOWED, *d)) {
                *Add(target) = *d;
            } else if (*d == '%') {
                if (d-data + 2 >= size)        break;
                if (!is_hex(d[1]))             break;
                if (!is_hex(d[2]))             break;
                // It's a hex-encoded byte.
                *Add(target) = hex_to_char(d[1], d[2]);
                d += 2;
            } else if (*d == '?') {
                if (target != &request->path)  break;
                target = &key;
            } else if (*d == '=') {
                if (target != &key)            break;
                if (key.count == 0)            break;
                target = &value;
            } else if (*d == '&' || *d == ' ') {
                if (key.count) {
                    // Add the previous key/value we were building.
                    *Add(&key)   = '\0';
                    *Add(&value) = '\0'; // If the query string has keys without values, we put empty strings for values.

                    *Set(query, key.data) = value.data;

                    key   = (char_array){.context = ctx};
                    value = (char_array){.context = ctx};
                }
                if (*d == ' ')  break; // Success!
                target = &key;
            } else {
                // There was an unexpected character.
                break;
            }
            d += 1;
        }

        // If we've come to the end of the available data, assume there's more to come. Don't worry about
        // setting a max URI length, because we'll set limits on the request size overall.
        if (d-data == size)  return false;

        if (*d != ' ') {
            // We didn't finish at a space, which means we came to an unexpected character in the URI.
            // If we managed to parse a path and we were onto a query string, we'll allow it.
            if (request->path.count && target != &request->path) {
                // Advance the data pointer to the next space character after the URI.
                while (d-data < size) {
                    d += 1;
                    if (*d == ' ')  break;
                }
            } else {
                // Otherwise, the request is bunk.
                char_array message = {.context = ctx};
                print_string(&message, "The request had an unexpected character at index %ld: ", d-data);
                print_string(&message, is_alphanum(*d) ? "'%c'.\n" : "\\x%02x.\n", *d);

                *response = (Response){400, .body = message.data, message.count};
                *phase = SENDING_REPLY;
                return true;
            }
        }

        *Add(&request->path) = '\0';
        request->path.count -= 1;

        if (query->count)  request->query = query;
    }

    if (!starts_with(d, " HTTP/")) {
        *response = (Response){400, .body = "There was an unexpected character in the HTTP version.\n"};
        *phase = SENDING_REPLY;    // Fail.
    } else {
        *phase = HANDLING_REQUEST; // Success.
    }

    return true;
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

    s64 milliseconds = 1000*time.tv_sec + time.tv_nsec/1e6;

    return milliseconds;
}

int main()
{
    //// Call the old main function so we can serve the files it creates. |Temporary.
    //int once_and_future_main();
    //once_and_future_main();

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
    }

    Map(s32, Pending_request) *pending_requests = NewMap(pending_requests, top_context);

    bool server_should_stop = false;

    Memory_Context *frame_ctx = new_context(top_context); // A "frame" is one iteration of the main loop.

    while (!server_should_stop)
    {
        reset_context(frame_ctx);

        // Build an array of file descriptors to poll. The first two elements in the array never change:
        // pollfds.data[0] is standard input and pollfds.data[1] is our main socket listening for
        // connections. If there are any other elements in the array, they are open client connections.
        Array(struct pollfd) pollfds = {.context = frame_ctx};
        {
            *Add(&pollfds) = (struct pollfd){.fd = STDIN_FILENO, .events = POLLIN};

            *Add(&pollfds) = (struct pollfd){.fd = my_socket_no, .events = POLLIN};

            for (s64 i = 0; i < pending_requests->count; i++) {
                Pending_request *pending_request = &pending_requests->vals[i];

                struct pollfd pollfd = {.fd = pending_request->socket_no};

                switch (pending_request->phase) {
                    case PARSING_REQUEST:  pollfd.events |= POLLIN;   break;
                    case SENDING_REPLY:    pollfd.events |= POLLOUT;  break;

                    default:  assert(!"Unexpected request phase.");
                }
                assert(pollfd.events);

                *Add(&pollfds) = pollfd;
            }
        }

        if (VERBOSE)  Log("Polling %ld open file descriptors.", pollfds.count);

        // If there aren't any open connections, wait indefinitely. If there are connections, poll
        // once per second so we can check if any connection has expired.
        int timeout_ms = (pollfds.count-2 > 0) ? 1000 : -1;

        int num_events = poll(pollfds.data, pollfds.count, timeout_ms);

        if (num_events < 0)  Fatal("poll failed (%s).", strerror(errno));

        s64 current_time = get_monotonic_time(); // We need to get this value after poll, which might have blocked for a long time, but before jumping to cleanup.

        // If poll() timed out without any events occurring, skip trying to process requests.
        if (!num_events)  goto cleanup;
        // Otherwise, num_events is a positive number.

        for (s64 pollfd_index = 0; pollfd_index < pollfds.count; pollfd_index += 1) {
            struct pollfd *pollfd = &pollfds.data[pollfd_index];

            if (!pollfd->revents)  continue;

            if (pollfd->fd == STDIN_FILENO) {
                assert(pollfd->revents == POLLIN);

                if (VERBOSE)  Log("There's something to read on standard input.");

                // We should exit gracefully if it says "q" or something. |Temporary: Exit on any input from stdin.
                server_should_stop = true;
                goto cleanup;
            } else if (pollfd->fd == my_socket_no) {
                assert(pollfd->revents == POLLIN);

                // A new connection has occurred.
                struct sockaddr_in client_socket_addr; // This will be initialised by accept().
                socklen_t client_socket_addr_size = sizeof(client_socket_addr);

                int client_socket_no = accept(my_socket_no, (struct sockaddr *)&client_socket_addr, &client_socket_addr_size);
                if (client_socket_no < 0)  Fatal("poll() said we could read from our main socket, but we couldn't get a new connection (%s).", strerror(errno));

                set_blocking(client_socket_no, false);

                if (VERBOSE)  Log("Adding a new client (socket %d).", client_socket_no);

                *Add(&pollfds) = (struct pollfd){
                    .fd      = client_socket_no,
                    .events  = POLLIN|POLLOUT,
                    .revents = POLLIN, // We also set .revents ourselves so that we'll try receiving from this socket (after the others) instead of waiting to poll them all again. |Hack
                };

                assert(Get(pending_requests, client_socket_no) == &pending_requests->vals[-1]); //|Temporary: We shouldn't have a request associated with this client socket yet. |Cleanup:IsSet()

                Memory_Context *request_context = new_context(top_context);

                *Set(pending_requests, client_socket_no) = (Pending_request){
                    .context      = request_context,
                    .start_time   = current_time,
                    .socket_no    = client_socket_no,
                    .phase        = PARSING_REQUEST,
                };
            } else if (pollfd->revents & POLLIN) {
                // There's something to read on a client socket.
                int client_socket_no = pollfd->fd;
                Pending_request *pending_request = Get(pending_requests, client_socket_no);
                assert(pending_request->phase == PARSING_REQUEST);

                if (VERBOSE)  Log("Socket %d has something to say!!", client_socket_no);

                char_array *inbox = &pending_request->inbox;
                if (!inbox->limit) {
                    *inbox = (char_array){.context = pending_request->context};
                    array_reserve(inbox, BUFSIZ);
                }

                while (true) {
                    char *free_data = inbox->data + inbox->count;
                    s64 num_free_bytes = inbox->limit - inbox->count - 1; // Reserve space for a null byte.
                    if (num_free_bytes <= 0) {
                        array_reserve(inbox, round_up_pow2(inbox->limit+1));
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
                        goto cleanup;
                    } else {
                        // We have successfully received some bytes.
                        if (VERBOSE)  Log("Read %ld bytes from socket %d.", recv_count, client_socket_no);

                        inbox->count += recv_count;
                        assert(inbox->count < inbox->limit);
                        inbox->data[inbox->count] = '\0';
                    }
                }
            } else if (pollfd->revents & POLLOUT) {
                // We can write to a client socket.
                int client_socket_no = pollfd->fd;
                Pending_request *pending_request = Get(pending_requests, client_socket_no);
                assert(pending_request->phase == SENDING_REPLY);

                if (VERBOSE)  Log("We're about to write to socket %d.", client_socket_no);

                char_array *reply_header   = &pending_request->reply_header;
                Response   *response       = &pending_request->response;
                s64        *num_bytes_sent = &pending_request->num_bytes_sent;

                s64 full_reply_size = reply_header->count + response->size;
                assert(*num_bytes_sent < full_reply_size);

                while (*num_bytes_sent < full_reply_size) {
                    void *data_to_send      = NULL;
                    s64   num_bytes_to_send = 0;
                    if (*num_bytes_sent < reply_header->count) {
                        // We're still sending the response header.
                        data_to_send      = reply_header->data  + *num_bytes_sent;
                        num_bytes_to_send = reply_header->count - *num_bytes_sent;
                    } else {
                        // We're sending the response body.
                        s64 body_sent = *num_bytes_sent - reply_header->count;

                        data_to_send      = response->body + body_sent;
                        num_bytes_to_send = response->size - body_sent;
                    }
                    assert(data_to_send);
                    assert(num_bytes_to_send > 0);

                    int flags = 0;
                    s64 send_count = send(client_socket_no, data_to_send, num_bytes_to_send, flags);
                    if (send_count < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK)  break;
                        else  Fatal("send failed (%s).", strerror(errno));
                    }
                    assert(send_count > 0);

                    *num_bytes_sent += send_count;
                }

                if (*num_bytes_sent < full_reply_size) {
                    // We've partially sent our reply.
                    if (VERBOSE)  Log("Sent %ld/%ld bytes to socket %d.", *num_bytes_sent, full_reply_size, client_socket_no);
                } else {
                    // We've fully sent our reply.
                    assert(*num_bytes_sent == full_reply_size);
                    if (VERBOSE) {
                        Memory_Context *ctx = pending_request->context;
                        Request *request = &pending_request->request;

                        char *method = request->method == GET ? "GET" : request->method == POST ? "POST" : "UNKNOWN!!";
                        char *query = request->query ? encode_query_string(request->query, ctx)->data : "";

                        Log("[%d] %s %s%s", response->status, method, request->path.data, query);
                    }

                    pending_request->phase = READY_TO_CLOSE;
                }
            }
        }

        //
        // Try to parse and handle pending requests.
        //

        for (s64 request_index = 0; request_index < pending_requests->count; request_index += 1) {
            s32 client_socket_no = pending_requests->keys[request_index];
            Pending_request *pending_request = &pending_requests->vals[request_index];

            if (pending_request->phase != PARSING_REQUEST)  continue;

            Memory_Context *ctx      = pending_request->context;
            char_array     *inbox    = &pending_request->inbox;
            Request        *request  = &pending_request->request;
            Response       *response = &pending_request->response;

            if (!inbox->count)  continue;

            //|Speed: We shouldn't try to parse again unless we received more bytes when we polled.

            bool parsed = parse_request(pending_request);

            if (!parsed)  continue;

            if (VERBOSE)  Log("Successfully parsed a message from socket %d.", client_socket_no);

            if (pending_request->phase == HANDLING_REQUEST) {
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

                *response = (*handler)(request, ctx);
            } else {
                assert(pending_request->phase == SENDING_REPLY);
            }

            if (response->body && !response->size)  response->size = strlen(response->body);

            char_array *reply_header = &pending_request->reply_header;
            *reply_header = (char_array){.context = pending_request->context};

            print_string(reply_header, "HTTP/1.0 %d\n", response->status);
            if (response->headers) {
                string_dict *h = response->headers;
                for (s64 i = 0; i < h->count; i++)  print_string(reply_header, "%s: %s\n", h->keys[i], h->vals[i]);
            }
            print_string(reply_header, "\n");

            pending_request->phase = SENDING_REPLY;
            //|Todo: Can we jump straight to trying to write to the socket?
        }

cleanup:
        // Remove sockets that have timed out or that are marked "ready to close".
        for (s64 request_index = 0; request_index < pending_requests->count; request_index++) {
            Pending_request *pending_request = &pending_requests->vals[request_index];

            // Filter out the sockets that we shouldn't close, unless the server should stop, in which
            // case we will disconnect everyone.
            bool should_close = server_should_stop;
            should_close |= (pending_request->phase == READY_TO_CLOSE);
            should_close |= (REQUEST_TIMEOUT < (current_time - pending_request->start_time));
            if (!should_close)  continue;

            s32 socket_no = pending_request->socket_no;

            //|Todo: Finish sending pending replies first. Maybe have a "graceful" flag to make this configurable.

            bool closed = !close(socket_no);
            if (!closed)  Fatal("We couldn't close a client socket (%s).", strerror(errno));

            free_context(pending_request->context);
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
