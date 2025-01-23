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

    enum Request_phase {
        PARSING_REQUEST=1,
        HANDLING_REQUEST,
        SENDING_REPLY,
        READY_TO_CLOSE,
        CLOSED,
    }                       phase;

    char_array              message;        // A buffer for storing bytes received.
    Request                 request;

    Response                response;

    char_array              reply_header;   // Our response's header in raw text form.
    s64                     num_bytes_sent; // The total number of bytes we've sent of our response. Includes both header and body.
};

struct HTTP_task {
    HTTP_task              *next;
    Client                 *client;
};

struct HTTP_queue {
    pthread_mutex_t         mutex;
    pthread_cond_t          ready;
    bool                    blocks_when_empty; // If true, the queue uses the condition variable.

    Memory_context         *context;

    HTTP_task              *head;
    HTTP_task              *tail;
};

static void add_to_queue(HTTP_queue *queue, Client *client)
{
    HTTP_task *task = New(HTTP_task, queue->context);
    task->client = client;

    pthread_mutex_lock(&queue->mutex);
    if (queue->head) {
        queue->tail->next = task;
        queue->tail = task;
    } else {
        queue->head = task;
        queue->tail = task;

        if (queue->blocks_when_empty)  pthread_cond_broadcast(&queue->ready);
    }
    pthread_mutex_unlock(&queue->mutex);
}

static Client *pop_queue(HTTP_queue *queue)
{
    pthread_mutex_lock(&queue->mutex);
    if (!queue->head) {
        if (!queue->blocks_when_empty) {
            pthread_mutex_unlock(&queue->mutex);
            return NULL;
        }
        do {pthread_cond_wait(&queue->ready, &queue->mutex);}  while (!queue->head);
    }

    HTTP_task *task = queue->head;
    queue->head = task->next;
    if (!task->next)  queue->tail = NULL;

    pthread_mutex_unlock(&queue->mutex);

    Client *client = task->client; // Copy to our stack before deallocating.
    dealloc(task, queue->context);
    return client;
}

HTTP_queue *create_queue(Memory_context *context, bool block_when_empty)
{
    // Always create a child context for the queue so allocations don't block other contexts.
    Memory_context *ctx = new_context(context);

    HTTP_queue *queue = New(HTTP_queue, ctx);
    queue->context = ctx;

    pthread_mutex_init(&queue->mutex, NULL);
    if (block_when_empty) {
        pthread_cond_init(&queue->ready, NULL);
        queue->blocks_when_empty = true;
    }

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

    if (size < 4)  return false;

    client->request = (Request){0};

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

        string_dict *query = &client->request.query;
        *query = (string_dict){.context = ctx};

        char_array key   = {.context = ctx};
        char_array value = {.context = ctx};

        // At first we're reading the request path. If we come to a query string, our target alternates between a pending key and value.
        char_array *target = path;

        while (d-data < size) {
            if (isalnum(*d) || Contains(ALLOWED, *d)) {
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
                append_string(&message, "The request had an unexpected character at index %ld: ", d-data);
                append_string(&message, isalnum(*d) ? "'%c'.\n" : "\\x%02x.\n", *d);

                client->response = (Response){400, .body = message.data, message.count};
                client->phase = SENDING_REPLY;
                return true;
            }
        }

        *Add(path) = '\0';
        path->count -= 1;
    }

    if (!starts_with(d, " HTTP/")) {
        client->response = (Response){400, .body = "There was an unexpected character in the HTTP version.\n"};
        client->phase = SENDING_REPLY;     // Fail.
    } else {
        client->phase = HANDLING_REQUEST;  // Success.
    }

    return true;
}

static char_array *encode_query_string(string_dict *query, Memory_context *context)
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
            if (isalnum(c) || Contains(ALLOWED, c))  *Add(result) = c;
            else  append_string(result, "%%%02x", c);
        }

        char *val = query->vals[i];
        s64 val_len = strlen(val);

        if (!val_len)  continue;

        *Add(result) = '=';

        for (s64 j = 0; j < val_len; j++) {
            char c = val[j];
            if (isalnum(c) || Contains(ALLOWED, c))  *Add(result) = c;
            else  append_string(result, "%%%02x", c);
        }
    }

    *Add(result) = '\0';
    result->count -= 1;

    return result;
}

static void deal_with_a_request(Server *server, Client *client)
// The main work that a worker thread does.
{
    if (client->phase == PARSING_REQUEST) {
        char_array *message = &client->message;
        if (message->limit == 0) {
            // Initialise the recv buffer.
            *message = (char_array){.context = client->context};
            s64 INITIAL_RECV_BUFFER_SIZE = 2048;
            array_reserve(message, INITIAL_RECV_BUFFER_SIZE);
        }

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
                client->phase = READY_TO_CLOSE;
                break;
            } else {
                // We have successfully received some bytes.
                message->count += recv_count;
                assert(message->count < message->limit);
                message->data[message->count] = '\0';
            }
        }

        // Try to parse the request.
        if (message->count > 0)  parse_request(client); //|Cleanup: We don't use the return value now, so maybe change the function signature.
    }

    if (client->phase == HANDLING_REQUEST) {
        Request *request = &client->request;

        // Find a matching handler.
        Request_handler *handler = NULL;
        for (s64 i = 0; i < server->routes.count; i++) {
            Route *route = &server->routes.data[i];
            if (route->method != request->method)  continue;

            request->captures = (Captures){.context = client->context};
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

        // If the response has a body but no size, calculate the size assuming the body is a zero-terminated string.
        if (response->body && !response->size)  response->size = strlen(response->body);

        if (!response->headers.context) {
            // If the headers dict has no context, we assume the handler didn't touch it beyond zero-initialising it.
            assert(!memcmp(&response->headers, &(string_dict){0}, sizeof(string_dict)));
            response->headers = (string_dict){.context = client->context};
        }
        // Add a content-length header.
        *Set(&response->headers, "content-length") = get_string(client->context, "%ld", response->size)->data;

        // Print the headers into a buffer.
        {
            char_array *reply_header = &client->reply_header;
            *reply_header = (char_array){.context = client->context};

            append_string(reply_header, "HTTP/1.0 %d\r\n", response->status);
            for (s64 i = 0; i < response->headers.count; i++) {
                char *key   = response->headers.keys[i];
                char *value = response->headers.vals[i];
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
            // We've fully sent our reply.
            assert(*num_bytes_sent == full_reply_size);

            client->phase = READY_TO_CLOSE;
        }
    }

    if (client->phase == READY_TO_CLOSE) {
        bool closed = !close(client->socket_no);
        if (!closed) {
            Fatal("We couldn't close a client socket (%s).", get_last_error().string);
        }
        client->phase = CLOSED;
    }

    add_to_queue(server->done_queue, client);

    // Write a byte to the worker pipe to tell the server to poll the client again.
    {
        u8 byte = 1;
        s64 r = write(server->worker_pipe_nos[1], &byte, sizeof(byte));
        if (r == -1) {
            //|Todo: Check EINTR?
            Fatal("We couldn't write to the worker pipe (%s).", get_last_error().string);
        }
    }
}

static void *thread_start(void *arg)
// The worker thread's loop.
{
    Server *server = arg;
    HTTP_queue *queue = server->work_queue;

    while (true) {
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

    server->work_queue = create_queue(context, true);

    server->done_queue = create_queue(context, false);

    server->worker_threads = (pthread_t_array){.context = context};
    int NUM_WORKER_THREADS = 1; //|Todo: Put this somewhere else.
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
    //|Todo: Should we make the pipe nonblocking?

    return server;
}

void start_server(Server *server)
{
    // On each iteration of the main loop, we'll build an array of file descriptors to poll.
    Array(struct pollfd) pollfds = {.context = server->context};

    // The first three elements in the pollfds array don't change:
    // pollfds.data[0] is our main socket listening for connections.
    *Add(&pollfds) = (struct pollfd){.fd = server->socket_no, .events = POLLIN};
    // pollfds.data[1] is the file descriptor for SIGINT.
    *Add(&pollfds) = (struct pollfd){.fd = server->sigint_file_no, .events = POLLIN};
    // pollfds.data[2] is the worker pipe.
    *Add(&pollfds) = (struct pollfd){.fd = server->worker_pipe_nos[0], .events = POLLIN};
    // Any other elements in the array are open client connections.

    // Also build an array of clients to remove at the end of each loop.
    Array(Client*) to_delete = {.context = server->context};

    bool server_should_stop = false;

    while (!server_should_stop)
    {
        while (true) {
            Client *client = pop_queue(server->done_queue); //|Speed: We lock and unlock the queue mutex every time.
            if (!client)  break;

            assert(*Get(&server->clients, client->socket_no) == client);

            // Every task in the done queue is associated with one byte written to the worker pipes. We'll read the byte now. This is so that, if there have been multiple writes to the pipes, we consume them all, so we don't get a redundant POLLIN when we next poll.
            u8 byte;
            read(server->worker_pipe_nos[0], &byte, sizeof(byte));

            if (client->phase == CLOSED) {
                *Add(&to_delete) = client;
                //|Temporary: We need to clean up straight away because if we try to keep accepting new connections first, the operating system will reassign the socket file descriptor for this client to a new client and when we create the new client, we will overwrite this one on the client map. In fact I think it's still a bug regardless. Because as soon as a thread closes a socket, we could get another connection that gets picked up by the poll() before the thread gets a chance to add the socket to the done_queue. |Bug
                goto cleanup;
            }

            struct pollfd pollfd = {.fd = client->socket_no};
            switch (client->phase) {
                case PARSING_REQUEST:  pollfd.events |= POLLIN;   break;
                case SENDING_REPLY:    pollfd.events |= POLLOUT;  break;

                default:  assert(!"Unexpected request phase.");
            }
            *Add(&pollfds) = pollfd;
        }

        // If there are clients to delete, wait half a second. Otherwise wait indefinitely.
        int timeout_ms = (to_delete.count > 0) ? 500 : -1;

        int num_events = poll(pollfds.data, pollfds.count, timeout_ms);
        if (num_events < 0) {
            Fatal("poll failed (%s).", get_last_error().string);
        }

        s64 current_time = get_monotonic_time(); // We need to get this value after polling, but before jumping to cleanup.

        if (num_events == 0)  goto cleanup;

        for (s64 pollfd_index = pollfds.count-1; pollfd_index >= 0; pollfd_index -= 1) { // Iterate backwards so we can delete from the array.
            struct pollfd *pollfd = &pollfds.data[pollfd_index];

            if (!pollfd->revents)  continue;
            //|Speed: We should decrement num_events and break when it hits 0.

            if (pollfd->revents & (POLLERR|POLLHUP|POLLNVAL)) {
                if (server->verbose) {
                    if (pollfd->revents & POLLERR)   printf("POLLERR on socket %d.\n", pollfd->fd);
                    if (pollfd->revents & POLLHUP)   printf("POLLHUP on socket %d.\n", pollfd->fd);
                    if (pollfd->revents & POLLNVAL)  printf("POLLNVAL on socket %d.\n", pollfd->fd);
                }

                Client *client = *Get(&server->clients, pollfd->fd);
                client->phase = READY_TO_CLOSE;
                add_to_queue(server->work_queue, client);//|Todo: It's probably better to just close it ourselves.
                array_unordered_remove_by_index(&pollfds, pollfd_index);
                continue;
            }

            if (pollfd->fd == server->sigint_file_no) {
                // We've received a SIGINT.
                struct signalfd_siginfo info; // We don't do anything with this, but we need to read it or we'll get POLLIN on the next loop.
                read(server->sigint_file_no, &info, sizeof(info));

                server_should_stop = true;
                continue;
            }

            if (pollfd->fd == server->worker_pipe_nos[0]) {
                // A worker thread is letting us know that they put something in the done_queue.
                // We'll check the queue on the next frame. There's nothing to do now.
                continue;
            }

            if (pollfd->fd == server->socket_no) {
                // A new connection has occurred.
                if (server_should_stop)  continue; //|Todo: Don't even poll the listen socket in this case.

                struct sockaddr_in client_socket_addr; // This will be initialised by accept().
                socklen_t client_socket_addr_size = sizeof(client_socket_addr);

                int client_socket_no = accept(server->socket_no, (struct sockaddr *)&client_socket_addr, &client_socket_addr_size);
                if (client_socket_no < 0) {
                    Fatal("poll() said we could read from our main socket, but we couldn't get a new connection (%s).", get_last_error().string);
                }

                set_blocking(client_socket_no, false);

                // Create a new memory context for the client and initialise the Client struct on that.
                Memory_context *client_context = new_context(server->context);

                Client *client = New(Client, client_context);
                client->context    = client_context;
                client->start_time = current_time;
                client->socket_no  = client_socket_no;
                client->phase      = PARSING_REQUEST;

                assert(!IsSet(&server->clients, client_socket_no));
                *Set(&server->clients, client_socket_no) = client;
                add_to_queue(server->work_queue, client);
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

cleanup:
        for (s64 i = 0; i < to_delete.count; i++) {
            Client *client = to_delete.data[i];
            assert(client->phase == CLOSED);

            { //|Cleanup:
                Memory_context *ctx = client->context;
                Request *req = &client->request;
                char *method = req->method == GET ? "GET" : req->method == POST ? "POST" : "UNKNOWN!!";
                char *path   = req->path.count ? req->path.data : "";
                char *query  = req->query.count ? encode_query_string(&req->query, ctx)->data : "";
                printf("[%d] %s %s%s\n", client->response.status, method, path, query);
                fflush(stdout);
            }

            Delete(&server->clients, client->socket_no);
            free_context(client->context);
        }
        to_delete.count = 0;

        if (server_should_stop) {
            if (server->clients.count > 0)  continue;//|Fixme: This won't work if server_should_stop is also the loop condition though.

            // Signal to the worker threads that it's time to wind up.
            for (s64 i = 0; i < server->worker_threads.count; i++)  add_to_queue(server->work_queue, NULL);
        }
    }

    // Join the worker threads.
    for (int i = 0; i < server->worker_threads.count; i++) {
        int r = pthread_join(server->worker_threads.data[i], NULL);
        if (r) {
            Fatal("Failed to join a thread (%s).", get_error_info(r).string);
        }
    }

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
