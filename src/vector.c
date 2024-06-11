#include <math.h>

#include "vector.h"

Vector2 add_vec2(Vector2 u, Vector2 w)
{
    Vector2 v = {u.v[0] + w.v[0], u.v[1] + w.v[1]};
    return v;
}

Vector2 sub_vec2(Vector2 u, Vector2 w)
{
    Vector2 v = {u.v[0] - w.v[0], u.v[1] - w.v[1]};
    return v;
}

Vector2 norm_vec2(Vector2 u)
{
    float length = hypotf(u.v[0], u.v[1]);
    Vector2 v = {u.v[0]/length, u.v[1]/length};
    return v;
}

Vector2 scale_vec2(float a, Vector2 u)
{
    Vector2 v = {a*u.v[0], a*u.v[1]};
    return v;
}

Vector2 rotate_90(Vector2 u)
{
    Vector2 v = {-u.v[1], u.v[0]};
    return v;
}

Vector2 rotate_270(Vector2 u)
{
    Vector2 v = {u.v[1], -u.v[0]};
    return v;
}
