#ifndef PG_H_INCLUDED
#define PG_H_INCLUDED

#include "libpq-fe.h"

#include "map.h"
#include "shapes.h"

//
// A Postgres result is an array of rows. Each row is an array of cells. Each cell is a u8_array.
// There is also a hash table that you can use to get the indexes of columns.
// The hash table returns -1 if the column does not exist.
//
//      Postgres_result *result;
//
//      s64 column_index = *Get(result->columns, "my_column_name");
//
//      if (column_index < 0) // There is no column called "my_column_name" in the results.
//
//      for (s64 row_index = 0; row_index < result->rows.count; row_index++) {
//          u8_array2 *row = &result->rows.data[row_index];
//
//          u8_array *cell = &row->data[column_index]; // This is the cell associated with "my_column_name".
//      }
//
typedef struct Postgres_result Postgres_result;

struct Postgres_result {
    s64_dict  *columns;
    u8_array3  rows;
};

PGconn *connect_to_database(char *url);
void parse_polygons(u8 *data, Polygon_array *result, u8 **end_data);
void parse_paths(u8 *data, Path_array *result, u8 **end_data);
Path_array *query_paths(PGconn *db, char *query, string_array *params, Memory_context *context);
Postgres_result *query_database(PGconn *db, char *query, string_array *params, Memory_context *context);
int get_int_from_cell(u8_array *cell);

#endif // PG_H_INCLUDED
