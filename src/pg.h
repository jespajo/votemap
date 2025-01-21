#ifndef PG_H_INCLUDED
#define PG_H_INCLUDED

#include "libpq-fe.h"

#include "array.h"
#include "map.h"

typedef struct PG_client PG_client;
typedef struct PG_result PG_result;

//
// You should initialise a PG_client statically.
//
//      PG_client db = {"postgres://user:pass@host:port/database", .use_cache = true};
//
// A connection to the database will be established when you call query_database().
// The connection will also be closed by this function unless the client's .keep_alive is true.
// So if you set .keep_alive, you'll need to call close_database() yourself when you're done.
//
struct PG_client {
    char   *conn_string;
    PGconn *conn;
    bool    keep_alive; // If true, don't close the connection after a query.
    bool    use_cache;  // If true, cache query results.
};

//
// A Postgres result is an array of rows. Each row is an array of cells. Each cell is a u8_array.
// There is also a hash table that you can use to get the indexes of columns.
// The hash table returns -1 if the column does not exist.
//
//      Postgres_result *result = query_database(db, query, params, ctx);
//
//      int column_index = *Get(result->columns, "my_column_name");
//
//      if (column_index < 0) // There is no column called "my_column_name" in the results.
//
//      for (s64 row_index = 0; row_index < result->rows.count; row_index++) {
//          u8_array2 *row = &result->rows.data[row_index];
//
//          u8_array *cell = &row->data[column_index]; // This is the cell associated with "my_column_name".
//      }
//
struct PG_result {
    int_dict  columns;
    u8_array3 rows;
};

PG_result *query_database(PG_client *client, char *query, string_array *params, Memory_context *context);
void close_database(PG_client *client);
u32 get_u32_from_cell(u8_array *cell);
float get_float_from_cell(u8_array *cell);
char_array get_char_array_from_cell(u8_array *cell);

#endif // PG_H_INCLUDED
