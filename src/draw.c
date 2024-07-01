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
        //
        // Draw the line AB as a rectangle.
        //
        Vector2 A = path->data[i];
        Vector2 B = path->data[i+1];
        Vector2 C = path->data[i+2];

        Vector2 points[3] = {A, B, C};
        float ABC_clockwise = clockwise_value(points, 3);

        Vector2 AB_offset; {
            Vector2 v = sub_vec2(B, A);
            v = norm_vec2(v);
            v = (ABC_clockwise > 0) ? rotate_90(v) : rotate_270(v);
            AB_offset = scale_vec2(width/2, v);
        }
        Vector2 rect[4]; {
            rect[0] = add_vec2(A, AB_offset);
            rect[1] = sub_vec2(A, AB_offset);
            rect[2] = add_vec2(B, AB_offset);
            rect[3] = sub_vec2(B, AB_offset);
        }

        *Add(out) = (Vertex){rect[0].v[0], rect[0].v[1], r, g, b, a};
        *Add(out) = (Vertex){rect[1].v[0], rect[1].v[1], r, g, b, a};
        *Add(out) = (Vertex){rect[2].v[0], rect[2].v[1], r, g, b, a};

        *Add(out) = (Vertex){rect[1].v[0], rect[1].v[1], r, g, b, a};
        *Add(out) = (Vertex){rect[2].v[0], rect[2].v[1], r, g, b, a};
        *Add(out) = (Vertex){rect[3].v[0], rect[3].v[1], r, g, b, a};

        if (ABC_clockwise == 0)  continue; // Don't draw a mitre if the points ABC are colinear.
        if (i+2 >= path->count)  continue; // Don't draw a mitre if AB is the last line in the path.

        //
        // Draw a mitre at point B.
        //
        Vector2 BC_offset; {
            Vector2 v = sub_vec2(C, B);
            v = norm_vec2(v);
            v = (ABC_clockwise < 0) ? rotate_90(v) : rotate_270(v);
            BC_offset = scale_vec2(width/2, v);
        }
        Vector2 mitre[3]; {
            mitre[0] = sub_vec2(B, AB_offset);
            mitre[1] = add_vec2(B, BC_offset);
            mitre[2] = sub_vec2(B, BC_offset);
        }

        *Add(out) = (Vertex){mitre[0].v[0], mitre[0].v[1], r, g, b, a};
        *Add(out) = (Vertex){mitre[1].v[0], mitre[1].v[1], r, g, b, a};
        *Add(out) = (Vertex){mitre[2].v[0], mitre[2].v[1], r, g, b, a};
    }
}
