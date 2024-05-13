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

        //
        // @Temporary: Draw lines as diamonds.
        //
        *Add(vertices) = (Vertex){p[0],       p[1],  r, g, b, a};
        *Add(vertices) = (Vertex){q[0],       q[1],  r, g, b, a};
        *Add(vertices) = (Vertex){p[0]+width, p[1],  r, g, b, a};

        *Add(vertices) = (Vertex){q[0],       q[1],  r, g, b, a};
        *Add(vertices) = (Vertex){p[0]+width, p[1],  r, g, b, a};
        *Add(vertices) = (Vertex){q[0]+width, q[1],  r, g, b, a};
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
