// GPU data of the reference path tracer, shared by the C++ uploader and the HLSL kernels. Every struct here is read
// through StructuredBuffer, whose layout is the tight C layout (4-byte scalars, no 16-byte vector alignment), so the
// C++ compilation of this header is the upload layout (static_asserts in GpuScene.cpp). No bool or 64-bit members.
#ifndef UNX_RT_TYPES_HLSLI
#define UNX_RT_TYPES_HLSLI
#include "Atmosphere.hlsli"
#include "Lights.hlsli"
#include "Material.hlsli"

RT_BEGIN_NAMESPACE
RT_CONST uint kRtNone = 0xFFFFFFFFu;

// Sun (PathTracer::Impl): cone of half-angle theta_s about dir; halfSin = sin(theta_s / 2), solidAngle =
// 2 pi 2 sin^2(theta_s / 2), sin2 = sin^2(theta_s) (all computed in double on the CPU), radiance at the top of the
// atmosphere.
struct RtSun
{
    float3 dir;
    float halfSin;
    float3 t1;
    float solidAngle;
    float3 t2;
    float sin2;
    float3 radiance;
    float pad;
};

// Uniform in the cone: 1 - cos(theta) = u (1 - cos(theta_s)), i.e. sin(theta / 2) = sqrt(u) sin(theta_s / 2) = x; then
// sin(theta) = 2 x sqrt(1 - x^2) and cos(theta) = 1 - 2 x^2 (the CPU's asin/sin/cos in double, without trigonometry).
RT_INLINE float3 rtSampleSun(RtSun s, float u1, float u2)
{
    const float x = sqrt(u1) * s.halfSin;
    const float sn = 2 * x * sqrt(max(0.0f, 1 - x * x)), cs = 1 - 2 * x * x, phi = 2 * kRtPi * u2;
    return normalize(s.dir * cs + s.t1 * (sn * cos(phi)) + s.t2 * (sn * sin(phi)));
}
RT_INLINE bool rtInSun(RtSun s, float3 d)
{
    const float3 x = cross(d, s.dir);
    return dot(d, s.dir) > 0 && dot(x, x) <= s.sin2;
}

// Pinhole camera (PathTracer.cpp cameraRay): forward, right = normalize(cross(forward, up)), up' = cross(right, forward),
// tanHalfFov = tan(fov / 2), aspect = W / H, all computed on the CPU exactly as the CPU estimator does.
struct RtCamera
{
    float3 position;
    float tanHalfFov;
    float3 forward;
    float aspect;
    float3 right;
    float nearPlane;
    float3 up;
    float exposure;
};

struct RtInstance
{
    float4 row0;  // object -> world 3x4, row-major
    float4 row1;
    float4 row2;
    uint mesh;            // RtMesh record (a deformed instance has its own world-space record)
    uint sourceMesh;      // scene mesh (submesh table)
    uint overrideOffset;  // into materialOverrides, kRtNone = mesh materials
    uint flags;           // bit 0: world-space vertex data (deformed), bit 1: deformed (not rigid)
};
RT_CONST uint kRtInstanceWorld = 1u;
RT_CONST uint kRtInstanceDeformed = 2u;

struct RtMesh
{
    uint vertexOffset;   // into positions / normals / tangents / uvs (element index)
    uint indexOffset;    // into indices (element index)
    uint hasTangents;
    uint hasUv;
};

struct RtSubmesh
{
    uint firstTriangle;  // mesh triangle index of the submesh's first triangle
    uint material;
    uint pad0;
    uint pad1;
};
struct RtMeshSubmeshes
{
    uint first;  // into submeshes
    uint count;
};

struct RtMaterial
{
    float3 baseColor;
    uint cls;
    float3 emissive;
    float roughness;
    float metallic;
    float specular;
    float alphaCutoff;
    float transmission;
    uint twoSided;
    uint baseColorTexture;
    uint normalTexture;
    uint roughMetalTexture;
    uint emissiveTexture;
    uint smooth;  // sun-caustic smooth set (Standard, no roughness texture, alpha <= 0.02)
    uint alphaTested;
    uint pad;
};

struct RtTexture
{
    uint width;
    uint height;
    uint wrap;
    uint texelOffset;  // into the decoded float4 texel buffer
};

// Smooth rigid triangle for the sun-caustic emission (world space), with its CPU triangle identity.
struct RtEmitTriangle
{
    float3 p0;
    uint instance;
    float3 e1;
    uint triIndex;
    float3 e2;
    uint geometry;  // BLAS geometry (submesh) holding the triangle
};

// Descriptor-heap indices of every buffer (ResourceDescriptorHeap[]).
struct RtBindings
{
    uint tlas;
    uint instances;
    uint meshes;
    uint meshSubmeshes;
    uint submeshes;
    uint materialOverrides;
    uint materials;
    uint textures;
    uint texels;
    uint positions;
    uint normals;
    uint tangents;
    uint uvs;
    uint indices;
    uint albedoTable;
    uint atmTable;
    uint lights;
    uint cellStart;
    uint cellLights;
    uint emitTriangles;
    uint emitCdf;      // double per triangle (uint2 bit pairs), running area sum
    uint accum0;       // double3 per pixel, half A
    uint accum1;       // half B
    uint splat0;       // float3 per pixel, sun-caustic splats of the current pass, half A
    uint splat1;
    uint counters;     // RtCounters
    uint pad0;
    uint pad1;
};

struct RtConstants
{
    RtCamera camera;
    RtAtmosphere atm;
    RtSun sun;
    RtLightGrid grid;
    RtBindings b;
    uint width;
    uint height;
    uint seedLo;
    uint seedHi;
    uint rrStart;
    uint forced;
    uint caustics;
    uint emitCount;
    uint orderMin;
    uint orderMax;
    uint surfMin;
    uint surfMax;
    uint emitAreaLo;   // double bits of the smooth set's total area
    uint emitAreaHi;
    uint instanceCount;
    uint pad;
};

// Per-run counters (uint, summed by waves; the CPU reads them after every slice).
RT_CONST uint kRtCounterRays = 0;       // rays traced (all kinds)
RT_CONST uint kRtCounterNans = 1;       // non-finite camera samples (dropped, as on the CPU)
RT_CONST uint kRtCounterTruncated = 2;  // paths that reached kMaxBounces
RT_CONST uint kRtCounterErrors = 3;     // OR of kRtError* bits
RT_CONST uint kRtCounterSplatNans = 4;  // non-finite caustic splats (dropped, as on the CPU)
RT_CONST uint kRtCounterCount = 8;

// Error bits (INTERFACES 3.6: every data-dependent loop has a hard limit; reaching it is an error, never a silent cut).
RT_CONST uint kRtErrorTraversal = 1u;   // RayQuery candidate loop limit
RT_CONST uint kRtErrorLightList = 2u;   // light cell list longer than the light count
RT_CONST uint kRtErrorSplatCas = 4u;    // caustic splat CAS retry limit
RT_CONST uint kRtErrorEmitSearch = 8u;  // emission CDF binary search limit
RT_CONST uint kRtErrorPanels = 16u;     // atmosphere quadrature panel limit
RT_END_NAMESPACE

#endif
