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

Vertex_array *draw_path(Path *path, float width, Vector4 colour, Memory_Context *context);
Vertex_array *draw_polygon(Polygon *polygon, Vector4 colour, Memory_Context *context);

#endif // DRAW_H_INCLUDED
