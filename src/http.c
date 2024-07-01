// For clock_gettime() we need _POSIX_C_SOURCE >= 199309L.
// For strerror_r()    we need _POSIX_C_SOURCE >= 200112L.
#define _POSIX_C_SOURCE 200112L

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "http.h"
#include "strings.h"

static char *get_error(int errno_)
{
#if OS != LINUX
    #error "get_error() is only implemented for Linux."
#endif
    // We don't want to have to pass a buffer or Memory_Context argument to functions that only need to allocate when
    // they fail, just so they can print system error messages. So we use a static buffer, which is |Threadunsafe, which
    // completely defeats the purpose of using strerror_r() rather than strerror(). But if/when we make this program
    // multithreaded, I think all we'll need to do is lock a mutex before accessing this buffer.
    char static message[256] = {0};

    if (strerror_r(errno_, message, lengthof(message)))  Fatal("We couldn't get the system's error message!");

    return message;
}

static s64 get_monotonic_time()
// In milliseconds.
{
#if OS != LINUX
    #error "get_monotonic_time() is only implemented for Linux."
#endif
    struct timespec time;
    bool success = !clock_gettime(CLOCK_MONOTONIC, &time);

    if (!success)  Fatal("clock_gettime failed (%s).", get_error(errno));

    s64 milliseconds = 1000*time.tv_sec + time.tv_nsec/1.0e6;

    return milliseconds;
}

static void set_blocking(int file_no, bool blocking)
// file_no is an open file descriptor.
{
#if OS != LINUX
    #error "set_blocking() is only implemented for Linux."
#endif
    int flags = fcntl(file_no, F_GETFL, 0);

    if (flags == -1)  Fatal("fcntl failed (%s).", get_error(errno));

    if (blocking)  flags &= ~O_NONBLOCK;
    else           flags |= O_NONBLOCK;

    bool success = !fcntl(file_no, F_SETFL, flags);

    if (!success)  Fatal("fcntl failed (%s).", get_error(errno));
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

static bool parse_request(Client *client)
// Parse the message in client->message and save the result in client->request. If we successfully parse a request,
// set client->phase to HANDLING_REQUEST and return true. If the request is invalid, fill out client->response, set
// the phase to SENDING_REPLY and return true. Return false only if the request looks fine but incomplete.
{
    Memory_Context *ctx = client->context;

    assert(client->phase == PARSING_REQUEST);
    assert(client->message.count);

    char *data = client->message.data;
    s64   size = client->message.count;

    if (size < 4)  return false;

    char *d = data; // A pointer to advance as we parse.

    if (starts_with(d, "GET ")) {
        client->request.method = GET;
        d += 4;
    } else {
        client->response = (Response){501, .body = "We only support GET requests!\n"};
        client->phase = SENDING_REPLY;
        return true;
    }

    // Other than alphanumeric characters, these are the only characters we don't treat specially in paths and query strings.
    // They're RFC 3986's unreserved characters plus a few.
    char const ALLOWED[] = "-._~/,+"; //|Cleanup: Defined twice.

    //
    // Parse the path and query string.
    //
    {
        char_array *path = &client->request.path;
        *path = (char_array){.context = ctx};

        string_dict *query = NewDict(query, ctx); // We'll only add this to the result if we fully parse a query string.

        // At first we're reading the request path. If we come to a query string, our target alternates between a pending key and value.
        char_array *target = path;

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
                if (target != path)            break;
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
            if (path->count && target != path) {
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

                client->response = (Response){400, .body = message.data, message.count};
                client->phase = SENDING_REPLY;
                return true;
            }
        }

        *Add(path) = '\0';
        path->count -= 1;

        if (query->count)  client->request.query = query;
    }

    if (!starts_with(d, " HTTP/")) {
        client->response = (Response){400, .body = "There was an unexpected character in the HTTP version.\n"};
        client->phase = SENDING_REPLY;     // Fail.
    } else {
        client->phase = HANDLING_REQUEST;  // Success.
    }

    return true;
}

static char_array *encode_query_string(string_dict *query, Memory_Context *context)
{
    char_array *result = NewArray(result, context);

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

Server *create_server(u32 address, u16 port, bool verbose, Memory_Context *context)
{
    Server *server = New(Server, context);

    server->address = address;
    server->port    = port;
    server->verbose = verbose;
    server->context = context;
    server->routes  = (Route_array){.context = context};
    server->clients = NewMap(server->clients, context);

    server->socket_no = socket(AF_INET, SOCK_STREAM, 0);
    if (server->socket_no < 0)  Fatal("Couldn't get a socket (%s).", get_error(errno));

    // Set SO_REUSEADDR because we want to run this program frequently during development.
    // Otherwise the kernel holds onto our address/port combo after our program finishes.
    if (setsockopt(server->socket_no, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int)) < 0) {
        Fatal("Couldn't set socket options (%s).", get_error(errno));
    }

    set_blocking(server->socket_no, false);

    struct sockaddr_in socket_addr = {
        .sin_family   = AF_INET,
        .sin_port     = htons(server->port),
        .sin_addr     = htonl(server->address),
    };

    if (bind(server->socket_no, (struct sockaddr const *)&socket_addr, sizeof(socket_addr)) < 0) {
        Fatal("Couldn't bind socket (%s).", get_error(errno));
    }

    int QUEUE_LENGTH = 32;
    if (listen(server->socket_no, QUEUE_LENGTH) < 0)  Fatal("Couldn't listen on socket (%s).", get_error(errno));

    Log("Listening on http://...", address>>24, address>>16&0xff, address>>8&0xff, address&0xff, port);

    return server;
}

void start_server(Server *server)
{
    Client_map  *clients = server->clients;
    Route_array *routes  = &server->routes;

    bool server_should_stop = false;

    Memory_Context *frame_ctx = new_context(server->context); // A "frame" is one iteration of the main loop.

    while (!server_should_stop)
    {
        reset_context(frame_ctx);

        // Build an array of file descriptors to poll. The first two elements in the array never change:
        // pollfds.data[0] is standard input and pollfds.data[1] is our main socket listening for
        // connections. If there are any other elements in the array, they are open client connections.
        Array(struct pollfd) pollfds = {.context = frame_ctx};

        *Add(&pollfds) = (struct pollfd){.fd = STDIN_FILENO, .events = POLLIN};

        *Add(&pollfds) = (struct pollfd){.fd = server->socket_no, .events = POLLIN};

        for (s64 i = 0; i < clients->count; i++) {
            Client *client = &clients->vals[i];

            struct pollfd pollfd = {.fd = client->socket_no};

            switch (client->phase) {
                case PARSING_REQUEST:  pollfd.events |= POLLIN;   break;
                case SENDING_REPLY:    pollfd.events |= POLLOUT;  break;

                default:  assert(!"Unexpected request phase.");
            }
            assert(pollfd.events);

            *Add(&pollfds) = pollfd;
        }

        if (server->verbose)  Log("Polling %ld open file descriptors.", pollfds.count);

        // If there aren't any open connections, wait indefinitely. If there are connections, poll
        // once per second so we can check if any connection has expired.
        int timeout_ms = (pollfds.count-2 > 0) ? 1000 : -1;

        int num_events = poll(pollfds.data, pollfds.count, timeout_ms);

        if (num_events < 0)  Fatal("poll failed (%s).", get_error(errno));

        s64 current_time = get_monotonic_time(); // We need to get this value after polling, but before jumping to cleanup.

        // If poll() timed out without any events occurring, skip trying to process requests.
        if (!num_events)  goto cleanup;
        assert(num_events > 0);

        for (s64 pollfd_index = 0; pollfd_index < pollfds.count; pollfd_index += 1) {
            struct pollfd *pollfd = &pollfds.data[pollfd_index];

            if (!pollfd->revents)  continue;

            if (pollfd->fd == STDIN_FILENO) {
                // There's something to read on standard input.
                char_array input = {.context = frame_ctx};
                while (true) {
                    int c = fgetc(stdin);
                    if (c == EOF || c == '\n')  break;

                    *Add(&input) = (char)c;
                }

                // Exit if the input was just the letter 'q'. Otherwise ignore it.
                if (input.count == 1 && input.data[0] == 'q') {
                    server_should_stop = true;
                    goto cleanup;
                }
            } else if (pollfd->fd == server->socket_no) {
                // A new connection has occurred.
                struct sockaddr_in client_socket_addr; // This will be initialised by accept().
                socklen_t client_socket_addr_size = sizeof(client_socket_addr);

                int client_socket_no = accept(server->socket_no, (struct sockaddr *)&client_socket_addr, &client_socket_addr_size);
                if (client_socket_no < 0)  Fatal("poll() said we could read from our main socket, but we couldn't get a new connection (%s).", get_error(errno));

                set_blocking(client_socket_no, false);

                if (server->verbose)  Log("Adding a new client (socket %d).", client_socket_no);

                *Add(&pollfds) = (struct pollfd){
                    .fd      = client_socket_no,
                    .events  = POLLIN|POLLOUT,
                    .revents = POLLIN, // We also set .revents ourselves so that we'll try receiving from this socket on this frame. |Hack
                };

                assert(Get(clients, client_socket_no) == &clients->vals[-1]); // We shouldn't have a request associated with this client socket yet. |Cleanup: IsSet()

                *Set(clients, client_socket_no) = (Client){
                    .context      = new_context(server->context),
                    .start_time   = current_time,
                    .socket_no    = client_socket_no,
                    .phase        = PARSING_REQUEST,
                };
            } else if (pollfd->revents & POLLIN) {
                // There's something to read on a client socket.
                int client_socket_no = pollfd->fd;
                Client *client = Get(clients, client_socket_no);
                assert(client->phase == PARSING_REQUEST);

                if (server->verbose)  Log("Socket %d has something to say!!", client_socket_no);

                char_array *message = &client->message;
                if (!message->limit) {
                    *message = (char_array){.context = client->context};

                    s64 INITIAL_RECV_BUFFER_SIZE = 2048;
                    array_reserve(message, INITIAL_RECV_BUFFER_SIZE);
                }

                while (true) {
                    char *free_data = message->data + message->count;
                    s64 num_free_bytes = message->limit - message->count - 1; // Reserve space for a null byte.
                    if (num_free_bytes <= 0) {
                        array_reserve(message, round_up_pow2(message->limit+1));
                        continue;
                    }

                    if (server->verbose)  Log("We're about to read socket %d.", client_socket_no);

                    int flags = 0;
                    s64 recv_count = recv(client_socket_no, free_data, num_free_bytes, flags);

                    if (recv_count < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            // There's nothing more to read now.
                            if (server->verbose)  Log("There was nothing to read.");
                            break;
                        } else {
                            // There was an actual error.
                            Fatal("We failed to read from socket %d (%s).", client_socket_no, get_error(errno));
                        }
                    } else if (recv_count == 0) {
                        // The client has disconnected.
                        if (server->verbose)  Log("Socket %d has disconnected.", client_socket_no);

                        client->phase = READY_TO_CLOSE;
                        goto cleanup;
                    } else {
                        // We have successfully received some bytes.
                        if (server->verbose)  Log("Read %ld bytes from socket %d.", recv_count, client_socket_no);

                        message->count += recv_count;
                        assert(message->count < message->limit);
                        message->data[message->count] = '\0';
                    }
                }
            } else if (pollfd->revents & POLLOUT) {
                // We can write to a client socket.
                int client_socket_no = pollfd->fd;
                Client *client = Get(clients, client_socket_no);
                assert(client->phase == SENDING_REPLY);

                if (server->verbose)  Log("We're about to write to socket %d.", client_socket_no);

                char_array *reply_header   = &client->reply_header;
                Response   *response       = &client->response;
                s64        *num_bytes_sent = &client->num_bytes_sent;

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
                        else  Fatal("send failed (%s).", get_error(errno));
                    }
                    assert(send_count > 0);

                    *num_bytes_sent += send_count;
                }

                if (*num_bytes_sent < full_reply_size) {
                    // We've partially sent our reply.
                    if (server->verbose)  Log("Sent %ld/%ld bytes to socket %d.", *num_bytes_sent, full_reply_size, client_socket_no);
                } else {
                    // We've fully sent our reply.
                    assert(*num_bytes_sent == full_reply_size);
                    if (server->verbose) {
                        Memory_Context *ctx = client->context;
                        Request *req = &client->request;
                        char *method = req->method == GET ? "GET" : req->method == POST ? "POST" : "UNKNOWN!!";
                        char *path   = req->path.count ? req->path.data : "";
                        char *query  = req->query ? encode_query_string(req->query, ctx)->data : "";
                        Log("[%d] %s %s%s", response->status, method, path, query);
                    }

                    client->phase = READY_TO_CLOSE;
                }
            }
        }

        //
        // Try to parse and handle pending requests.
        //
        for (s64 request_index = 0; request_index < clients->count; request_index += 1) {
            Client   *client   = &clients->vals[request_index];
            Request  *request  = &client->request;
            Response *response = &client->response;

            if (client->phase != PARSING_REQUEST)        continue;
            if (client->message.count == 0)              continue;

            //|Todo: Don't try to parse unless we received more bytes when we last polled.

            // Parse the request.
            bool parsed = parse_request(client);
            if (!parsed)  continue;

            if (server->verbose)  Log("Successfully parsed a message from socket %d.", client->socket_no);

            if (client->phase == HANDLING_REQUEST) {
                // Find a matching handler.
                Request_handler *handler = NULL;
                for (s64 i = 0; i < routes->count; i++) {
                    Route *route = &routes->data[i];

                    bool match = (route->method == request->method);
                    match &= is_match(route->path, request->path.data);

                    if (match)  handler = route->handler;
                }
                if (!handler) {
                    if (server->verbose)  Log("We couldn't find a handler for this request, so we're returning a 404.");
                    handler = &serve_404;
                }

                // Run the handler.
                *response = (*handler)(request, client->context);
            } else {
                assert(client->phase == SENDING_REPLY);
            }

            // If the response has a body but no size, calculate the size assuming the body is a zero-terminated string.
            if (response->body && !response->size)  response->size = strlen(response->body);

            char_array *reply_header = &client->reply_header;
            *reply_header = (char_array){.context = client->context};

            print_string(reply_header, "HTTP/1.0 %d\n", response->status);
            if (response->headers) {
                string_dict *h = response->headers;
                for (s64 i = 0; i < h->count; i++)  print_string(reply_header, "%s: %s\n", h->keys[i], h->vals[i]);
            }
            print_string(reply_header, "\n");

            client->phase = SENDING_REPLY;
            //|Todo: Can we jump straight to trying to write to the socket?
        }

cleanup:
        // Remove sockets that have timed out or that are marked "ready to close".
        for (s64 request_index = 0; request_index < clients->count; request_index++) {
            Client *client = &clients->vals[request_index];

            s64 CONNECTION_TIMEOUT = 5*1000; // The maximum age of any connection in milliseconds.

            // Skip sockets that we shouldn't close, unless the server should stop, in which case we will disconnect everyone.
            bool should_close = server_should_stop;
            should_close |= (client->phase == READY_TO_CLOSE);
            should_close |= (CONNECTION_TIMEOUT < (current_time - client->start_time));
            if (!should_close)  continue;

            s32 client_socket_no = client->socket_no;

            //|Todo: Finish sending pending replies first. Maybe have a "graceful" flag to make this configurable.

            bool closed = !close(client_socket_no);
            if (!closed)  Fatal("We couldn't close a client socket (%s).", get_error(errno));

            free_context(client->context);
            Delete(clients, client_socket_no);

            if (server->verbose)  Log("Closed and deleted socket %d.", client_socket_no);
        }
    }

    if (close(server->socket_no) < 0)  Fatal("We couldn't close our own socket (%s).", get_error(errno));
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

Response serve_404(Request *request, Memory_Context *context)
{
    char const static body[] = "Can't find it.\n";

    return (Response){
        .status  = 404,
        .headers = NULL,
        .body    = (u8 *)body,
        .size    = lengthof(body),
    };
}
