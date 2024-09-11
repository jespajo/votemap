#ifndef SHAPES_H_INCLUDED
#define SHAPES_H_INCLUDED

#include "array.h"
#include "vector.h"

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
float clockwise_value(Vector2 *points, s64 num_points);
bool points_are_clockwise(Vector2 *points, s64 num_points);
bool points_are_anticlockwise(Vector2 *points, s64 num_points);
Path_array *triangulate_polygon(Polygon *polygon, Memory_context *context);

#endif // SHAPES_H_INCLUDED
