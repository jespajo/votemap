#ifndef HTTP_H_INCLUDED
#define HTTP_H_INCLUDED

#include "array.h"
#include "map.h"
#include "regex.h"

typedef struct Server      Server;
typedef struct Request     Request;
typedef struct Response    Response;
typedef struct Client      Client;
typedef struct Route       Route;
typedef Array(Route)       Route_array;
typedef Map(s32, Client*)  Client_map;
typedef Array(pthread_t)   pthread_t_array;
typedef struct Client_queue Client_queue;

struct Server {
    Memory_context         *context;

    u32                     address;
    u16                     port;

    s32                     socket;             // The file descriptor for the socket that accepts connections.
    s32                     interrupt_handle;   // The file descriptor for handling SIGINT.

    Route_array             routes;

    Client_map              clients;

    Client_queue           *work_queue;
    pthread_t_array         worker_threads;
    s32                     worker_pipe[2];     // File descriptors for the pipe that workers use to communicate to the server (read, write).
};

enum HTTP_method {GET=1, POST}; // We can add HTTP_ prefixes to these later if we need.

struct Request {
    enum HTTP_method        method;
    char_array              path;
    string_array            path_params;
    string_dict             query;//|Todo: Rename query_params.
};

struct Response {
    int                     status;
    string_dict             headers;        // The request handler is expected to set the content-type header. The server sets the content-length header.
    void                   *body;
    s64                     size;           // The number of bytes in the body.
};

// A Request_handler is a function that takes a pointer to a Request and returns a Response.
typedef Response Request_handler(Request*, Memory_context*);

struct Route {
    enum HTTP_method        method;
    Regex                  *path_regex;
    Request_handler        *handler;
};

Server *create_server(u32 address, u16 port, Memory_context *context);
void start_server(Server *server);
void add_route(Server *server, enum HTTP_method method, char *path_pattern, Request_handler *handler);
void add_file_route(Server *server, char *path_pattern, char *directory);

// Request_handler functions:
Response serve_files(Request *request, Memory_context *context); //|Cleanup: Remove this, because external code shouldn't use it directly, only via add_file_route().
Response serve_404(Request *request, Memory_context *context);

#endif // HTTP_H_INCLUDED
