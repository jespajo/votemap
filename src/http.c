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

typedef struct Task Task;

struct Task {
    enum {
        DEAL_WITH_A_CLIENT=1,
        REFRESH_FILE_TREE,
        TIME_TO_WIND_UP,
    }                       type;
    union {
        // If the type is DEAL_WITH_A_CLIENT:
        Client             *client;

        // If the type is REFRESH_FILE_TREE:
        File_tree_accessor *file_tree_accessor;
    };
};

struct Task_queue {
    pthread_mutex_t         mutex;
    pthread_cond_t          ready;          // The server will broadcast when there are tasks.

    Array(Task);
    Task                   *head;           // A pointer to the first element in the array that hasn't been taken from the queue. If it points to the element after the end of the array, the queue is empty.
};

static Task_queue *create_queue(Memory_context *context)
{
    Task_queue *queue = NewArray(queue, context);

    pthread_mutex_init(&queue->mutex, NULL);
    pthread_cond_init(&queue->ready, NULL);

    array_reserve(queue, 8);
    queue->head = queue->data;

    return queue;
}

static void add_to_queue(Task_queue *queue, Task task)
{
    Task_queue *q = queue;

    pthread_mutex_lock(&q->mutex);

    s64 head_index = q->head - q->data;
    assert(0 <= head_index && head_index <= q->count);

    bool queue_was_empty = (head_index == q->count);

    if (q->count == q->limit && head_index > 0) {
        // We're out of room in the array, but there's space to the left of the head. Shift the head back to the start of the array.
        memmove(q->data, q->head, (q->count - head_index)*sizeof(q->data[0]));
        q->count  -= head_index;
        head_index = 0;
    }

    *Add(q) = task;
    q->head = &q->data[head_index];

    if (queue_was_empty)  pthread_cond_broadcast(&q->ready);
    pthread_mutex_unlock(&q->mutex);
}

static Task pop_queue(Task_queue *queue)
{
    Task_queue *q = queue;

    pthread_mutex_lock(&q->mutex);
    while (q->head == &q->data[q->count])  pthread_cond_wait(&q->ready, &q->mutex);

    Task task = *q->head;
    q->head += 1;

    pthread_mutex_unlock(&q->mutex);
    return task;
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
        string_dict *query = &client->request.query_params;

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
// Also save pointers to the route and the result of the matching regex on the client.
{
    assert(client->phase == HANDLING_REQUEST);

    Request *request = &client->request;

    for (s64 i = 0; i < server->routes.count; i++) {
        Route *route = &server->routes.data[i];

        if (route->method != request->method)  continue;

        Match *match = run_regex(route->path_regex, request->path.data, request->path.count, client->context);
        if (match->success) {
            client->route       = route;
            client->route_match = match;

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

static void init_client(Server *server, Client *client, Memory_context *context, s32 socket, s64 start_time)
{
    *client                   = (Client){0};

    client->server            = server;
    client->context           = context;
    client->start_time        = start_time;
    client->socket            = socket;
    client->phase             = PARSING_REQUEST;

    client->message           = (char_array){.context = context};
    client->crlf_offsets      = (s16_array){.context = context};

    client->request.path      = (char_array){.context = context};
    client->request.query_params = (string_dict){.context = context};

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

void refresh_file_tree(File_tree_accessor *accessor); //|Temporary: Until we put the File_tree_accessor stuff into its own module.

static void *worker_thread_routine(void *arg)
// The worker thread's main loop.
{
    Server *server = arg;
    Task_queue *queue = server->task_queue;

    while (true)
    {
        Task task = pop_queue(queue);

        if (task.type == TIME_TO_WIND_UP)  break;

        if (task.type == REFRESH_FILE_TREE) {
            refresh_file_tree(task.file_tree_accessor);
            continue;
        }

        assert(task.type == DEAL_WITH_A_CLIENT); //|Cleanup: From here to the end of the loop should be its own function.
        Client *client = task.client;

        if (client->phase == PARSING_REQUEST) {
            bool received = receive_message(client);

            if (received)  parse_request(client); //|Cleanup: We don't use the return value now, so maybe change the parse_request() function signature.
        }

        if (client->phase == HANDLING_REQUEST) {
            Request_handler *handler = find_request_handler(server, client);
            if (!handler)  handler = &serve_404;

            // Run the handler.
            client->response = (*handler)(client);
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
                    char *query  = req->query_params.count ? encode_query_string(&req->query_params, ctx)->data : "";
                    s64 ms = current_time - client->start_time; //|Fixme: The fact that we use the client->start_time here results in inaccurate logging about how long requests take, because browsers leave connections open for a long time in between requests. Instead we should be using the time when we received the first byte of the request.
                    printf("[%d] %s %s%s %ldms\n", client->response.status, method, path, query, ms);
                    fflush(stdout);
                }

                if (client->keep_alive) {
                    // Reset the client and prepare to receive more data on the socket.
                    reset_context(client->context);
                    init_client(server, client, client->context, client->socket, current_time);
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

    server->task_queue = create_queue(context);

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

#ifndef FILE_TREE_STUFF_WHICH_WE_WILL_PROBABLY_PUT_INTO_ITS_OWN_MODULE
//typedef struct File_tree_accessor File_tree_accessor;
typedef struct File_tree_resource File_tree_resource;

struct File_tree_accessor {
    Memory_context     *context;

    char               *directory;

    pthread_mutex_t     mutex;
    File_tree_resource *resource;
};

struct File_tree_resource {
    // A child context of the accessor's context. It contains everything to do with
    // this resource including the resource struct itself.
    Memory_context     *context;

    struct {
        pthread_mutex_t     mutex;
        int                 value;
    }                   num_refs;

    File_node          *file_tree;
    s64                 time_created;

    // The first thread to notice that a resource has expired sets update_pending = true
    // to let other threads know that it is taking responsibility for the update.
    struct {
        pthread_mutex_t     mutex;
        bool                value;
    }                   update_pending;
};

File_tree_resource *acquire_file_tree(File_tree_accessor *accessor)
{
    File_tree_resource *resource = NULL;

    pthread_mutex_lock(&accessor->mutex);
    {
        // Now that we've locked the accessor's mutex, we know that the resource the accessor points
        // to is valid and cannot be deleted while we hold the mutex. This is true because:
        // - A resource will not be deallocated until its .num_refs reaches 0.
        // - Every resource has its .num_refs initialised to 1 before any accessor points to it.
        // - Every thread that accesses a resource increments .num_refs before decrementing it.
        // - The one exception to the above point is the routine responsible for updating an
        //   accessor's resource, which also locks the accessor's mutex and changes the accessor's
        //   pointer before decrementing the old resource's .num_refs.
        resource = accessor->resource;

        // Increment .num_refs so the resource stays valid after we unlock the accessor's mutex.
        pthread_mutex_lock(&resource->num_refs.mutex);
        {
            resource->num_refs.value += 1;
        }
        pthread_mutex_unlock(&resource->num_refs.mutex);
    }
    pthread_mutex_unlock(&accessor->mutex);

    return resource;
}

void free_file_tree_resource(File_tree_resource *resource)
{
    assert(resource->num_refs.value == 0);

    pthread_mutex_destroy(&resource->num_refs.mutex);
    pthread_mutex_destroy(&resource->update_pending.mutex);

    free_context(resource->context);
}

bool release_file_tree(File_tree_resource *resource)
// Return true if we were the last one to release the resource and hence must clean it up.
{
    int num_refs;
    pthread_mutex_lock(&resource->num_refs.mutex);
    {
        resource->num_refs.value -= 1;
        num_refs = resource->num_refs.value;
    }
    pthread_mutex_unlock(&resource->num_refs.mutex);

    return (num_refs == 0);
}

void refresh_file_tree(File_tree_accessor *accessor)
// Replace the current resource on the accessor. Clean up the old resource if no-one else has a reference to it.
{
    Memory_context *context = new_context(accessor->context);

    File_tree_resource *resource = New(File_tree_resource, context);
    resource->context = context;
    pthread_mutex_init(&resource->update_pending.mutex, NULL);

    resource->file_tree = get_file_tree(accessor->directory, context);

    resource->time_created = get_monotonic_time();//|Todo: Maybe take this as an arg.
    pthread_mutex_init(&resource->num_refs.mutex, NULL);
    resource->num_refs.value = 1;

    // Put the resource on the accessor.
    File_tree_resource *old_resource;
    pthread_mutex_lock(&accessor->mutex);
    {
        old_resource = accessor->resource;
        accessor->resource = resource;
    }
    pthread_mutex_unlock(&accessor->mutex);

    if (!old_resource)  return; // This lets us also use this function to create a resource for the first time, assuming the accessor has already been initialised except for its resource member, which should be NULL.

    bool should_clean_up = release_file_tree(old_resource);

    if (should_clean_up)  free_file_tree_resource(old_resource);
}

File_tree_accessor *create_file_tree_accessor(char *directory, Memory_context *context)
{
    Memory_context *sub_context = new_context(context); // |Memory

    File_tree_accessor *accessor = New(File_tree_accessor, sub_context);
    accessor->context = sub_context;
    pthread_mutex_init(&accessor->mutex, NULL);

    // Copy the directory path, sans the trailing slash if there is one.
    {
        int length = strlen(directory);
        if (directory[length-1] == '/')  length -= 1;

        accessor->directory = copy_string(directory, length, context).data;
    }

    refresh_file_tree(accessor);

    return accessor;
}
#endif

void add_file_route(Server *server, char *path_pattern, char *directory)
// Add a route to serve files under a given directory.
//
// We might want to make this more flexible later: directory could be a single file (which, if it won't change, we could just keep in memory for the program's lifetime!---that will mean extending the concept of a "shared resource"). Also, the path_pattern could be like "files/(?<file_name>.*)"---that is, have a special capture group that gets taken as the file path rather than the full request path.
{
    Regex *regex = compile_regex(path_pattern, server->context);
    assert(regex);

    Route route = {GET, regex, &serve_files};

    route.file_tree_accessor = create_file_tree_accessor(directory, server->context);

    //|Robustness: Assert the server hasn't started.
    *Add(&server->routes) = route;
}

void start_server(Server *server)
{
    //
    // The pollfds array is both the array of file descriptors passed to poll() and the authoritative list
    // of the clients currently owned by the main thread. The main thread initialises a memory context for
    // new clients and then passes them to worker threads via the server.task_queue. Until a worker thread
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
                init_client(server, client, new_context(server->context), client_socket, current_time);

                assert(!IsSet(&server->clients, client_socket));
                *Set(&server->clients, client_socket) = client;
                add_to_queue(server->task_queue, (Task){DEAL_WITH_A_CLIENT, .client=client});
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
                add_to_queue(server->task_queue, (Task){DEAL_WITH_A_CLIENT, .client=client});
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
    for (s64 i = 0; i < server->worker_threads.count; i++)  add_to_queue(server->task_queue, (Task){TIME_TO_WIND_UP});

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

Response create_index_page(File_node *file_node, Memory_context *context)
{
    assert(file_node->type == DIRECTORY);

    char_array doc = get_string(context, "<!DOCTYPE HTML>\n");
    append_string(&doc, "<html>\n");

    append_string(&doc, "<head>\n");
    append_string(&doc, "<title>%s</title>\n", file_node->name);
    append_string(&doc, "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"/>\n");
    append_string(&doc, "</head>\n");

    append_string(&doc, "<body>\n");
    append_string(&doc, "<p><a href=\"../\">[Go up a directory.]</a></p>\n");
    append_string(&doc, "<h1>%s</h1>\n", file_node->name);

    //
    // Make two passes over the child nodes to print the directories first.
    //
    for (s64 i = 0; i < file_node->children.count; i++) {
        File_node *child = &file_node->children.data[i];
        if (child->type == DIRECTORY) {
            append_string(&doc, "<p><a href=\"%s/\">%s/</a></p>\n", child->name, child->name);
        }
    }
    for (s64 i = 0; i < file_node->children.count; i++) {
        File_node *child = &file_node->children.data[i];
        if (child->type != DIRECTORY) {
            append_string(&doc, "<p><a href=\"%s\">%s</a></p>\n", child->name, child->name);
        }
    }
    append_string(&doc, "</body>\n");
    append_string(&doc, "</html>\n");

    string_dict headers = (string_dict){.context = context};
    *Set(&headers, "content-type") = "text/html";

    return (Response){200, .headers=headers, .body=doc.data, .size=doc.count};
}

Response serve_files(Client *client)
{
    File_tree_accessor *accessor = client->route->file_tree_accessor;
    File_tree_resource *resource = acquire_file_tree(accessor);

    // Check whether the resource has expired. If the file tree is older than CACHE_TIMEOUT milliseconds,
    // we'll schedule it to be created again, though we'll still use the old one for the current request.
    s64 CACHE_TIMEOUT = 1000;
    s64 current_time = get_monotonic_time();
    bool expired = (current_time - resource->time_created) > CACHE_TIMEOUT;

    // Check whether a different thread is taking responsibility for updating the resource.
    bool we_should_update = false;
    if (expired) {
        int r = pthread_mutex_trylock(&resource->update_pending.mutex);
        if (r == 0) {
            // We got the lock on .update_pending. If we're the first one here, it's up to us.
            we_should_update = (resource->update_pending.value == false);
            resource->update_pending.value = true;
            pthread_mutex_unlock(&resource->update_pending.mutex);
        } else if (r == EBUSY) {
            // Do nothing. The only thing we ever do with .update_pending is set it once,
            // so if someone else has the lock, we know it's set.
        } else {
            Fatal("Failed to trylock a mutex: %s", get_error_info(r).string);
        }
    }

    if (we_should_update) {
        Task_queue *task_queue = client->server->task_queue;
        add_to_queue(task_queue, (Task){REFRESH_FILE_TREE, .file_tree_accessor=accessor});
    }

    Memory_context *context = client->context;
    Request        *request = &client->request;

    Response response = {0};

    File_node *file_node = find_file_node(&request->path.data[1], resource->file_tree);

    if (!file_node) {
        char static body[] = "That file isn't on our list.\n";
        response = (Response){404, .body=body, .size=lengthof(body)};
        goto done;
    }

    if (file_node->type == DIRECTORY) {
        File_node *index = find_file_node("index.html", file_node);
        if (index) {
            file_node = index;
        } else if (request->path.data[request->path.count-1] == '/') {
            // There is no index.html in this directory. Create an index page dynamically.
            response = create_index_page(file_node, context);
            goto done;
        } else {
            //
            // The request path doesn't end with a slash. Force the client to send it again with a slash.
            // This is mean, but it makes it easier for us to create index pages dynamically, because if
            // a web page's URL ends with a slash, browsers treat links on the page as relative to the
            // page itself. So this means we can link to the files in this directory just by their name.
            //
            char static body[] = "This page has moved permanently.\n";
            response = (Response){301, .body=body, .size=lengthof(body)};
            response.headers = (string_dict){.context = context};
            *Set(&response.headers, "location") = get_string(context, "%s/", request->path.data).data;
            goto done;
        }
    }

    if (file_node->type != REGULAR_FILE) {
        char static body[] = "We can't serve that type of file.\n";
        response = (Response){403, .body=body, .size=lengthof(body)};
        goto done;
    }

    u8_array *file = load_binary_file(file_node->path.data, context);
    if (!file) {
        char static body[] = "That file is on our list, yet it doesn't exist.\n";
        response = (Response){500, .body=body, .size=lengthof(body)};
        goto done;
    }

    response = (Response){200, .body=file->data, .size=file->count};

    char *content_type = NULL;
    for (s64 i = file_node->path.count-1; i >= 0; i--) {
        if (file_node->path.data[i] == '/')  break;
        if (file_node->path.data[i] != '.')  continue;

        char *file_extension = &file_node->path.data[i+1];

        if (!strcmp(file_extension, "html"))       content_type = "text/html";
        else if (!strcmp(file_extension, "js"))    content_type = "text/javascript";
        else if (!strcmp(file_extension, "json"))  content_type = "application/json";
        else if (!strcmp(file_extension, "ttf"))   content_type = "font/ttf";

        break;
    }
    if (content_type) {
        response.headers = (string_dict){.context = context};
        *Set(&response.headers, "content-type") = content_type;
    }

done:;
    bool should_clean_up = release_file_tree(resource);

    if (should_clean_up) {
        // Clean up the old resource. We could create a task to schedule this work on a different thread
        // rather than making the current request wait, but there's no need because cleaning up is fast.
        free_file_tree_resource(resource);
    }

    return response;
}

Response serve_404(Client *client)
{
    char const static body[] = "Can't find it.\n";

    return (Response){
        .status  = 404,
        .body    = (u8 *)body,
        .size    = lengthof(body),
    };
}
