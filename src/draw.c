#include <math.h>

#include "draw.h"

void draw_polygon(Polygon *polygon, Vector3 colour, Vertex_array *out)
{
    Memory_context *ctx = out->context;

    float r = colour.v[0];
    float g = colour.v[1];
    float b = colour.v[2];

    Triangle_array *triangles = triangulate_polygon(polygon, ctx);

    for (s64 i = 0; i < triangles->count; i++) {
        Triangle *triangle = &triangles->data[i];

        for (int j = 0; j < 3; j++) {
            float x = triangle->p[j].v[0];
            float y = triangle->p[j].v[1];

            *Add(out) = (Vertex){x, y, r, g, b};
        }
    }
}

void draw_path(Path *path, float width, Vector3 colour, Vertex_array *out)
{
    float r = colour.v[0];
    float g = colour.v[1];
    float b = colour.v[2];

    for (s64 i = 0; i < path->count-1; i++) {
        //
        // Draw the line AB as a rectangle.
        //
        Vector2 A = path->data[i];
        Vector2 B = path->data[i+1];
        // If AB is the last line in the path, C does not exist. Make it the same as B.
        // That also means ABC will be colinear so we will not draw a mitre below.
        Vector2 C = (i < path->count-2) ? path->data[i+2] : path->data[i+1];

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

        *Add(out) = (Vertex){rect[0].v[0], rect[0].v[1], r, g, b};
        *Add(out) = (Vertex){rect[1].v[0], rect[1].v[1], r, g, b};
        *Add(out) = (Vertex){rect[2].v[0], rect[2].v[1], r, g, b};

        *Add(out) = (Vertex){rect[1].v[0], rect[1].v[1], r, g, b};
        *Add(out) = (Vertex){rect[2].v[0], rect[2].v[1], r, g, b};
        *Add(out) = (Vertex){rect[3].v[0], rect[3].v[1], r, g, b};

        // Don't draw a mitre if the points ABC are colinear.
        if (ABC_clockwise == 0)  continue;

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

        *Add(out) = (Vertex){mitre[0].v[0], mitre[0].v[1], r, g, b};
        *Add(out) = (Vertex){mitre[1].v[0], mitre[1].v[1], r, g, b};
        *Add(out) = (Vertex){mitre[2].v[0], mitre[2].v[1], r, g, b};
    }
}

Vertex_array *copy_verts_in_the_box(Vertex_array *verts, float min_x, float min_y, float max_x, float max_y, Memory_context *context)
// Create a copy of an array of triangle vertices, excluding any without a single point within the given box.
// |Speed: It would be a bit quicker if it operated on triangles instead of verts.
{
    Vertex_array *result = NewArray(result, context);
    array_reserve(result, verts->count);

    assert(verts->count % 3 == 0);

    for (s64 i = 0; i < verts->count; i += 3) {
        Vertex *v = &verts->data[i];

        if (v[0].x < min_x && v[1].x < min_x && v[2].x < min_x)  continue;
        if (v[0].y < min_y && v[1].y < min_y && v[2].y < min_y)  continue;
        if (v[0].x > max_x && v[1].x > max_x && v[2].x > max_x)  continue;
        if (v[0].y > max_y && v[1].y > max_y && v[2].y > max_y)  continue;

        *Add(result) = v[0];
        *Add(result) = v[1];
        *Add(result) = v[2];
    }

    return result;
}
