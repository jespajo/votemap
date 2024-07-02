#ifndef PG_H_INCLUDED
#define PG_H_INCLUDED

#include "libpq-fe.h"

#include "map.h"
#include "shapes.h"

//
// A Postgres_result is an array of rows. Each row is a hash table, which has all the row's cells
// (one for each column) keyed by the column names. An empty cell in a result is represented by a
// pointer to a zeroed-out u8_array struct. So:
//
//      u8_array *cell = *Get(row, "my_column_name");
//
//      if (!cell)             <-- The query didn't have a column called my_column_name.
//      else if (!cell->count) <-- The column name is correct, but the cell in this row is empty.
//
typedef Dict(u8_array *)    Result_row;
typedef Array(Result_row *) Postgres_result;

PGconn *connect_to_database(char *url);
void parse_polygons(u8 *data, Polygon_array *result, u8 **end_data);
Polygon_array *query_polygons(PGconn *db, char *query, string_array *params, Memory_Context *context);
void parse_paths(u8 *data, Path_array *result, u8 **end_data);
Path_array *query_paths(PGconn *db, char *query, string_array *params, Memory_Context *context);
Postgres_result *query_database(PGconn *db, char *query, string_array *params, Memory_Context *context);

#endif // PG_H_INCLUDED
