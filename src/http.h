#ifndef HTTP_H_INCLUDED
#define HTTP_H_INCLUDED

#include "array.h"
#include "map.h"

typedef struct Server              Server;
typedef struct Request             Request;
typedef struct Response            Response;
typedef struct Pending_request     Pending_request;
typedef Map(s32, Pending_request)  Pending_requests;  // A map from client socket file descriptors to their associated Pending_request structs.
typedef Response Request_handler(Request *, Memory_Context *);
typedef struct Route               Route;
typedef Array(Route)               Route_array;

enum HTTP_method {GET=1, POST};

struct Route {
    enum HTTP_method    method;
    char               *path;
    Request_handler    *handler;
};

struct Server {
    u32                     address;
    u16                     port;
    bool                    verbose;
    Memory_Context         *context;

    s32                     socket_no;

    Route_array             routes;
    Route                  *default_route;

    Pending_requests       *pending_requests;
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

Server *create_server(u32 address, u16 port, bool verbose, Memory_Context *context);
void start_server(Server *server);
void add_route(Server *server, Route route);
void set_default_route(Server *server, Route route);
Response serve_file(Request *request, Memory_Context *context);
Response handle_404(Request *request, Memory_Context *context);

#endif // HTTP_H_INCLUDED
