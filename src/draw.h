#ifndef DRAW_H_INCLUDED
#define DRAW_H_INCLUDED

#include "shapes.h"

// This type is closely linked with the vertex shader in web/script.js.
struct Vertex {
    float x, y;
    float r, g, b, a;
};
typedef struct Vertex  Vertex;
typedef Array(Vertex)  Vertex_array;

void draw_polygon(Polygon *polygon, Vector4 colour, Vertex_array *out);
void draw_path(Path *path, float width, Vector4 colour, Vertex_array *out);
Vertex_array *copy_verts_in_the_box(Vertex_array *verts, float min_x, float min_y, float max_x, float max_y, Memory_context *context);

#endif // DRAW_H_INCLUDED
