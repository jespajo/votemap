#include <math.h>

#include "../array.h"
#include "../map.h"
#include "../pg.h"
#include "../shapes.h"
#include "../strings.h"

const double PI = 4*atan(1);

typedef struct Edge     Edge;
typedef Map(s32, Edge)  Topology;

struct Edge {
    //
    // We get the terms "next left edge" and "next right edge" from PostGIS. If you walked
    // on the left side of the path, the next edge you'd come to would be the next left edge.
    // The next right edge is the one you'd come to if you were on the other side, facing
    // the opposite direction. If, on your walk, you first come to the end of the next edge,
    // rather than the start, the ref is negative.
    //
    //   \   /prev_left                                          next_left|
    //    \ /                             edge->                          |
    //     X--------------------------------------------------------------+-----
    //    / \                                                             |
    //   /   \next_right                                        prev_right|
    //
    // Edges store references to their next neighbours only.
    //
    s32     next_refs[2]; // Next left edge, next right edge.

    bool    outer; // True if at least one of the edge's two faces is "the universal face"---i.e. the edge is on the outside of the overall shape.

    Path    geom;
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

//|Todo: Move to map.h.
void map_reserve_(void **keys, void **vals, s64 *count, s64 *limit, s64 new_limit, u64 key_size, u64 val_size, Hash_bucket **buckets, s64 *num_buckets, Memory_context *context)
{
    init_map_if_needed(keys, vals, count, limit, key_size, val_size, buckets, num_buckets, context);

    // Resize the key/value arrays.
    {
        *limit = round_up_pow2(new_limit+1);//|Cleanup: I don't think we should add 1. Callers should know they need to reserve one more than they need. They should do the rounding themselves as well.

        *keys = (u8 *)*keys - key_size;
        *vals = (u8 *)*vals - val_size;
        *keys = resize(*keys, *limit, key_size, context);
        *vals = resize(*vals, *limit, val_size, context);
        *keys = (u8 *)*keys + key_size;
        *vals = (u8 *)*vals + val_size;
    }

    // Resize the buckets.
    {
        s64 new_num_buckets = round_up_pow2(3*new_limit/2); //|Todo: Figure out what it should really be. |Incomplete

        Hash_bucket *new_buckets = New(new_num_buckets, Hash_bucket, context);

        for (s64 old_index = 0; old_index < *num_buckets; old_index++) {
            if (!(*buckets)[old_index].hash)  continue;

            s64 new_index = (*buckets)[old_index].hash % (2*(*num_buckets));
            while (new_buckets[new_index].hash) {
                new_index -= 1;
                if (new_index < 0)  new_index += 2*(*num_buckets);
            }
            new_buckets[new_index] = (*buckets)[old_index];
        }

        dealloc(*buckets, context);

        *buckets = new_buckets;
        *num_buckets = new_num_buckets;
    }

}
#define map_reserve(MAP, LIMIT)  \
    map_reserve_((void**)&(MAP)->keys, (void**)&(MAP)->vals, &(MAP)->count, &(MAP)->limit, (LIMIT), sizeof(*(MAP)->keys), sizeof(*(MAP)->vals), &(MAP)->buckets, &(MAP)->num_buckets, (MAP)->context)

Topology *load_topology(PGconn *database, char *topology_name, Memory_context *context)
{
    Memory_context *ctx = context;

    Topology *topo = NewMap(topo, ctx);

    char query[1024];
    {
        char *format = "select edge_id, next_left_edge, next_right_edge, left_face, right_face, st_asbinary(geom) from %s.edge";
        int num_chars = snprintf(query, sizeof(query), format, topology_name);
        assert(num_chars < sizeof(query));
    }

    Postgres_result *result = query_database(database, query, NULL, ctx);

    int id_column              = *Get(&result->columns, "edge_id");          assert(id_column >= 0);
    int next_left_edge_column  = *Get(&result->columns, "next_left_edge");   assert(next_left_edge_column >= 0);
    int next_right_edge_column = *Get(&result->columns, "next_right_edge");  assert(next_right_edge_column >= 0);
    int left_face_column       = *Get(&result->columns, "left_face");        assert(left_face_column >= 0);
    int right_face_column      = *Get(&result->columns, "right_face");       assert(right_face_column >= 0);
    int geom_column            = *Get(&result->columns, "st_asbinary");      assert(geom_column >= 0);

    map_reserve(topo, result->rows.count);

    for (s64 row = 0; row < result->rows.count; row++) {
        u8_array *id_cell              = &result->rows.data[row].data[id_column];
        u8_array *next_left_edge_cell  = &result->rows.data[row].data[next_left_edge_column];
        u8_array *next_right_edge_cell = &result->rows.data[row].data[next_right_edge_column];
        u8_array *left_face_cell       = &result->rows.data[row].data[left_face_column];
        u8_array *right_face_cell      = &result->rows.data[row].data[right_face_column];
        u8_array *geom_cell            = &result->rows.data[row].data[geom_column];

        int edge_id = (int)get_u32_from_cell(id_cell);

        Edge *edge = Set(topo, edge_id);

        edge->next_refs[0] = (s32)get_u32_from_cell(next_left_edge_cell);
        edge->next_refs[1] = (s32)get_u32_from_cell(next_right_edge_cell);

        s64 left_face  = (s32)get_u32_from_cell(left_face_cell);
        s64 right_face = (s32)get_u32_from_cell(right_face_cell);
        edge->outer = (left_face == 0 || right_face == 0);

        // Parse the edge's linestring geometry. |Cleanup: Factor into a function in pg module. parse_path() exists but flips the Y axis so it's no good.
        edge->geom = (Path){.context = ctx};
        {
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

    return topo;
}

char_array *get_topology_printed(Topology *topology, Memory_context *context)
{
    Path_array faces = {.context = context}; //|Leak
    {
        Path face = {.context = context}; // We reuse this variable for the pending face. To avoid undue leaks, we only clear it fully when we add a face---otherwise we just set the count to 0 so that we can reuse its data block.

        for (s64 edge_index = 0; edge_index < topology->count; edge_index++) {
            s32 edge_id = topology->keys[edge_index];
            Edge *edge = &topology->vals[edge_index];

            for (int side = 0; side < 2; side++) {
                face.count = 0;

                // To make sure we only add each face once, we'll only add a face when we come to its lowest edge ID.
                bool lowest = true;

                for (s64 i = 0; i < edge->geom.count; i++) {
                    Vector2 p = edge->geom.data[side ? edge->geom.count-1-i : i];
                    *Add(&face) = p;
                }

                s32 next_ref = edge->next_refs[side];
                while (true) {
                    s32 next_id = abs(next_ref);
                    if (next_id < edge_id) {
                        lowest = false;
                        break;
                    }

                    assert(IsSet(topology, next_id));
                    Edge *e = Get(topology, next_id);

                    for (s64 i = 0; i < e->geom.count; i++) {
                        Vector2 p = e->geom.data[next_ref < 0 ? e->geom.count-1-i : i];
                        if (!i)  assert(same_point(p, face.data[face.count-1]));
                        else     *Add(&face) = p;
                    }

                    if (e->next_refs[next_ref < 0] == (side ? -edge_id : edge_id))  break;

                    next_ref = e->next_refs[next_ref < 0];
                }
                if (!lowest)  continue;

                // We've come to the lowest edge ID for the face.
                assert(face.count && same_point(face.data[0], face.data[face.count-1]));
                *Add(&faces) = face;
                face = (Path){.context = context};
            }
        }
    }

    char_array *out = get_string(context, "MULTIPOLYGON(");
    {
        for (s64 i = 0; i < faces.count; i++) {
            Path *face = &faces.data[i];
            assert(face->count);
            assert(same_point(face->data[0], face->data[face->count-1]));

            if (!i)  append_string(out, "\n ((");
            else     append_string(out, ",\n ((");

            for (s64 j = 0; j < face->count; j++) {
                float x = face->data[j].v[0];
                float y = face->data[j].v[1];

                if (!j)  append_string(out, "%.0f %.0f", x, y);
                else     append_string(out, ", %.0f %.0f", x, y);
            }
            append_string(out, "))");
        }
        *Add(out) = ')';
    }

    return out;
}

float get_ring_area(Vector2 *points, s64 num_points)
{
    assert(same_point(points[0], points[num_points-1]));

    float a = 0;
    for (int i = 0; i < num_points-1; i++) { // Minus 1 because the first point is the same as the last.
        float x1 = points[i].v[0];
        float y1 = points[i].v[1];
        float x2 = points[i+1].v[0];
        float y2 = points[i+1].v[1];

        a += (y1+y2)*(x1-x2);
    }

    float area = 0.5*fabsf(a);

    return area;
}

float get_path_length(Vector2 *points, s64 num_points)
{
    float length = 0;

    for (int i = 0; i < num_points-1; i++) {
        float x1 = points[i].v[0];
        float y1 = points[i].v[1];
        float x2 = points[i+1].v[0];
        float y2 = points[i+1].v[1];

        length += hypotf(x2-x1, y2-y1);
    }

    return length;
}

void remove_dangling_edges(Topology *topology)
{
    bool done = true;

    for (s64 edge_index = 0; edge_index < topology->count; edge_index++) {
        s32 edge_id = topology->keys[edge_index];
        Edge *edge = &topology->vals[edge_index];

        for (int side = 0; side < 2; side++) {
            s32 next_ref = edge->next_refs[side];
            s32 next_id  = abs(next_ref);

            assert(IsSet(topology, next_id));
            Edge *next = Get(topology, next_id);

            if (next_ref < 0) {
                if (next->next_refs[1] !=  next_id)  continue;
            } else {
                if (next->next_refs[0] != -next_id)  continue;
            }

            // The next edge is dangling. We will delete it.
            done = false;

            edge->next_refs[side] = next->next_refs[next_ref > 0];

            Delete(topology, next_id);
            printf("Deleted dangling edge %d.\n", next_id);

            break;
        }
    }

    if (!done)  remove_dangling_edges(topology);
}

void simplify_topology(Topology *topology, Memory_context *scratch)
{
    Memory_context *ctx = (scratch) ? scratch : new_context(topology->context);
    reset_context(ctx);

    bool done = true;

    Path face = {.context = ctx};

    for (s64 edge_index = 0; edge_index < topology->count; edge_index++) {
        s32 edge_id = topology->keys[edge_index];
        Edge *edge = &topology->vals[edge_index];

        if (edge->outer)  continue;

        // The first task is to follow the edges to construct the face on both sides of the current edge.
        // As we do so, we'll collect a couple things:
        // - The ID of the edge on this side just before the current edge. These will be positive if the current edge is their next left edge, or negative if the current edge is their next right edge.
        s32   prev_refs[2];
        // - And some statistics about the geometry of each face.
        float area[2];
        float perim[2];
        float thickness[2];

        for (int side = 0; side < 2; side++) {
            face.count = 0;

            // Construct the geometry of the current side's face.
            {
                for (s64 i = 0; i < edge->geom.count; i++) {
                    Vector2 p = edge->geom.data[side ? edge->geom.count-1-i : i];
                    *Add(&face) = p;
                }

                s32 next_ref = edge->next_refs[side];
                while (true) {
                    assert(IsSet(topology, abs(next_ref)));
                    Edge *e = Get(topology, abs(next_ref));

                    for (s64 i = 0; i < e->geom.count; i++) {
                        Vector2 p = e->geom.data[next_ref < 0 ? e->geom.count-1-i : i];
                        if (!i)  assert(same_point(p, face.data[face.count-1]));
                        else     *Add(&face) = p;
                    }

                    if (e->next_refs[next_ref < 0] == (side ? -edge_id : edge_id)) {
                        prev_refs[side] = next_ref;
                        break;
                    }

                    next_ref = e->next_refs[next_ref < 0];
                }

                if (prev_refs[side] < 0)  assert(abs(Get(topology, -prev_refs[side])->next_refs[1]) == edge_id);
                else                      assert(abs(Get(topology,  prev_refs[side])->next_refs[0]) == edge_id);
            }
            assert(face.count && same_point(face.data[0], face.data[face.count-1]));

            area[side]  = get_ring_area(face.data, face.count);
            perim[side] = get_path_length(face.data, face.count);
            thickness[side] = (4*PI*area[side])/(perim[side]*perim[side]);
        }

        // Test whether we should merge the faces by deleting the edge.
        bool remove_edge = false;

        for (int side = 0; side < 2; side++) {
            bool keep_face = false;

            // We'll only keep the face if it's a shape we like.
            keep_face |= (area[side] > 1000000000);
            keep_face |= (area[side] >   84000000 && thickness[side] > 0.049);
            keep_face |= (area[side] >   52000000 && thickness[side] > 0.05);
            keep_face |= (area[side] >   11000000 && thickness[side] > 0.06);
            keep_face |= (area[side] >    5700000 && thickness[side] > 0.13);
            keep_face |= (area[side] >    3200000 && thickness[side] > 0.14);
            keep_face |= (area[side] >     430000 && thickness[side] > 0.15);
            keep_face |= (area[side] >     300000 && thickness[side] > 0.53);

            // We'll always remove the face if it's a sliver compared to its neighbour.
            keep_face &= !(area[side] < 0.00002*area[!side]); // This narrowly avoids Lingiari eating Solomon.
            keep_face &= !(area[side] < 0.001*area[!side] && thickness[side] < 0.15); // This targets the huge slivers in the border between O'Connor and Durack.

            if (!keep_face) {
                remove_edge = true;
                break;
            }
        }

        if (!remove_edge)  continue;

        // We are going to delete the edge.
        done = false;

        // Update the references to the deleted edge's ID.
        for (int side = 0; side < 2; side++) {
            s32 prev_id = abs(prev_refs[side]);
            if (prev_id == edge_id)  continue;

            assert(IsSet(topology, prev_id));
            Edge *prev = Get(topology, prev_id);

            s32 new_ref = (abs(edge->next_refs[!side]) == edge_id) ? edge->next_refs[side] : edge->next_refs[!side];

            prev->next_refs[prev_refs[side] < 0] = new_ref;
            printf("Updated (edge %d)->next_refs[%d] to %d.\n", prev_id, prev_refs[side] < 0, new_ref);
        }

        Delete(topology, edge_id);
        printf("Deleted edge %d.\n", edge_id);

        remove_dangling_edges(topology);
    }

    // Recurse until we do a full loop without deleting any edges.
    if (!done)  simplify_topology(topology, ctx);
}

int main()
{
    Memory_context *ctx = new_context(NULL);

    PGconn *db = connect_to_database("postgres://postgres:postgisclarity@osm.tal/gis");

    Topology *topo = load_topology(db, "district_topo3", ctx);

    simplify_topology(topo, NULL);

    char_array *printed = get_topology_printed(topo, ctx);

    write_array_to_file(printed, "topo.txt");

    free_context(ctx);
    return 0;
}
