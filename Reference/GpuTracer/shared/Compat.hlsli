// C++ / HLSL compatibility for the shared reference-estimator code (Reference/GpuTracer/shared). The same source is
// compiled by DXC (HLSL 2021, the GPU reference path tracer) and by the C++ compiler (tests that compare it with the
// CPU estimator and with the GPU). Shared code sticks to a common subset:
//   - free functions only, parameters by value or through RT_OUT(T) / RT_INOUT(T);
//   - vector types float2/float3/float4/uint2 built with float3(a, b, c) and rtSplat3(x), components .x .y .z .w only
//     (no swizzles), no vector comparisons;
//   - uint, int, uint64_t, float; static const constants; the intrinsics declared below.
// Data the shared code reads (tables, lights) comes through functions the includer defines before including.
#ifndef UNX_RT_COMPAT_HLSLI
#define UNX_RT_COMPAT_HLSLI

#ifdef __HLSL_VERSION
#define RT_OUT(T) out T
#define RT_INOUT(T) inout T
#define RT_INLINE
#define RT_CONST static const
#define RT_BEGIN_NAMESPACE
#define RT_END_NAMESPACE
#define RT_LOOP [loop]
float3 rtSplat3(float v) { return float3(v, v, v); }
float rtAsFloat(uint v) { return asfloat(v); }
uint rtAsUint(float v) { return asuint(v); }
int rtAsInt(float v) { return asint(v); }
float rtIntAsFloat(int v) { return asfloat(v); }
bool rtIsFinite(float v) { return !isinf(v) && !isnan(v); }
#else
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#define RT_OUT(T) T&
#define RT_INOUT(T) T&
#define RT_INLINE inline
#define RT_CONST inline constexpr
#define RT_BEGIN_NAMESPACE namespace unx::reference::shared {
#define RT_END_NAMESPACE }
#define RT_LOOP

namespace unx::reference::shared
{
using uint = uint32_t;

struct float2
{
    float x = 0, y = 0;
    float2() = default;
    float2(float a, float b) : x(a), y(b) {}
};
struct float3
{
    float x = 0, y = 0, z = 0;
    float3() = default;
    float3(float a, float b, float c) : x(a), y(b), z(c) {}
};
struct float4
{
    float x = 0, y = 0, z = 0, w = 0;
    float4() = default;
    float4(float a, float b, float c, float d) : x(a), y(b), z(c), w(d) {}
};
struct uint2
{
    uint x = 0, y = 0;
    uint2() = default;
    uint2(uint a, uint b) : x(a), y(b) {}
};

inline float3 operator+(float3 a, float3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline float3 operator-(float3 a, float3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline float3 operator*(float3 a, float3 b) { return { a.x * b.x, a.y * b.y, a.z * b.z }; }
inline float3 operator/(float3 a, float3 b) { return { a.x / b.x, a.y / b.y, a.z / b.z }; }
inline float3 operator*(float3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
inline float3 operator*(float s, float3 a) { return { a.x * s, a.y * s, a.z * s }; }
inline float3 operator/(float3 a, float s) { return { a.x / s, a.y / s, a.z / s }; }
inline float3 operator-(float3 a) { return { -a.x, -a.y, -a.z }; }
inline float3& operator+=(float3& a, float3 b) { a = a + b; return a; }
inline float3& operator-=(float3& a, float3 b) { a = a - b; return a; }
inline float3& operator*=(float3& a, float3 b) { a = a * b; return a; }
inline float3& operator*=(float3& a, float s) { a = a * s; return a; }
inline float2 operator+(float2 a, float2 b) { return { a.x + b.x, a.y + b.y }; }
inline float2 operator-(float2 a, float2 b) { return { a.x - b.x, a.y - b.y }; }
inline float2 operator*(float2 a, float s) { return { a.x * s, a.y * s }; }

inline float3 rtSplat3(float v) { return { v, v, v }; }
inline float dot(float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float3 cross(float3 a, float3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
inline float length(float3 a) { return std::sqrt(dot(a, a)); }
inline float3 normalize(float3 a) { return a / length(a); }
inline float saturate(float x) { return std::clamp(x, 0.0f, 1.0f); }
inline float clamp(float x, float a, float b) { return std::clamp(x, a, b); }
inline int clamp(int x, int a, int b) { return std::clamp(x, a, b); }
inline uint clamp(uint x, uint a, uint b) { return std::clamp(x, a, b); }
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
inline float rsqrt(float x) { return 1.0f / std::sqrt(x); }
using std::abs;
using std::acos;
using std::asin;
using std::ceil;
using std::cos;
using std::exp;
using std::floor;
using std::log;
using std::max;
using std::min;
using std::pow;
using std::sin;
using std::sqrt;
inline float rtAsFloat(uint v) { float f; std::memcpy(&f, &v, 4); return f; }
inline uint rtAsUint(float v) { uint u; std::memcpy(&u, &v, 4); return u; }
inline int rtAsInt(float v) { int i; std::memcpy(&i, &v, 4); return i; }
inline float rtIntAsFloat(int v) { float f; std::memcpy(&f, &v, 4); return f; }
inline bool rtIsFinite(float v) { return std::isfinite(v); }
} // namespace unx::reference::shared
#endif

RT_BEGIN_NAMESPACE
RT_CONST float kRtPi = 3.14159265358979323846f;

RT_INLINE float rtMax3(float3 v) { return max(v.x, max(v.y, v.z)); }
RT_INLINE float rtAvg3(float3 v) { return (v.x + v.y + v.z) * (1.0f / 3.0f); }
RT_INLINE float rtLuminance(float3 v) { return 0.2126f * v.x + 0.7152f * v.y + 0.0722f * v.z; }
RT_INLINE bool rtIsZero3(float3 v) { return v.x == 0 && v.y == 0 && v.z == 0; }
RT_INLINE bool rtFinite3(float3 v) { return rtIsFinite(v.x) && rtIsFinite(v.y) && rtIsFinite(v.z); }
RT_INLINE float3 rtExpNeg3(float3 t) { return float3(exp(-t.x), exp(-t.y), exp(-t.z)); }
RT_INLINE float3 rtMax3v(float3 a, float3 b) { return float3(max(a.x, b.x), max(a.y, b.y), max(a.z, b.z)); }
RT_INLINE float3 rtMin3v(float3 a, float3 b) { return float3(min(a.x, b.x), min(a.y, b.y), min(a.z, b.z)); }

// log(1 + x) and exp(x) - 1 without cancellation (Kahan / Higham): the CPU estimator uses std::log1p / std::expm1 in
// double; these float forms keep full float relative precision for small |x|.
RT_INLINE float rtLog1p(float x)
{
    const float u = 1.0f + x;
    if (u == 1.0f) return x;
    return log(u) * (x / (u - 1.0f));
}
RT_INLINE float rtExpm1(float x)
{
    if (abs(x) < 1e-5f) return x + 0.5f * x * x;
    const float u = exp(x);
    if (u == 1.0f) return x;
    const float um1 = u - 1.0f;
    if (um1 == -1.0f) return -1.0f;
    return um1 * (x / log(u));
}

// Orthonormal frame with the CPU estimator's reference-axis choice (the same tangent for the same normal).
RT_INLINE void rtOrthonormal(float3 n, RT_OUT(float3) t1, RT_OUT(float3) t2)
{
    const float3 ref = abs(n.y) < 0.99f ? float3(0, 1, 0) : float3(1, 0, 0);
    t1 = normalize(cross(ref, n));
    t2 = cross(n, t1);
}
RT_END_NAMESPACE

#endif
