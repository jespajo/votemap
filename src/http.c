// For sigemptyset and sigaddset, we need to define _POSIX_C_SOURCE (as anything).
// For pthread_sigmask, we need to define _POSIX_C_SOURCE >= 199506L.
#define _POSIX_C_SOURCE 199506L
#include <signal.h>
#include <sys/signalfd.h>

#include <arpa/inet.h>
#include <ctype.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "http.h"
#include "strings.h"
#include "system.h"

struct Client {
    Memory_context         *context;

    s32                     socket;         // The client socket's file descriptor.
    s64                     start_time;     // When we accepted the connection.

    enum {
        PARSING_REQUEST=1,
        HANDLING_REQUEST,
        SENDING_REPLY,
        READY_TO_CLOSE,
    }                       phase;

    char_array              message;        // A buffer for storing bytes received.
    s16_array               crlf_offsets;
    Request                 request;

    Response                response;

    char_array              reply_header;   // Our response's header in text form.
    s64                     num_bytes_sent; // The total number of bytes we've sent of our response. Includes both header and body.

    enum {
        HTTP_VERSION_1_0=1,
        HTTP_VERSION_1_1,
    }                       http_version;
    bool                    keep_alive;     // Whether to keep the socket open after processing the request.
};

struct Client_queue {
    pthread_mutex_t         mutex;
    pthread_cond_t          ready;          // The server will broadcast when there are tasks.

    Array(Client*);
    Client                **head;           // A pointer to the first element in the array that hasn't been taken from the queue. If it points to the element after the end of the array, the queue is empty.
};

static Client_queue *create_queue(Memory_context *context)
{
    Client_queue *queue = NewArray(queue, context);

    pthread_mutex_init(&queue->mutex, NULL);
    pthread_cond_init(&queue->ready, NULL);

    array_reserve(queue, 8);
    queue->head = queue->data;

    return queue;
}

static void add_to_queue(Client_queue *queue, Client *client)
{
    Client_queue *q = queue;

    pthread_mutex_lock(&q->mutex);

    s64 head_index = q->head - q->data;
    assert(0 <= head_index && head_index <= q->count);

    bool queue_was_empty = (head_index == q->count);

    if (q->count == q->limit && head_index > 0) {
        // We're out of room in the array, but there's space to the left of the head. Shift the head back to the start of the array.
        memmove(q->data, q->head, (q->count - head_index)*sizeof(Client*));
        q->count  -= head_index;
        head_index = 0;
    }

    *Add(q) = client;
    q->head = &q->data[head_index];

    if (queue_was_empty)  pthread_cond_broadcast(&q->ready);
    pthread_mutex_unlock(&q->mutex);
}

static Client *pop_queue(Client_queue *queue)
{
    Client_queue *q = queue;

    pthread_mutex_lock(&q->mutex);
    while (q->head == &q->data[q->count])  pthread_cond_wait(&q->ready, &q->mutex);

    Client *client = *q->head;
    q->head += 1;

    pthread_mutex_unlock(&q->mutex);
    return client;
}

static bool receive_message(Client *client)
// Try to read from the client socket. Return true if we received data and there was no error or disconnection.
{
    char_array *message = &client->message;
    if (message->limit == 0) {
        s64 INITIAL_RECV_BUFFER_SIZE = 2048;
        array_reserve(message, INITIAL_RECV_BUFFER_SIZE);
    }

    bool we_received_data = false;

    // Try to read from the client socket.
    while (true) {
        char *buffer = &message->data[message->count];
        s64 num_free_bytes = message->limit - message->count - 1; // Reserve space for a null byte.
        if (num_free_bytes <= 0) {
            array_reserve(message, round_up_pow2(message->limit+1));
            continue;
        }

        int flags = MSG_NOSIGNAL;
        s64 recv_count = recv(client->socket, buffer, num_free_bytes, flags);
        if (recv_count > 0) {
            // We have successfully received some bytes.
            message->count += recv_count;
            assert(message->count < message->limit);
            message->data[message->count] = '\0';
            we_received_data = true;
            continue;
        }
        if (recv_count < 0) {
            // Break out if we've read all the available data.
            if (errno == EAGAIN || errno == EWOULDBLOCK)  break;
            // Otherwise there was an actual error.
            log_error("We failed to read from a socket (%s).", get_last_error().string);
        }
        // We get here if there was an error (recv_count < 0) or the client disconnected (recv_count == 0).
        client->phase = READY_TO_CLOSE;
        return false;
    }

    return we_received_data;
}

// These are the characters that we treat just like regular letters and numbers when we come across them
// in paths and query strings. Everything else either has a special meaning or is not allowed.
#define ALLOWED_URI_CHARS "-._~/,+"

static bool parse_request(Client *client)
// Parse the message in client->message and save the result in client->request. If we successfully parse a request,
// set client->phase to HANDLING_REQUEST and return true. If the request is invalid, fill out client->response, set
// the phase to SENDING_REPLY and return true. Return false only if the request looks fine but incomplete.
{
    Memory_context *ctx = client->context;

    assert(client->phase == PARSING_REQUEST);
    assert(client->message.count);

    char *data = client->message.data;
    s64   size = client->message.count;

    char *d = data; // A pointer to advance as we parse.

    // First, wait until we've received the whole header.
    {
        // We're storing the CRLF offsets as 16-bit integers, which means the header can't be longer than 32768 bytes.
        // Since we don't parse POST requests yet, we'll just apply this limit to the request as a whole.
        if (size > (s64)INT16_MAX) {
            char static body[] = "The request is too large.\n";
            client->response = (Response){413, .body=body, .size=lengthof(body)};
            client->phase = SENDING_REPLY;
            return true;
        }

        s16_array *offsets = &client->crlf_offsets;

        // If we've previously noted some CRLF offsets, skip past the last one.
        if (offsets->count > 0)  d = data + offsets->data[offsets->count-1] + 2;

        bool full_header_received = false;
        char *last_cr = NULL;
        char *last_lf = NULL;

        for (; d-data < size; d++) {
            if (*d == '\r')  last_cr = d;
            if (*d != '\n')  continue;
            last_lf = d;
            if (last_cr == last_lf-1) {
                // We've found a CRLF.
                s64 offset = last_cr - data;
                assert(offset < (s64)INT16_MAX);
                full_header_received = (offsets->count > 0 && offset == offsets->data[offsets->count-1] + 2);
                if (full_header_received)  break; // Don't add the final CRLF to the offsets.
                *Add(offsets) = (s16)offset;
            }
        }

        if (!full_header_received)  return false;
    }

    d = data;

    if (starts_with(d, "GET ")) {
        client->request.method = GET;
        d += 4;
    } else {
        char static body[] = "We only support GET requests!\n";
        client->response = (Response){501, .body=body, .size=lengthof(body)};
        client->phase = SENDING_REPLY;
        return true;
    }

    //
    // Parse the path and query string.
    //
    {
        char_array  *path  = &client->request.path;
        string_dict *query = &client->request.query;

        char_array key   = {.context = ctx};
        char_array value = {.context = ctx};

        // At first we're reading the request path. If we come to a query string, our target alternates between a pending key and value.
        char_array *target = path;

        while (d-data < size) {
            if (isalnum(*d) || Contains(ALLOWED_URI_CHARS, *d)) {
                *Add(target) = *d;
            } else if (*d == '%') {
                if (d-data + 2 >= size)        break;
                if (!isxdigit(d[1]))           break;
                if (!isxdigit(d[2]))           break;
                // It's a hex-encoded byte.
                *Add(target) = (char)hex_to_byte(d[1], d[2]);
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

        if (*d != ' ') {
            // We didn't finish at a space, which means we came to an unexpected character in the URI. But if managed
            // to parse a path and we were onto a query string, we'll allow it and just disregard the query string.
            if (path->count && target != path) {
                // Advance to the next space character after the URI.
                while (d-data < size) {
                    d += 1;
                    if (*d == ' ')  break;
                }
            } else {
                // Otherwise, the request is bunk.
                char_array message = {.context = ctx};
                append_string(&message, "The request had an unexpected character at index %ld: ", d-data);
                append_string(&message, isalnum(*d) ? "'%c'.\n" : "\\x%02x.\n", *d);

                client->response = (Response){400, .body = message.data, message.count};
                client->phase = SENDING_REPLY;
                return true;
            }
        }
        assert(*d == ' ');
        d += 1;

        *Add(path) = '\0';
        path->count -= 1;
    }

    if (starts_with(d, "HTTP/1.0")) {
        client->http_version = HTTP_VERSION_1_0;
    } else if (starts_with(d, "HTTP/1.1")) {
        client->http_version = HTTP_VERSION_1_1;
        client->keep_alive   = true; // Connection: keep-alive is the default in version 1.1.
    } else {
        char static body[] = "Unsupported HTTP version.\n";
        client->response = (Response){505, .body=body, .size=lengthof(body)};
        client->phase = SENDING_REPLY;
        return true;
    }

    for (s64 i = 0; i < client->crlf_offsets.count-1; i++) {
        char *line = data + (s64)client->crlf_offsets.data[i] + 2;
        char *eol  = data + (s64)client->crlf_offsets.data[i+1];

        // Normalise the header in place.
        for (char *c = line; c < eol; c++)  *c = tolower(*c);

        if (starts_with(line, "connection:")) {
            char *value = trim_left(line + lengthof("connection:"), " \t");
            if (starts_with(value, "keep-alive"))  client->keep_alive = true;
            else if (starts_with(value, "close"))  client->keep_alive = false;

            break; // For now, the only request header that we care about is "connection".
        }
    }

    client->phase = HANDLING_REQUEST;  // Success.
    return true;
}

static Request_handler *find_request_handler(Server *server, Client *client)
// Find a route for a client and return the route handler.
{
    assert(client->phase == HANDLING_REQUEST);

    Request *request = &client->request;

    for (s64 i = 0; i < server->routes.count; i++) {
        Route *route = &server->routes.data[i];

        if (route->method != request->method)  continue;

        Match *match = run_regex(route->path_regex, request->path.data, request->path.count, client->context);
        if (match->success) {
            request->path_params = copy_capture_groups(match, client->context);

            return route->handler;
        }
    }

    return NULL;
}

static void print_response_headers(Client *client)
// Print the headers into the client->reply_header buffer.
{
    Response   *response     = &client->response;
    char_array *reply_header = &client->reply_header;

    array_reserve(reply_header, 128);

    char *version = "HTTP/1.0";
    if (client->http_version == HTTP_VERSION_1_1)  version = "HTTP/1.1";

    append_string(reply_header, "%s %d\r\n", version, response->status);

    // Add our own headers first.
    if (client->http_version == HTTP_VERSION_1_0 && client->keep_alive) {
        append_string(reply_header, "connection: keep-alive\r\n");
    } else if (client->http_version == HTTP_VERSION_1_1 && !client->keep_alive) {
        append_string(reply_header, "connection: close\r\n");
    }
    append_string(reply_header, "content-length: %ld\r\n", response->size);

    // Add the headers provided by the request handler.
    for (s64 i = 0; i < response->headers.count; i++) {
        char *key   = response->headers.keys[i];
        char *value = response->headers.vals[i];
        //|Todo: Make sure the handler didn't duplicate any of the headers we added.
        append_string(reply_header, "%s: %s\r\n", key, value);
    }
    append_string(reply_header, "\r\n");
}

static bool send_reply(Client *client)
// Return true if we successfully send the full reply. If sending would block, return false.
// If there is an error, set client->phase to READY_TO_CLOSE and return false.
{
    //
    // Our reply is split across two buffers: client.reply_header and client.response.body. We keep them
    // separate because the body is created first (by the request handler) and the header comes after that.
    // So if we wanted to put them both into one buffer, we'd have to copy the body.
    //
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

        int flags = MSG_NOSIGNAL;
        s64 send_count = send(client->socket, data_to_send, num_bytes_to_send, flags);
        if (send_count < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)  break;

            log_error("We failed to send to a client socket (%s).", get_last_error().string);
            client->phase = READY_TO_CLOSE;
            return false;
        }
        assert(send_count > 0);

        *num_bytes_sent += send_count;
    }

    // Return false if we've partially sent our reply.
    if (*num_bytes_sent < full_reply_size)  return false;

    // We've fully sent our reply. Success!
    assert(*num_bytes_sent == full_reply_size);
    return true;
}

static void init_client(Client *client, Memory_context *context, s32 socket, s64 start_time)
{
    *client                   = (Client){0};

    client->context           = context;
    client->start_time        = start_time;
    client->socket            = socket;
    client->phase             = PARSING_REQUEST;

    client->message           = (char_array){.context = context};
    client->crlf_offsets      = (s16_array){.context = context};

    client->request.path      = (char_array){.context = context};
    client->request.query     = (string_dict){.context = context};

    client->response.headers  = (string_dict){.context = context};

    client->reply_header      = (char_array){.context = context};
}

static void close_and_delete_client(Server *server, Client *client)
{
    int r = close(client->socket);
    if (r == -1) {
        Fatal("We couldn't close a client socket (%s).", get_last_error().string);
    }

    Delete(&server->clients, client->socket);
    free_context(client->context);
    dealloc(client, server->context);
}

static char_array *encode_query_string(string_dict *query, Memory_context *context)
// Take a query string that we previously parsed into a string_dict and turn it back into text form.
{
    char_array *result = NewArray(result, context);

    *Add(result) = '?';

    for (s64 i = 0; i < query->count; i++) {
        if (i)  *Add(result) = '&';

        char *key = query->keys[i];
        s64 key_len = strlen(key);

        for (s64 j = 0; j < key_len; j++) {
            char c = key[j];
            if (isalnum(c) || Contains(ALLOWED_URI_CHARS, c))  *Add(result) = c;
            else  append_string(result, "%%%02x", c);
        }

        char *val = query->vals[i];
        s64 val_len = strlen(val);

        if (!val_len)  continue;

        *Add(result) = '=';

        for (s64 j = 0; j < val_len; j++) {
            char c = val[j];
            if (isalnum(c) || Contains(ALLOWED_URI_CHARS, c))  *Add(result) = c;
            else  append_string(result, "%%%02x", c);
        }
    }

    *Add(result) = '\0';
    result->count -= 1;

    return result;
}

static void *worker_thread_routine(void *arg)
// The worker thread's main loop.
{
    Server *server = arg;
    Client_queue *queue = server->work_queue;

    while (true)
    {
        Client *client = pop_queue(queue);
        if (!client)  break; // The server adds NULL pointers to the queue to tell the workers it's time to wind up.

        if (client->phase == PARSING_REQUEST) {
            bool received = receive_message(client);

            if (received)  parse_request(client); //|Cleanup: We don't use the return value now, so maybe change the parse_request() function signature.
        }

        if (client->phase == HANDLING_REQUEST) {
            Request_handler *handler = find_request_handler(server, client);
            if (!handler)  handler = &serve_404;

            // Run the handler.
            client->response = (*handler)(&client->request, client->context);
            assert(client->response.status);

            client->phase = SENDING_REPLY;
        }

        if (client->phase == SENDING_REPLY) {
            if (!client->reply_header.count)  print_response_headers(client);

            bool success = send_reply(client);

            if (success) {
                s64 current_time = get_monotonic_time();

                { // |Cleanup: This logging bit in general.
                    Memory_context *ctx = client->context;
                    Request *req = &client->request;
                    char *method = req->method == GET ? "GET" : req->method == POST ? "POST" : "UNKNOWN!!";
                    char *path   = req->path.count ? req->path.data : "";
                    char *query  = req->query.count ? encode_query_string(&req->query, ctx)->data : "";
                    s64 ms = current_time - client->start_time;
                    printf("[%d] %s %s%s %ldms\n", client->response.status, method, path, query, ms);
                    fflush(stdout);
                }

                if (client->keep_alive) {
                    // Reset the client and prepare to receive more data on the socket.
                    reset_context(client->context);
                    init_client(client, client->context, client->socket, current_time);
                } else {
                    client->phase = READY_TO_CLOSE;
                }
            }
        }

        if (client->phase == READY_TO_CLOSE) {
            // Do nothing. The main thread will close the client socket and free memory.
        }

        // Write the client pointer to the worker pipe to let the server know we're done with it.
        {
            s64 num_bytes_written = write(server->worker_pipe[1], &client, sizeof(client));
            if (num_bytes_written == -1) {
                Fatal("We couldn't write to the worker pipe (%s).", get_last_error().string);
            } //|Todo: Repeat if we were interrupted before writing the full thing?
        }
    }

    return NULL;
}

Server *create_server(u32 address, u16 port, Memory_context *context)
{
    Server *server = New(Server, context);

    server->context = context;
    server->address = address;
    server->port    = port;

    server->socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server->socket < 0) {
        Fatal("Couldn't get a socket (%s).", get_last_error().string);
    }

    // Set SO_REUSEADDR because we want to run this program frequently during development.
    // Otherwise the kernel holds onto our address/port combo after our program finishes.
    int r = setsockopt(server->socket, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int));
    if (r < 0) {
        Fatal("Couldn't set socket options (%s).", get_last_error().string);
    }

    set_blocking(server->socket, false);

    struct sockaddr_in socket_addr = {
        .sin_family   = AF_INET,
        .sin_port     = htons(server->port),
        .sin_addr     = {htonl(server->address)},
    };

    r = bind(server->socket, (struct sockaddr const *)&socket_addr, sizeof(socket_addr));
    if (r < 0) {
        Fatal("Couldn't bind socket (%s).", get_last_error().string);
    }

    int QUEUE_LENGTH = 32;
    r = listen(server->socket, QUEUE_LENGTH);
    if (r < 0) {
        Fatal("Couldn't listen on socket (%s).", get_last_error().string);
    }

    printf("Listening on http://%d.%d.%d.%d:%d...\n", address>>24, address>>16&0xff, address>>8&0xff, address&0xff, port);

    // Create a file descriptor to handle SIGINT.
    sigset_t signal_mask;
    sigemptyset(&signal_mask);
    sigaddset(&signal_mask, SIGINT);

    r = pthread_sigmask(SIG_BLOCK, &signal_mask, NULL);
    if (r) {
        Fatal("Couldn't mask SIGINT (%s).", get_error_info(r).string);
    }

    server->interrupt_handle = signalfd(-1, &signal_mask, SFD_NONBLOCK);
    if (server->interrupt_handle == -1) {
        Fatal("Couldn't create a file descriptor to handle SIGINT (%s).", get_last_error().string);
    }

    server->routes = (Route_array){.context = context};

    server->clients = (Client_map){.context = context, .binary_mode = true};

    server->work_queue = create_queue(context);

    server->worker_threads = (pthread_t_array){.context = context};
    int NUM_WORKER_THREADS = 4; //|Todo: Make this configurable or find out how many processors the computer has.
    for (int i = 0; i < NUM_WORKER_THREADS; i++) {
        int r = pthread_create(Add(&server->worker_threads), NULL, worker_thread_routine, server);
        if (r) {
            Fatal("Thread creation failed (%s).", get_error_info(r).string);
        }
    }

    r = pipe(server->worker_pipe);
    if (r == -1) { //|Consistency: (== -1) or (< 0)?
        Fatal("Couldn't create a pipe (%s).", get_last_error().string);
    }

    return server;
}

void add_route(Server *server, enum HTTP_method method, char *path_pattern, Request_handler *handler)
{
    //|Todo: Check the server phase. You can't add routes once the server's started.

    Regex *regex = compile_regex(path_pattern, server->context);
    assert(regex);

    *Add(&server->routes) = (Route){method, regex, handler};
}

void start_server(Server *server)
{
    //
    // The pollfds array is both the array of file descriptors passed to poll() and the authoritative list
    // of the clients currently owned by the main thread. The main thread initialises a memory context for
    // new clients and then passes them to worker threads via the server.work_queue. Until a worker thread
    // sends the client pointer back to the server via the worker pipe, it owns (i.e. can modify) the
    // Client struct and the client's memory context. Separately the server maintains server.clients, a
    // hash table containing all open connections, keyed by the file descriptors of the open sockets. This
    // hash table is not threadsafe and should never be accessed by the worker threads.
    //
    Array(struct pollfd) pollfds = {.context = server->context};

    // The first three members of this array don't change. Note that we will iterate backwards when checking
    // poll events, so we put the SIGINT handle first because it rarely needs to be checked.
    *Add(&pollfds) = (struct pollfd){.fd = server->interrupt_handle, .events = POLLIN};
    *Add(&pollfds) = (struct pollfd){.fd = server->socket,           .events = POLLIN};
    *Add(&pollfds) = (struct pollfd){.fd = server->worker_pipe[0],   .events = POLLIN};

    s64 non_client_pollfds_count = pollfds.count; // Any other elements in this array are open client connections.

    bool server_should_stop = false;

    while (!server_should_stop || server->clients.count) // This is the server's main loop.
    {
        // If there are open connections, poll twice per second so we can keep checking whether
        // any connections have timed out. Otherwise wait indefinitely.
        int timeout_ms = (server->clients.count > 0) ? 500 : -1;

        int num_events = poll(pollfds.data, pollfds.count, timeout_ms);
        if (num_events < 0) {
            Fatal("poll failed (%s).", get_last_error().string);
        }

        s64 current_time = get_monotonic_time(); // We need to get this value after polling.

        for (s64 pollfd_index = pollfds.count-1; pollfd_index >= 0; pollfd_index -= 1) { // Iterate backwards so we can delete from the array.
            if (num_events == 0)  break;

            struct pollfd *pollfd = &pollfds.data[pollfd_index];

            if (!pollfd->revents)  continue;
            else  num_events -= 1;

            if (pollfd->revents & (POLLERR|POLLHUP|POLLNVAL)) {
                Client *client = *Get(&server->clients, pollfd->fd);
                close_and_delete_client(server, client);
                array_unordered_remove_by_index(&pollfds, pollfd_index);
                continue;
            }

            if (pollfd->fd == server->worker_pipe[0]) {
                // A worker thread is letting us know that they've finished with a client (for now).
                Client *client;
                s64 num_bytes_read = read(server->worker_pipe[0], &client, sizeof(client));
                if (num_bytes_read == -1) {
                    Fatal("We couldn't read from the worker pipe (%s).\n", get_last_error().string);
                } //|Todo: Make sure we read the whole pointer.
                assert(*Get(&server->clients, client->socket) == client);

                if (client->phase == READY_TO_CLOSE) {
                    close_and_delete_client(server, client);
                } else {
                    struct pollfd pollfd = {.fd = client->socket};

                    if (client->phase == PARSING_REQUEST)     pollfd.events |= POLLIN;
                    else if (client->phase == SENDING_REPLY)  pollfd.events |= POLLOUT;
                    else  assert(!"Unexpected request phase.");

                    *Add(&pollfds) = pollfd;
                }
                continue;
            }

            if (pollfd->fd == server->socket) {
                // A new connection has occurred.
                assert(server_should_stop == false);

                struct sockaddr_in client_socket_addr; // This will be initialised by accept().
                socklen_t client_socket_addr_size = sizeof(client_socket_addr);

                int client_socket = accept(server->socket, (struct sockaddr *)&client_socket_addr, &client_socket_addr_size);
                if (client_socket < 0) {
                    Fatal("poll() said we could read from our main socket, but we couldn't get a new connection (%s).", get_last_error().string);
                }

                set_blocking(client_socket, false);

                Client *client = New(Client, server->context);
                init_client(client, new_context(server->context), client_socket, current_time);

                assert(!IsSet(&server->clients, client_socket));
                *Set(&server->clients, client_socket) = client;
                add_to_queue(server->work_queue, client);
                continue;
            }

            if (pollfd->fd == server->interrupt_handle) {
                // We've received a SIGINT.
                struct signalfd_siginfo info; // |Cleanup: We don't do anything with this.
                read(server->interrupt_handle, &info, sizeof(info));

                server_should_stop = true;

                // In the future, don't poll the SIGINT file descriptor or the server's socket listening for new connections.
                assert(pollfds.data[0].fd == server->interrupt_handle);
                pollfds.data[0].events = 0;
                assert(pollfds.data[1].fd == server->socket);
                pollfds.data[1].events = 0;
                continue;
            }

            if (pollfd->revents & (POLLIN|POLLOUT)) {
                // We can read from or write to a client socket.
                Client *client = *Get(&server->clients, pollfd->fd);
                assert(client);
                add_to_queue(server->work_queue, client);
                array_unordered_remove_by_index(&pollfds, pollfd_index);
                continue; //|Cleanup: We continue in every case?
            }
        }

        // Remove connections that have expired.
        for (s64 pollfd_index = pollfds.count-1; pollfd_index >= non_client_pollfds_count; pollfd_index -= 1) {
            struct pollfd *pollfd = &pollfds.data[pollfd_index];
            assert(!pollfd->revents); // We should have removed any pollfds with returned events.

            Client *client = *Get(&server->clients, pollfd->fd);
            assert(client);

            s64 request_age = current_time - client->start_time;
            s64 max_age     = (server_should_stop) ? 1000 : 15000;

            if (request_age > max_age) {
                close_and_delete_client(server, client);
                array_unordered_remove_by_index(&pollfds, pollfd_index);
            }
        }
    }

    // Signal to the worker threads that it's time to wind up.
    for (s64 i = 0; i < server->worker_threads.count; i++)  add_to_queue(server->work_queue, NULL);

    // Join the worker threads.
    for (int i = 0; i < server->worker_threads.count; i++) {
        int r = pthread_join(server->worker_threads.data[i], NULL);
        if (r) {
            Fatal("Failed to join a thread (%s).", get_error_info(r).string);
        }
    }

    // Close the server's main socket. |Cleanup: Do this earlier?
    bool closed = !close(server->socket);
    if (!closed) {
        Fatal("We couldn't close our own socket (%s).", get_last_error().string);
    }
}

static Response serve_file_insecurely(Request *request, Memory_context *context) //|Deprecated
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
    if (!file) {
        char static body[] = "We couldn't find that file.\n";
        return (Response){404, .body=body, .size=lengthof(body)};
    }

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

    string_dict headers = {.context = context};
    if (content_type)  *Set(&headers, "content-type") = content_type;

    return (Response){200, headers, file->data, file->count};
}

int compare_strings(void const *a, void const *b)
// When using this with bsearch(), make sure that bsearch()'s first argument is a char**.
{
    return strcmp(*(char**)a, *(char**)b);
}
// We're creating these wrappers to avoid misusing the above. |Cleanup: Move to strings.h.
void sort_strings_alphabetically(char **strings, s64 num_strings)
{
    qsort(strings, num_strings, sizeof(char*), compare_strings);
}
char *search_alphabetically_sorted_strings(char *string, char **strings, s64 num_strings)
{
    char *match = bsearch(&string, strings, num_strings, sizeof(char*), compare_strings);
    return match;
}

typedef struct File_list_accessor File_list_accessor;
typedef struct File_list_resource File_list_resource;

struct File_list_accessor {
    Memory_context     *context;

    pthread_mutex_t     mutex_a;
    File_list_resource *resource;
};

struct File_list_resource {
    // A child context of the accessor's context, which contains everything to do with
    // this resource including the resource struct itself.
    Memory_context     *context;

    // mutex_b and num_refs work together as a semaphore.
    // We'd actually use a semaphore but we're doing this in C99.
    pthread_mutex_t     mutex_b;
    int                 num_refs;

    string_array        file_list;
    s64                 time_created;

    // Similarly, mutex_c and update_pending could just be an atomic variable.
    // A single thread, seeing that a resource has expired, sets update_pending = true
    // to signify that it is taking responsibility for updating the resource.
    pthread_mutex_t     mutex_c;
    bool                update_pending;
};

// |Temporary: We'll make this global for now, but it's meant to go on the server struct.
File_list_accessor file_list_accessor = {.mutex_a = PTHREAD_MUTEX_INITIALIZER};

static File_list_resource *create_file_list_resource(File_list_accessor *accessor)
// This function doesn't actually attach the resource to the accessor, but just uses its context.
//
{
    Memory_context *context = new_context(accessor->context);

    File_list_resource *resource = New(File_list_resource, context);

    resource->context = context;

    pthread_mutex_init(&resource->mutex_b, NULL);
    pthread_mutex_init(&resource->mutex_c, NULL);

    resource->file_list = (string_array){.context = context};

    recursively_add_file_names(NULL, 0, &resource->file_list);

    // |Speed: Do we get the file list already sorted making this the pathological case for qsort()?
    sort_strings_alphabetically(resource->file_list.data, resource->file_list.count);

    resource->time_created = get_monotonic_time();//|Todo: Maybe take this as an arg.

    resource->num_refs = 1;

    return resource;
}

Response serve_files(Request *request, Memory_context *context)
{
    File_list_accessor *accessor = &file_list_accessor;
    File_list_resource *resource = NULL;

    // When a thread wants to access a resource, it must lock accessor->mutex_a and increment resource->num_refs
    // (which also requires locking resource->mutex_b) before releasing mutex_a.
    pthread_mutex_lock(&accessor->mutex_a);
    {
        // Now that we've locked mutex_a, we know that the resource that the accessor points to is
        // valid and cannot be deleted while we hold mutex_a. This is true because:
        // - A resource will not be deallocated until its .num_refs reaches 0.
        // - Every resource has its .num_refs initialised to 1 before any accessor points to it.
        // - Every thread that accesses a resource increments .num_refs before decrementing it.
        // - The one exception to the above point is the routine responsible for updating an
        //   accessor's resource, which also locks mutex_a and changes the accessor's pointer before
        //   decrementing the old resource's .num_refs.
        resource = accessor->resource;

        // If the resource doesn't exist at all, create it. |Temporary: This will move to create_server() so we won't need this here.
        if (!resource) {
            Memory_context *server_context = context->parent; //|Hack.
            assert(!accessor->context);
            accessor->context = new_context(server_context);
            resource = create_file_list_resource(accessor);
            accessor->resource = resource;
        }

        // Increment the resource's .num_refs, so we can release mutex_a but keep our lease on the resource.
        pthread_mutex_lock(&resource->mutex_b);
        {
            resource->num_refs += 1;
        }
        pthread_mutex_unlock(&resource->mutex_b);
    }
    pthread_mutex_unlock(&accessor->mutex_a);

    // Check whether the resource has expired.
    s64 CACHE_TIMEOUT = 5000;
    s64 current_time = get_monotonic_time();
    bool expired = (current_time - resource->time_created) > CACHE_TIMEOUT;

    // Check whether a different thread is taking responsibility for updating the resource.
    bool we_should_update = false;
    if (expired) {
        int r = pthread_mutex_trylock(&resource->mutex_c);
        if (r == 0) {
            // We got the lock on mutex_c. If we're the first one here, it's up to us.
            if (!resource->update_pending)  we_should_update = true;
            resource->update_pending = true;
            pthread_mutex_unlock(&resource->mutex_c);
        } else if (r != EBUSY) {
            Fatal("Failed to try locking a mutex: %s", get_error_info(r).string);
        }
    }

    if (we_should_update) {
        // |Todo: Schedule this work to be done on a different thread so we can respond to the current request ASAP.
        Memory_context *server_context = context->parent; //|Hack.
        Memory_context *new_resource_context = new_context(server_context);

        File_list_resource *new_resource = create_file_list_resource(accessor);

        bool we_should_clean_up = false;

        pthread_mutex_lock(&accessor->mutex_a);
        pthread_mutex_lock(&resource->mutex_b);
        {
            accessor->resource = new_resource;  // This updates the accessor, but we'll keep using the old resource pointer on this thread, like we would if we had scheduled the update to happen asynchronously.
            resource->num_refs -= 1;

            we_should_clean_up = (resource->num_refs == 0);
        }
        pthread_mutex_unlock(&resource->mutex_b);
        pthread_mutex_unlock(&accessor->mutex_a);

        assert(we_should_clean_up == false); // Not possible for the time being, because this thread still holds a reference to the resource. But when we put this work on another thread, we might have to clean up here.
    }

    // Now we can use the resource and decrement .num_refs when we're done.

    char *requested_file = request->path.data;
    assert(requested_file[0] == '/');
    requested_file += 1;

    string_array *list = &resource->file_list;

    char *match = search_alphabetically_sorted_strings(requested_file, list->data, list->count);

    Response response = {0};
    if (match) {
        response = serve_file_insecurely(request, context);
    } else {
        char static body[] = "You seem to be requesting a file that's not on the list of files we can serve.\n";
        response = (Response){400, .body = body, .size = lengthof(body)};
    }

    // We've finished with the resource.

    bool we_should_clean_up = false;
    pthread_mutex_lock(&resource->mutex_b);
    {
        resource->num_refs -= 1;
        we_should_clean_up = (resource->num_refs == 0);
    }
    pthread_mutex_unlock(&resource->mutex_b);

    if (we_should_clean_up) {
        //|Temporary: We shouldn't think about cleaning up until after we've responded to the current request. Not 100% sure how to do it. Another async task?

        {
            // Just to make sure things look as we expect:
            Memory_context *server_context = context->parent; //|Hack
            assert(resource->context->parent == accessor->context);
            assert(accessor->context->parent == server_context);
        }

        pthread_mutex_destroy(&resource->mutex_b);
        pthread_mutex_destroy(&resource->mutex_c);

        free_context(resource->context);
    }

    return response;
}

Response serve_404(Request *request, Memory_context *context)
{
    char const static body[] = "Can't find it.\n";

    return (Response){
        .status  = 404,
        .body    = (u8 *)body,
        .size    = lengthof(body),
    };
}
