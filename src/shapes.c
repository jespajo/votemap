#include "shapes.h"

bool same_point(Vector2 p, Vector2 q)
{
    if (p.v[0] != q.v[0])  return false;
    if (p.v[1] != q.v[1])  return false;
    return true;
}

float clockwise_value(Vector2 *points, s64 num_points)
{
    assert(num_points >= 3);

    float det = 0;
    for (int i = 0; i < num_points; i++) {
        float x1 = points[i].v[0];
        float y1 = points[i].v[1];
        float x2 = points[(i+1) % num_points].v[0];
        float y2 = points[(i+1) % num_points].v[1];

        det += (x2 - x1)/(y1 + y2);
    }
    return det;
}

bool points_are_clockwise(Vector2 *points, s64 num_points)
{
    return clockwise_value(points, num_points) < 0; // (Inverted Y axis.)
}

bool points_are_anticlockwise(Vector2 *points, s64 num_points)
{
    return clockwise_value(points, num_points) > 0;
}

static bool is_polygon(Polygon *polygon)
// See Polygon typedef.
{
    if (polygon->count < 0)  return false; // Should this be <= ? @Todo
    for (s64 i = 0; i < polygon->count; i++) {
        Path *ring = &polygon->data[i];
        if (!i) {
            if (points_are_clockwise(ring->data, ring->count))      return false;
        } else {
            if (points_are_anticlockwise(ring->data, ring->count))  return false;
        }
    }
    return true;
}

static bool point_in_triangle(Vector2 point, Vector2 *triangle)
{
    // If the point is the same as one of the triangle's points, it's not considered "in" the triangle.
    for (int i = 0; i < 3; i++) {
        if (same_point(triangle[i], point))  return false;
    }

    float *p  = point.v;
    float *t1 = triangle[0].v;
    float *t2 = triangle[1].v;
    float *t3 = triangle[2].v;

    float d1 = (p[0] - t1[0])*(t2[1] - t1[1]) - (p[1] - t1[1])*(t2[0] - t1[0]);
    float d2 = (p[0] - t2[0])*(t3[1] - t2[1]) - (p[1] - t2[1])*(t3[0] - t2[0]);
    float d3 = (p[0] - t3[0])*(t1[1] - t3[1]) - (p[1] - t3[1])*(t1[0] - t3[0]);

    bool negative = d1 < 0 || d2 < 0 || d3 < 0;
    bool positive = d1 > 0 || d2 > 0 || d3 > 0;

    return !(negative && positive);
}

Path_array *triangulate_polygon(Polygon *polygon, Memory_Context *context)
{
    assert(is_polygon(polygon));
    Memory_Context *ctx = context;

    Path_array *triangles = NewArray(triangles, ctx);

    // @Todo: If the polygon has holes, turn it into one big ring.
    // But for now we'll just take the outer ring and ignore holes.
    Path *ring = &polygon->data[0];

    //
    // This is like a circular linked list, except the links are indices instead of pointers, and the
    // data and links are stored in separate arrays. So:
    //
    //      Vector2 *point = ring->data[point_index];
    //
    //      int next_point_index = links[point_index];
    //      Vector2  *next_point = ring->data[next_point_index];
    //
    // When we "chop off an ear", we'll update a single link to cut out the removed point.
    //
    int *links = New(ring->count, int, ctx);
    {
        for (int i = 0; i < ring->count-1; i++)  links[i] = i + 1;
        links[ring->count-1] = 0; // { 1, 2, 3, ..., 0 }
    }

    int current_point_index =  0;
    int halt_point_index    = -1; // If >= 0, means "we've tried this point index already"

    int expect_num_triangles = ring->count-2;

    while (triangles->count < expect_num_triangles) {
        int i0 = current_point_index;
        int i1 = links[current_point_index];
        int i2 = links[links[current_point_index]];

        Vector2 tri[3] = {ring->data[i0], ring->data[i1], ring->data[i2]};

        float det = clockwise_value(tri, 3);
        if (det == 0) {
            // The points are colinear. The middle point can be removed without consequence.
            links[i0] = i2;
            expect_num_triangles -= 1;
            continue;
        }
        bool clockwise = det < 0;

        if (!clockwise) {
            // The ear is on the outer hull. Check whether any other point is inside this ear.
            int i = links[i2];

            while (i != i0) {
                if (point_in_triangle(ring->data[i], tri))  break;
                i = links[i];
            }

            if (i == i0) {
                // None of the other points are inside. Off with the ear!
                links[i0] = i2;

                Path *ear = Add(triangles);
                *ear = (Path){.context = ctx};
                for (int j = 0; j < 3; j++)  *Add(ear) = tri[j];

                current_point_index = i2;
                halt_point_index    = -1;

                continue;
            }
        }

        // We won't remove this triangle.

        if (halt_point_index < 0)  halt_point_index = current_point_index;

        current_point_index = i1;
        if (current_point_index != halt_point_index)  continue;

        if (triangles->count == expect_num_triangles-1) {
            // Sometimes the last three points aren't in anticlockwise order for reasons unknown and we end up here.
            // We don't need to fix the triangles because they're on their way to becoming GPU fodder.
            Path *final = Add(triangles);
            *final = (Path){.context = ctx};
            for (int i = 0; i < 3; i++)   *Add(final) = tri[i];
            break;
        }

        Log("Partial triangulation. Created %d/%d triangles.", triangles->count, expect_num_triangles);
        break;
    }

    return triangles;
}
