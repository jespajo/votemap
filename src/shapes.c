#include "shapes.h"

bool same_point(Vector2 p, Vector2 q)
{
    if (p.v[0] != q.v[0])  return false;
    if (p.v[1] != q.v[1])  return false;
    return true;
}

bool points_are_clockwise(Vector2 *points, s64 num_points)
// This function assumes that we're working with a TOP-left origin, with Y increasing downwards.
{
    if (num_points < 3) {
        Error("points_are_clockwise() needs at least 3 points. We only have %ld.", num_points);
    }
    float sum = 0;
    for (int i = 0; i < num_points-1; i++) {
        float *p = points[i].v;
        float *q = points[i+1].v;
        sum += (q[0] - p[0])/(q[1] + p[1]);
    }
    float *p = points[0].v;
    float *q = points[num_points-1].v;
    sum += (p[0] - q[0])/(p[1] + q[1]);
    return sum > 0;
}

bool is_polygon(Polygon *polygon)
// See Polygon typedef.
{
    if (polygon->count < 0)  return false; // @Fixme: Should this be <= ?
    for (s64 i = 0; i < polygon->count; i++) {
        Path *ring = &polygon->data[i];
        bool clockwise = points_are_clockwise(ring->data, ring->count);
        if (!i) {
            if (clockwise)   return false;
        } else {
            if (!clockwise)  return false;
        }
    }
    return true;
}

bool is_triangle(Path triangle)
{
    // @Todo: Check the three points aren't colinear.
    if (triangle.count == 3)  return true;
    if (triangle.count == 4) {
        Vector2 first = triangle.data[0];
        Vector2 last  = triangle.data[3];
        if (same_point(first, last))  return true;
    }
    return false;
}

bool point_in_triangle(Vector2 point, Path triangle)
{
    assert(is_triangle(triangle));

    // If the point is the same as one of the triangle's points, it's not considered in the triangle.
    for (int i = 0; i < 3; i++) {
        if (same_point(triangle.data[i], point))  return false;
    }

    float *p  = point.v;
    float *t1 = triangle.data[0].v;
    float *t2 = triangle.data[1].v;
    float *t3 = triangle.data[2].v;

    float d1 = (p[0] - t1[0])*(t2[1] - t1[1]) - (p[1] - t1[1])*(t2[0] - t1[0]);
    float d2 = (p[0] - t2[0])*(t3[1] - t2[1]) - (p[1] - t2[1])*(t3[0] - t2[0]);
    float d3 = (p[0] - t3[0])*(t1[1] - t3[1]) - (p[1] - t3[1])*(t1[0] - t3[0]);

    bool negative = d1 < 0 || d2 < 0 || d3 < 0;
    bool positive = d1 > 0 || d2 > 0 || d3 > 0;

    return !(negative && positive);
}

Path_array *triangulate_polygon(Polygon *polygon, Memory_Context *ctx)
{
    assert(is_polygon(polygon));

    Path_array *triangles = NewArray(triangles, ctx);

    // @Todo: If the polygon has holes, turn it into one big ring.
    // But for now we'll just take the outer ring and ignore holes.
    Path *ring = &polygon->data[0];

    // This is like a circular linked list but all the links are stored in a separate array, and
    // instead of being pointers, they're the indices of the next points in the ring. This makes it
    // easy to iterate over all the points starting from any one: just call `i = next[i]` at the end
    // of each loop until you encouter the current point again. The other advantage is that, as we
    // "chop off ears" in the process of turning polygons into triangles, we just update the
    // preceding index to point to the one after the removed one.
    int *next = New(ring->count, int, ctx);
    for (int i = 0; i < ring->count-1; i++)  next[i] = i + 1;
    next[ring->count-1] = 0; // { 1, 2, 3, ..., 0 }

    int num_triangles = ring->count-2;
    {
        int i1 = 0;
        int i0 = 0;

        while (triangles->count < num_triangles) {
            int v1 = i1;
            int v2 = next[v1];
            int v3 = next[v2];

            bool remove = true;

            Path ear = {.context = ctx}; // @Speed!: Wait... you create this even if you're not going to use it?? Jeez, no wonder this is so slow.

            *Add(&ear) = ring->data[v1];
            *Add(&ear) = ring->data[v2];
            *Add(&ear) = ring->data[v3];
            *Add(&ear) = ring->data[v1]; // Repeat the first point. @Speed, but might be useful for is_triangle verification??

            if (points_are_clockwise(ear.data, 3)) {
                // The ear is not on the outer hull.
                remove = false;
            } else {
                // The ear is on the outer hull. Check all other points to see if any of them is inside this ear.
                int i2 = next[v3];
                while (i2 != v1) {
                    if (point_in_triangle(ring->data[i2], ear)) {
                        remove = false; // There is another point inside the ear.
                        break;
                    }
                    i2 = next[i2];
                }
            }

            if (!remove) {
                i1 = v2;
                // Make sure we don't loop around the ring forever. If there are no triangles yet, we shouldn't be back at the start where i1 == 0.
                assert(triangles->count || i1);
                // If we have partially triangulated, return what we've got. @Todo: Control what to return if triangulation fails with a parameter?
                if (!(!triangles->count || i1 != i0)) {
                    Log("Partial triangulation. Used %d out of %d vertices.", triangles->count, ring->count); // @Fixme.
                    return triangles;
                }
                continue;
            }

            // Off with the ear!

            *Add(triangles) = ear;

            next[v1] = v3;
            i1       = v3;
            i0       = v3;
        }
    }

    assert(triangles->count == num_triangles);
    return triangles;
}
