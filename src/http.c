//|Todo:
// Fix the issue where you can crash the server by holding ctrl+r in the browser.
// Stress test and benchmark.
// Clean up.
// Documentation of memory ownership. The pollfds array is the authoritative list of the clients currently owned by the main thread. The main thread initialises a memory context for new clients and then passes them to worker threads via the server.work_queue. Until a worker thread writes the client pointer to the worker pipe, it owns (i.e. can modify) the Client struct and the client's memory context. Separately the server maintains server.clients, a hash table containing all open connections, keyed by the file descriptors of the open sockets. This hash table is not threadsafe and should never be accessed by the worker threads.


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

    s32                     socket_no;      // The client socket's file descriptor.
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

    char_array              reply_header;   // Our response's header in raw text form.
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

static Client_queue *create_queue(Memory_context *context)
{
    Client_queue *queue = NewArray(queue, context);

    pthread_mutex_init(&queue->mutex, NULL);
    pthread_cond_init(&queue->ready, NULL);

    array_reserve(queue, 8);
    queue->head = queue->data;

    return queue;
}

static u8 hex_to_byte(char c1, char c2)
// Turn two hexadecimal digits into a byte of data. E.g. hex_to_byte('8', '0') -> 128 (0x80).
{
    // Assume the caller has already validated the characters with isxdigit().
    assert(isxdigit(c1) && isxdigit(c2));

    c1 |= 0x20; // OR-ing with 0x20 makes ASCII letters lowercase and doesn't affect ASCII numbers.
    c2 |= 0x20;

    u8 x1 = c1 <= '9' ? c1-'0' : c1-'a'+10;
    u8 x2 = c2 <= '9' ? c2-'0' : c2-'a'+10;

    return (u8)((x1 << 4) | x2);
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
            client->response = (Response){413, .body = "The request is too large.\n"};
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
        client->response = (Response){501, .body = "We only support GET requests!\n"};
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
        client->response = (Response){505, .body = "Unsupported HTTP version.\n"};
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

static void init_client(Client *client, Memory_context *context, s32 socket_no, s64 start_time)
{
    *client                   = (Client){0};

    client->context           = context;
    client->start_time        = start_time;
    client->socket_no         = socket_no;
    client->phase             = PARSING_REQUEST;

    client->message           = (char_array){.context = context};
    client->crlf_offsets      = (s16_array){.context = context};

    client->request.path      = (char_array){.context = context};
    client->request.query     = (string_dict){.context = context};
    client->request.captures  = (Captures){.context = context}; //|Cleanup: Once we change the match_regex() signature, we won't need to initialise .captures here.

    client->response.headers  = (string_dict){.context = context};

    client->reply_header      = (char_array){.context = context};
}

static void deal_with_a_request(Server *server, Client *client)
// The main work that a worker thread does.
{
    if (client->phase == PARSING_REQUEST) {
        char_array *message = &client->message;
        if (message->limit == 0) {
            s64 INITIAL_RECV_BUFFER_SIZE = 2048;
            array_reserve(message, INITIAL_RECV_BUFFER_SIZE);
        }

        bool we_should_try_to_parse = false;

        // Try to read from the client socket.
        while (true) {
            char *buffer = &message->data[message->count];
            s64 num_free_bytes = message->limit - message->count - 1; // Reserve space for a null byte.
            if (num_free_bytes <= 0) {
                array_reserve(message, round_up_pow2(message->limit+1));
                continue;
            }

            int flags = 0;
            s64 recv_count = recv(client->socket_no, buffer, num_free_bytes, flags);
            if (recv_count < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // There's nothing more to read now.
                    break;
                } else {
                    // There was an actual error. |Todo: Don't make this a fatal error.
                    Fatal("We failed to read from a socket (%s).", get_last_error().string);
                }
            } else if (recv_count == 0) {
                // The client has disconnected.
                we_should_try_to_parse = false;
                client->phase = READY_TO_CLOSE;
                break;
            } else {
                // We have successfully received some bytes.
                message->count += recv_count;
                assert(message->count < message->limit);
                message->data[message->count] = '\0';
                we_should_try_to_parse = true;
            }
        }

        // Try to parse the request.
        if (we_should_try_to_parse)  parse_request(client); //|Cleanup: We don't use the return value now, so maybe change the parse_request() function signature.
    }

    if (client->phase == HANDLING_REQUEST) {
        Request *request = &client->request;

        // Find a matching handler.
        Request_handler *handler = NULL;
        for (s64 i = 0; i < server->routes.count; i++) {
            Route *route = &server->routes.data[i];
            if (route->method != request->method)  continue;

            bool match = match_regex(request->path.data, request->path.count, route->path_regex, &request->captures);
            if (match) {
                handler = route->handler;
                break;
            }
        }
        if (!handler) {
            // We couldn't find a handler for this request, so return a 404.
            handler = &serve_404;
        }

        // Run the handler.
        Response *response = &client->response;
        *response = (*handler)(request, client->context);

        // If the response has a body but no size, calculate the size assuming the body is a zero-terminated string. |Silly
        if (response->body && !response->size)  response->size = strlen(response->body);

        // Print the headers into a buffer.
        {
            char_array *reply_header = &client->reply_header;
            array_reserve(reply_header, 128);

            char *version;
            if (client->http_version == HTTP_VERSION_1_0)       version = "HTTP/1.0";
            else if (client->http_version == HTTP_VERSION_1_1)  version = "HTTP/1.1";
            else  assert(!"Unexpected HTTP version.");

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

        client->phase = SENDING_REPLY;
    }

    if (client->phase == SENDING_REPLY) {
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
            s64 send_count = send(client->socket_no, data_to_send, num_bytes_to_send, flags);
            if (send_count < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK)  break;
                else  Fatal("send failed (%s).", get_last_error().string); //|Todo: Make this non-fatal.
            }
            assert(send_count > 0);

            *num_bytes_sent += send_count;
        }

        if (*num_bytes_sent < full_reply_size) {
            // We've partially sent our reply.
        } else {
            // We've fully sent our reply. Success!
            assert(*num_bytes_sent == full_reply_size);

            { // |Cleanup: This logging bit in general.
                Memory_context *ctx = client->context;
                Request *req = &client->request;
                char *method = req->method == GET ? "GET" : req->method == POST ? "POST" : "UNKNOWN!!";
                char *path   = req->path.count ? req->path.data : "";
                char *query  = req->query.count ? encode_query_string(&req->query, ctx)->data : "";
                printf("[%d] %s %s%s\n", client->response.status, method, path, query);
                fflush(stdout);
            }

            if (client->keep_alive) {
                // Reset the client and prepare to receive more data on the socket.
                reset_context(client->context);
                init_client(client, client->context, client->socket_no, get_monotonic_time());
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
        s64 num_bytes_written = write(server->worker_pipe_nos[1], &client, sizeof(client));
        if (num_bytes_written == -1) {
            Fatal("We couldn't write to the worker pipe (%s).", get_last_error().string);
        } //|Todo: Repeat if we were interrupted before writing the full thing?
    }
}

static void *thread_start(void *arg)
// The worker thread's main loop.
{
    Server *server = arg;
    Client_queue *queue = server->work_queue;

    while (true)
    {
        Client *client = pop_queue(queue);
        if (!client)  break; // A task without a client means the thread should stop. |Temporary: I'm not sure the best way to do this.

        deal_with_a_request(server, client); //|Cleanup: This function is tiny now.
    }

    return NULL;
}

Server *create_server(u32 address, u16 port, bool verbose, Memory_context *context)
{
    Server *server = New(Server, context);

    server->context = context;
    server->address = address;
    server->port    = port;
    server->verbose = verbose;

    server->socket_no = socket(AF_INET, SOCK_STREAM, 0);
    if (server->socket_no < 0) {
        Fatal("Couldn't get a socket (%s).", get_last_error().string);
    }

    // Set SO_REUSEADDR because we want to run this program frequently during development.
    // Otherwise the kernel holds onto our address/port combo after our program finishes.
    int r = setsockopt(server->socket_no, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int));
    if (r < 0) {
        Fatal("Couldn't set socket options (%s).", get_last_error().string);
    }

    set_blocking(server->socket_no, false);

    struct sockaddr_in socket_addr = {
        .sin_family   = AF_INET,
        .sin_port     = htons(server->port),
        .sin_addr     = {htonl(server->address)},
    };

    r = bind(server->socket_no, (struct sockaddr const *)&socket_addr, sizeof(socket_addr));
    if (r < 0) {
        Fatal("Couldn't bind socket (%s).", get_last_error().string);
    }

    int QUEUE_LENGTH = 32;
    r = listen(server->socket_no, QUEUE_LENGTH);
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

    server->sigint_file_no = signalfd(-1, &signal_mask, SFD_NONBLOCK);
    if (server->sigint_file_no == -1) {
        Fatal("Couldn't create a file descriptor to handle SIGINT (%s).", get_last_error().string);
    }

    server->routes = (Route_array){.context = context};

    server->clients = (Client_map){.context = context, .binary_mode = true};

    server->work_queue = create_queue(context);

    server->worker_threads = (pthread_t_array){.context = context};
    int NUM_WORKER_THREADS = 2; //|Todo: Put this somewhere else.
    for (int i = 0; i < NUM_WORKER_THREADS; i++) {
        int r = pthread_create(Add(&server->worker_threads), NULL, thread_start, server);
        if (r) {
            Fatal("Thread creation failed (%s).", get_error_info(r).string);
        }
    }

    r = pipe(server->worker_pipe_nos);
    if (r == -1) { //|Consistency: (== -1) or (< 0)?
        Fatal("Couldn't create a pipe (%s).", get_last_error().string);
    }

    return server;
}

static void close_and_delete_client(Server *server, Client *client)
{
    int r = close(client->socket_no);
    if (r == -1) {
        Fatal("We couldn't close a client socket (%s).", get_last_error().string);
    }

    Delete(&server->clients, client->socket_no);
    free_context(client->context);
    dealloc(client, server->context);
}

void start_server(Server *server)
{
    // An array of file descriptors to poll.
    Array(struct pollfd) pollfds = {.context = server->context};
    // The file descriptor that lets us know when we've received a SIGINT.
    *Add(&pollfds) = (struct pollfd){.fd = server->sigint_file_no, .events = POLLIN};
    // The server's main socket for new connections.
    *Add(&pollfds) = (struct pollfd){.fd = server->socket_no, .events = POLLIN};
    // The pipe that the worker threads use to communicate with us.
    *Add(&pollfds) = (struct pollfd){.fd = server->worker_pipe_nos[0], .events = POLLIN};
    // Note that we will iterate backwards when checking the returned events, and we'll break once we've processed all the events.
    // So we put the SIGINT file descriptor first because it rarely needs to be checked.
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
                if (server->verbose) {
                    if (pollfd->revents & POLLERR)   printf("POLLERR on socket %d.\n", pollfd->fd);
                    if (pollfd->revents & POLLHUP)   printf("POLLHUP on socket %d.\n", pollfd->fd);
                    if (pollfd->revents & POLLNVAL)  printf("POLLNVAL on socket %d.\n", pollfd->fd);
                }

                Client *client = *Get(&server->clients, pollfd->fd);
                close_and_delete_client(server, client);
                array_unordered_remove_by_index(&pollfds, pollfd_index);
                continue;
            }

            if (pollfd->fd == server->worker_pipe_nos[0]) {
                // A worker thread is letting us know that they've finished with a client (for now).
                Client *client;
                s64 num_bytes_read = read(server->worker_pipe_nos[0], &client, sizeof(client));
                if (num_bytes_read == -1) {
                    Fatal("We couldn't read from the worker pipe (%s).\n", get_last_error().string);
                } //|Todo: Make sure we read the whole pointer.
                assert(*Get(&server->clients, client->socket_no) == client);

                if (client->phase == READY_TO_CLOSE) {
                    close_and_delete_client(server, client);
                } else {
                    struct pollfd pollfd = {.fd = client->socket_no};

                    if (client->phase == PARSING_REQUEST)     pollfd.events |= POLLIN;
                    else if (client->phase == SENDING_REPLY)  pollfd.events |= POLLOUT;
                    else  assert(!"Unexpected request phase.");

                    *Add(&pollfds) = pollfd;
                }
                continue;
            }

            if (pollfd->fd == server->socket_no) {
                // A new connection has occurred.
                assert(server_should_stop == false);

                struct sockaddr_in client_socket_addr; // This will be initialised by accept().
                socklen_t client_socket_addr_size = sizeof(client_socket_addr);

                int client_socket_no = accept(server->socket_no, (struct sockaddr *)&client_socket_addr, &client_socket_addr_size);
                if (client_socket_no < 0) {
                    Fatal("poll() said we could read from our main socket, but we couldn't get a new connection (%s).", get_last_error().string);
                }

                set_blocking(client_socket_no, false);

                Client *client = New(Client, server->context);
                init_client(client, new_context(server->context), client_socket_no, current_time);

                assert(!IsSet(&server->clients, client_socket_no));
                *Set(&server->clients, client_socket_no) = client;
                add_to_queue(server->work_queue, client);
                continue;
            }

            if (pollfd->fd == server->sigint_file_no) {
                // We've received a SIGINT.
                struct signalfd_siginfo info; // |Cleanup: We don't do anything with this.
                read(server->sigint_file_no, &info, sizeof(info));

                server_should_stop = true;

                // In the future, don't poll the SIGINT file descriptor or the server's socket listening for new connections.
                assert(pollfds.data[0].fd == server->sigint_file_no);
                pollfds.data[0].events = 0;
                assert(pollfds.data[1].fd == server->socket_no);
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
    bool closed = !close(server->socket_no);
    if (!closed) {
        Fatal("We couldn't close our own socket (%s).", get_last_error().string);
    }
}

void add_route(Server *server, enum HTTP_method method, char *path_pattern, Request_handler *handler)
{
    //|Todo: Check the server phase. You can't add routes once the server's started.

    Regex *regex = compile_regex(path_pattern, server->context);
    assert(regex);

    *Add(&server->routes) = (Route){method, regex, handler};
}

Response serve_file_insecurely(Request *request, Memory_context *context)
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
        return (Response){404, .body = "We couldn't find that file.\n"};
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

Response serve_404(Request *request, Memory_context *context)
{
    char const static body[] = "Can't find it.\n";

    return (Response){
        .status  = 404,
        .headers = NULL,
        .body    = (u8 *)body,
        .size    = lengthof(body),
    };
}
