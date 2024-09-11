#ifndef HTTP_H_INCLUDED
#define HTTP_H_INCLUDED

#include "array.h"
#include "map.h"

typedef struct Server     Server;
typedef struct Request    Request;
typedef struct Response   Response;
typedef struct Client     Client;
typedef struct Route      Route;
typedef Array(Route)      Route_array;
typedef Map(s32, Client)  Client_map;  // A map from client socket file descriptors to their associated Client structs.

struct Server {
    Memory_context         *context;

    u32                     address;
    u16                     port;
    bool                    verbose;

    s32                     socket_no;

    Route_array             routes;

    Client_map             *clients;
};

enum HTTP_method {GET=1, POST};

struct Request {
    enum HTTP_method        method;
    char_array              path;
    string_dict            *query;
};

struct Response {
    int                     status;
    string_dict            *headers;
    void                   *body;
    s64                     size;           // The number of bytes in the body. If 0, body (if set) points to a zero-terminated string.
};

// A Request_handler is a function that takes a pointer to a Request and returns a Response.
typedef Response Request_handler(Request *, Memory_context *);

struct Route {
    enum HTTP_method        method;
    char                   *path;
    Request_handler        *handler;
};

Server *create_server(u32 address, u16 port, bool verbose, Memory_context *context);
void start_server(Server *server);

// Request_handler functions:
Response serve_file(Request *request, Memory_context *context);
Response serve_file_NEW(Request *request, Memory_context *context);//|Temporary
Response serve_404(Request *request, Memory_context *context);

#endif // HTTP_H_INCLUDED
