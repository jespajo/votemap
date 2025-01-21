#ifndef SHAPES_H_INCLUDED
#define SHAPES_H_INCLUDED

#include "array.h"
#include "vector.h"

// In Path data, we assume:
// - The Y-axis increases downwards.
// - When we represent a closed shape, the first and last points are the same.
typedef Array(Vector2)  Path;
typedef Array(Path)     Path_array;

// In Polygon data, we assume:
// - The Y-axis increases downwards.
// - The first ring is the outer ring, given counter-clockwise.
// - Subsequent rings are holes given clockwise.
typedef Array(Path)     Polygon;
typedef Array(Polygon)  Polygon_array;

// Triangles are three points. The order of points is not significant.
typedef struct Triangle Triangle;
struct Triangle {Vector2 p[3];};
typedef Array(Triangle)  Triangle_array;

bool same_point(Vector2 p, Vector2 q);
float clockwise_value(Vector2 *points, s64 num_points);
bool points_are_clockwise(Vector2 *points, s64 num_points);
bool points_are_anticlockwise(Vector2 *points, s64 num_points);
Triangle_array *triangulate_polygon(Polygon *polygon, Memory_context *context);
void parse_wkb_polygons(u8 *data, Polygon_array *result, u8 **end_data);
void parse_wkb_paths(u8 *data, Path_array *result, u8 **end_data);

#endif // SHAPES_H_INCLUDED
