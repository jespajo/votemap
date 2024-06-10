#ifndef VECTOR_H_INCLUDED
#define VECTOR_H_INCLUDED

typedef struct {float v[2];}  Vector2;
typedef struct {float v[3];}  Vector3;
typedef struct {float v[4];}  Vector4;

Vector2 add_vec2(Vector2 u, Vector2 w);
Vector2 sub_vec2(Vector2 u, Vector2 w);
Vector2 norm_vec2(Vector2 u);
Vector2 scale_vec2(float a, Vector2 u);

#endif // VECTOR_H_INCLUDED
