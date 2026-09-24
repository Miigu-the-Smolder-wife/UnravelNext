#pragma once
// Shared math types (INTERFACES_KO.md 6.1). Conventions, fixed for every track:
//   world: right-handed, +Y up, metres. View space: camera looks down -Z, +Y up.
//   Matrices are stored row-major (m[row][col]) and transform column vectors: p' = M * p. HLSL declares them
//   row_major and uses mul(M, v). Affine transforms are float3x4 (the implicit last row is 0 0 0 1).
//   Projection: reversed Z, infinite far plane: depth 1 at the near plane, 0 at infinity (clear depth = 0,
//   depth test GREATER_EQUAL).
#include <cmath>
#include <cstdint>

namespace unx
{
struct float2
{
    float x = 0, y = 0;
};
struct float3
{
    float x = 0, y = 0, z = 0;
};
struct float4
{
    float x = 0, y = 0, z = 0, w = 0;
};
struct uint2
{
    uint32_t x = 0, y = 0;
};
struct uint4
{
    uint32_t x = 0, y = 0, z = 0, w = 0;
};

inline float3 operator+(float3 a, float3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline float3 operator-(float3 a, float3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline float3 operator*(float3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
inline float3 operator*(float s, float3 a) { return a * s; }
inline float3 operator*(float3 a, float3 b) { return { a.x * b.x, a.y * b.y, a.z * b.z }; }
inline float3 operator/(float3 a, float s) { return { a.x / s, a.y / s, a.z / s }; }
inline float3 operator-(float3 a) { return { -a.x, -a.y, -a.z }; }
inline float dot(float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float3 cross(float3 a, float3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
inline float length(float3 a) { return std::sqrt(dot(a, a)); }
inline float3 normalize(float3 a) { return a / length(a); }

// Affine object-to-world transform: rows are (x' y' z') = m[r][0..2] * p + m[r][3].
struct float3x4
{
    float m[3][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 } };
    float3 transformPoint(float3 p) const
    {
        return { m[0][0] * p.x + m[0][1] * p.y + m[0][2] * p.z + m[0][3], m[1][0] * p.x + m[1][1] * p.y + m[1][2] * p.z + m[1][3],
                 m[2][0] * p.x + m[2][1] * p.y + m[2][2] * p.z + m[2][3] };
    }
    float3 transformVector(float3 v) const
    {
        return { m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z, m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z, m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z };
    }
    static float3x4 translation(float3 t)
    {
        float3x4 r;
        r.m[0][3] = t.x;
        r.m[1][3] = t.y;
        r.m[2][3] = t.z;
        return r;
    }
};

struct float4x4
{
    float m[4][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } };
};

inline float4x4 mul(const float4x4& a, const float4x4& b)
{
    float4x4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
        {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a.m[i][k] * b.m[k][j];
            r.m[i][j] = s;
        }
    return r;
}

// General 4x4 inverse (cofactor expansion); returns identity for singular input.
inline float4x4 inverse(const float4x4& a)
{
    const float* m = &a.m[0][0];
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    const float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    float4x4 r;
    if (det == 0) return r;
    for (int i = 0; i < 16; ++i) (&r.m[0][0])[i] = inv[i] / det;
    return r;
}

// View matrix for a camera at 'eye' looking along 'forward' (unit) with 'up' (world): camera looks down -Z.
inline float4x4 lookTo(float3 eye, float3 forward, float3 up)
{
    float3 z = normalize(-forward);
    float3 x = normalize(cross(up, z));
    float3 y = cross(z, x);
    float4x4 v;
    v.m[0][0] = x.x; v.m[0][1] = x.y; v.m[0][2] = x.z; v.m[0][3] = -dot(x, eye);
    v.m[1][0] = y.x; v.m[1][1] = y.y; v.m[1][2] = y.z; v.m[1][3] = -dot(y, eye);
    v.m[2][0] = z.x; v.m[2][1] = z.y; v.m[2][2] = z.z; v.m[2][3] = -dot(z, eye);
    return v;
}

// Reversed-Z infinite perspective: clip.z = near, clip.w = -z_view, so depth = near / -z_view (1 at the near plane).
inline float4x4 perspectiveReversedInfinite(float verticalFovRadians, float aspect, float nearPlane)
{
    const float f = 1.0f / std::tan(verticalFovRadians * 0.5f);
    float4x4 p{};
    p.m[0][0] = f / aspect; p.m[0][1] = 0; p.m[0][2] = 0; p.m[0][3] = 0;
    p.m[1][0] = 0; p.m[1][1] = f; p.m[1][2] = 0; p.m[1][3] = 0;
    p.m[2][0] = 0; p.m[2][1] = 0; p.m[2][2] = 0; p.m[2][3] = nearPlane;
    p.m[3][0] = 0; p.m[3][1] = 0; p.m[3][2] = -1; p.m[3][3] = 0;
    return p;
}
} // namespace unx
