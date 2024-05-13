#ifndef SHAPES_H_INCLUDED
#define SHAPES_H_INCLUDED

#include "array.h"

// Later we'll probably move these to vector.h.
typedef struct {float v[2];}  Vector2;
typedef struct {float v[3];}  Vector3;
typedef struct {float v[4];}  Vector4;

// In Path data, we guarantee:
// - the y-axis increases downwards
// - when we represent a closed shape, the first and last points are the same
typedef Array(Vector2)  Path;
typedef Array(Path)     Path_array;

// In Polygon data, we guarantee:
// - the y-axis increases downwards
// - the first ring is the outer ring given counter-clockwise
// - subsequent rings are holes given clockwise
typedef Array(Path)     Polygon;
typedef Array(Polygon)  Polygon_array;

bool same_point(Vector2 p, Vector2 q);
bool points_are_clockwise(Vector2 *points, s64 num_points);
bool is_polygon(Polygon *polygon);
bool is_triangle(Path triangle);
bool point_in_triangle(Vector2 point, Path triangle);
Path_array *triangulate_polygon(Polygon *polygon, Memory_Context *ctx);

#endif // SHAPES_H_INCLUDED
