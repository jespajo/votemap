#include <math.h>

#include "draw.h"

Vertex_array *draw_path(Path *path, float width, Vector4 colour, Memory_Context *context)
{
    Vertex_array *vertices = NewArray(vertices, context);

    float r = colour.v[0];
    float g = colour.v[1];
    float b = colour.v[2];
    float a = colour.v[3];

    for (s64 i = 0; i < path->count-1; i++) {
        float *p = path->data[i].v;
        float *q = path->data[i+1].v;

        float dx = q[0] - p[0];
        float dy = q[1] - p[1];

        // Find the four corners of the rectangle.
        Vector2 p1, p2, q1, q2;
        if (dx == 0) {
            p1 = (Vector2){p[0] - width/2, p[1]};
            p2 = (Vector2){p[0] + width/2, p[1]};
            q1 = (Vector2){q[0] - width/2, q[1]};
            q2 = (Vector2){q[0] + width/2, q[1]};
        } else {
            float slope = dy/dx;
            float theta = atanf(slope);

            float x_offset = (width/2)*sinf(theta);
            float y_offset = (width/2)*cosf(theta);

            p1 = (Vector2){p[0] - x_offset, p[1] + y_offset};
            p2 = (Vector2){p[0] + x_offset, p[1] - y_offset};
            q1 = (Vector2){q[0] - x_offset, q[1] + y_offset};
            q2 = (Vector2){q[0] + x_offset, q[1] - y_offset};
        }

        // Add the vertices.
        *Add(vertices) = (Vertex){p1.v[0], p1.v[1],  r, g, b, a};
        *Add(vertices) = (Vertex){p2.v[0], p2.v[1],  r, g, b, a};
        *Add(vertices) = (Vertex){q1.v[0], q1.v[1],  r, g, b, a};

        *Add(vertices) = (Vertex){q1.v[0], q1.v[1],  r, g, b, a};
        *Add(vertices) = (Vertex){q2.v[0], q2.v[1],  r, g, b, a};
        *Add(vertices) = (Vertex){p2.v[0], p2.v[1],  r, g, b, a};
    }

    return vertices;
}

Vertex_array *draw_polygon(Polygon *polygon, Vector4 colour, Memory_Context *context)
{
    Vertex_array *vertices = NewArray(vertices, context);

    float r = colour.v[0];
    float g = colour.v[1];
    float b = colour.v[2];
    float a = colour.v[3];

    Path_array *triangles = triangulate_polygon(polygon, context);

    for (s64 i = 0; i < triangles->count; i++) {
        Path *triangle = &triangles->data[i];

        for (int j = 0; j < 3; j++) {
            float x = triangle->data[j].v[0];
            float y = triangle->data[j].v[1];

            *Add(vertices) = (Vertex){x, y, r, g, b, a};
        }
    }

    return vertices;
}

void add_verts(Vertex_array *array, Vertex_array *appendage)
{
    for (s64 i = 0; i < appendage->count; i++) {
        *Add(array) = appendage->data[i];
    }
}
