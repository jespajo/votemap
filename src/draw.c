#include <math.h>

#include "draw.h"

void draw_polygon(Polygon *polygon, Vector4 colour, Vertex_array *out)
{
    Memory_Context *ctx = out->context;

    float r = colour.v[0];
    float g = colour.v[1];
    float b = colour.v[2];
    float a = colour.v[3];

    Path_array *triangles = triangulate_polygon(polygon, ctx);

    for (s64 i = 0; i < triangles->count; i++) {
        Path *triangle = &triangles->data[i];

        for (int j = 0; j < 3; j++) {
            float x = triangle->data[j].v[0];
            float y = triangle->data[j].v[1];

            *Add(out) = (Vertex){x, y, r, g, b, a};
        }
    }
}

void draw_path(Path *path, float width, Vector4 colour, Vertex_array *out)
{
    float r = colour.v[0];
    float g = colour.v[1];
    float b = colour.v[2];
    float a = colour.v[3];

    for (s64 i = 0; i < path->count-1; i++) {
        // Points (from the data):
        Vector2 B = path->data[i];
        Vector2 C = path->data[i+1];

        // Vectors:
        Vector2 BC      = sub_vec2(B, C);
        Vector2 BC_norm = norm_vec2(BC);
        Vector2 rotated = {-BC_norm.v[1], BC_norm.v[0]}; // Rotate 90 degrees.
        Vector2 scaled  = scale_vec2(width/2, rotated);

        // Points (corners of a rectangle):
        Vector2 p1 = add_vec2(B, scaled);
        Vector2 p2 = sub_vec2(B, scaled);
        Vector2 q1 = add_vec2(C, scaled);
        Vector2 q2 = sub_vec2(C, scaled);

        // Vertices:
        *Add(out) = (Vertex){p1.v[0], p1.v[1], r, g, b, a};
        *Add(out) = (Vertex){p2.v[0], p2.v[1], r, g, b, a};
        *Add(out) = (Vertex){q1.v[0], q1.v[1], r, g, b, a};

        *Add(out) = (Vertex){p2.v[0], p2.v[1], r, g, b, a};
        *Add(out) = (Vertex){q1.v[0], q1.v[1], r, g, b, a};
        *Add(out) = (Vertex){q2.v[0], q2.v[1], r, g, b, a};
    }
}
