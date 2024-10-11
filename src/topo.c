#include "array.h"
#include "map.h"
#include "pg.h"
#include "shapes.h"

typedef struct Node     Node;
typedef Array(Node)     Node_array;
typedef struct Edge     Edge;
typedef Array(Edge)     Edge_array;
typedef struct Topology Topology;

struct Node {
    s32     id;
    Vector2 geom;
};

struct Edge {
    s32     id;

    s32     start_node;
    s32     end_node;

    // The ID of the next edge you encounter if you're:
    s32     next_left_edge;  // ... on the left side of the edge, walking from the start node to the end node.
    s32     next_right_edge; // ... on the right side of the edge, walking backwards (from the end node to the start node).
    // The IDs are negated if, when we reach the next edge, we are at its end node rather than its start node.

    Path    geom;
};

struct Topology {
    Node_array nodes;
    Edge_array edges;
};

//|Copypasta. These belong in pg.c. We shouldn't need to expose them in the pg.h header file because we'll expose parsing functions instead.
enum WKB_Byte_Order {WKB_BIG_ENDIAN, WKB_LITTLE_ENDIAN};
enum WKB_Geometry_Type {WKB_POINT=1, WKB_LINESTRING, WKB_POLYGON, WKB_MULTIPOINT, WKB_MULTILINESTRING, WKB_MULTIPOLYGON, WKB_GEOMETRYCOLLECTION};

Vector2 parse_point(u8 *data, u8 **end_data)
//|Todo: Move to pg.h and use in pg.c.
{
    u8 *d = data;

    double x;  memcpy(&x, d, sizeof(double));
    d += sizeof(double);

    double y;  memcpy(&y, d, sizeof(double));
    d += sizeof(double);

    Vector2 p = {(float)x, (float)y};

    if (end_data)  *end_data = d;

    return p;
}

Topology *load_topology(PGconn *database, char *topology_name, Memory_context *context)
{
    Memory_context *ctx = context;

    Topology *topology = New(Topology, ctx);
    topology->nodes = (Node_array){.context = ctx};
    topology->edges = (Edge_array){.context = ctx};

    // Get the nodes.
    {
        char query[1024];
        int num_chars = snprintf(query, sizeof(query), "select node_id, st_asbinary(geom) from %s.node", topology_name);
        assert(num_chars < sizeof(query));

        Postgres_result *result = query_database(database, query, NULL, ctx);

        int id_column   = *Get(&result->columns, "node_id");      assert(id_column >= 0);
        int geom_column = *Get(&result->columns, "st_asbinary");  assert(geom_column >= 0);

        for (s64 row = 0; row < result->rows.count; row++) {
            u8_array *id_cell   = &result->rows.data[row].data[id_column];
            u8_array *geom_cell = &result->rows.data[row].data[geom_column];

            Node *node = Add(&topology->nodes);

            node->id = (s32)get_u32_from_cell(id_cell);

            // Parse the node's point geometry. |Cleanup: Factor into a function in pg.h.
            u8 *d = geom_cell->data;

            u8 byte_order = *d;
            d += sizeof(u8);
            assert(byte_order == WKB_LITTLE_ENDIAN);

            u32 wkb_type;  memcpy(&wkb_type, d, sizeof(u32));
            d += sizeof(u32);
            assert(wkb_type == WKB_POINT);

            node->geom = parse_point(d, &d);

            assert((d - geom_cell->data) == geom_cell->count);
        }
    }

    // Get the edges.
    {
        char query[1024];
        int num_chars = snprintf(query, sizeof(query), "select *, st_asbinary(geom) from %s.edge", topology_name);
        assert(num_chars < sizeof(query));

        Postgres_result *result = query_database(database, query, NULL, ctx);

        int id_column              = *Get(&result->columns, "edge_id");          assert(id_column >= 0);
        int start_node_column      = *Get(&result->columns, "start_node");       assert(start_node_column >= 0);
        int end_node_column        = *Get(&result->columns, "end_node");         assert(end_node_column >= 0);
        int next_left_edge_column  = *Get(&result->columns, "next_left_edge");   assert(next_left_edge_column >= 0);
        int next_right_edge_column = *Get(&result->columns, "next_right_edge");  assert(next_right_edge_column >= 0);
        int geom_column            = *Get(&result->columns, "st_asbinary");      assert(geom_column >= 0);

        for (s64 row = 0; row < result->rows.count; row++) {
            u8_array *id_cell              = &result->rows.data[row].data[id_column];
            u8_array *start_node_cell      = &result->rows.data[row].data[start_node_column];
            u8_array *end_node_cell        = &result->rows.data[row].data[end_node_column];
            u8_array *next_left_edge_cell  = &result->rows.data[row].data[next_left_edge_column];
            u8_array *next_right_edge_cell = &result->rows.data[row].data[next_right_edge_column];
            u8_array *geom_cell            = &result->rows.data[row].data[geom_column];

            Edge *edge = Add(&topology->edges);
            edge->geom = (Path){.context = ctx};

            edge->id              = (s32)get_u32_from_cell(id_cell);
            edge->start_node      = (s32)get_u32_from_cell(start_node_cell);
            edge->end_node        = (s32)get_u32_from_cell(end_node_cell);
            edge->next_left_edge  = (s32)get_u32_from_cell(next_left_edge_cell);
            edge->next_right_edge = (s32)get_u32_from_cell(next_right_edge_cell);

            // Parse the edge's linestring geometry. |Cleanup: Factor into a function in pg.h. It nearly exists but we flip the Y axis so it's no good generically...
            u8 *d = geom_cell->data;

            u8 byte_order = *d;
            d += sizeof(u8);
            assert(byte_order == WKB_LITTLE_ENDIAN);

            u32 wkb_type;  memcpy(&wkb_type, d, sizeof(u32));
            d += sizeof(u32);
            assert(wkb_type == WKB_LINESTRING);

            u32 num_points;  memcpy(&num_points, d, sizeof(u32));
            d += sizeof(u32);

            while (num_points--)  *Add(&edge->geom) = parse_point(d, &d);

            assert((d - geom_cell->data) == geom_cell->count);
        }
    }

    return topology;
}

int main()
{
    Memory_context *ctx = new_context(NULL);

    PGconn *db = connect_to_database("postgres://postgres:postgisclarity@osm.tal/gis");

    Topology *topo = load_topology(db, "jpj_topo", ctx);

    printf("Number of nodes: %ld\n", topo->nodes.count);
    printf("Number of edges: %ld\n", topo->edges.count);

    free_context(ctx);
    return 0;
}
