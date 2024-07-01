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
typedef Response Request_handler(Request *, Memory_Context *);

struct Server {
    u32                     address;
    u16                     port;
    bool                    verbose;
    Memory_Context         *context;

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

struct Client {
    Memory_Context         *context;

    s32                     socket_no;      // The client socket's file descriptor.
    s64                     start_time;     // When we accepted the connection.

    enum Request_phase {
        PARSING_REQUEST=1,
        HANDLING_REQUEST,
        SENDING_REPLY,
        READY_TO_CLOSE,
    }                       phase;

    char_array              message;        // A buffer for storing bytes received.
    Request                 request;

    Response                response;

    char_array              reply_header;   // Our response's header in raw text form.
    s64                     num_bytes_sent; // The total number of bytes we've sent of our response. Includes both header and body.
};

struct Route {
    enum HTTP_method        method;
    char                   *path;
    Request_handler        *handler;
};

Server *create_server(u32 address, u16 port, bool verbose, Memory_Context *context);
void start_server(Server *server);
Response serve_file(Request *request, Memory_Context *context);
Response serve_404(Request *request, Memory_Context *context);

#endif // HTTP_H_INCLUDED
