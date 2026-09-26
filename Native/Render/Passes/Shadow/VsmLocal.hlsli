// Local-light virtual shadow maps (ARCHITECTURE 2.4 "그림자 128"): layout and geometry shared by S's kernels and the
// public lookups. Owner: S.
//
// Each shadowed local light (at most VSM_LOCAL_LIGHTS; S assigns shadow slots, VsmSystem) owns a cube of 6 faces around
// its position; each face is a mip chain of VSM_LOCAL_MIPS virtual resolutions 128 x 2^m texels (m = 0 .. 6: 128 ..
// 8192), cut into 128^2-texel pages like the sun's. Their page-table slots follow the sun's:
//   slot = VSM_SUN_SLOTS + (shadowSlot x 6 + face) x VSM_LOCAL_FACE_SLOTS + mipBase(m) + y 2^m + x,  mipBase(m) = (4^m - 1) / 3
// and share the physical pool, the metadata and the block hierarchy. A slot's tag is its light's generation (a shadow slot
// handed to another light, or a moved light, invalidates its pages).
//
// Face coordinates: for face f with basis (right, up, axis), a point at d = p - light position has depth z = dot(d, axis)
// and tangent coordinates (x, y) = (dot(d, right), dot(d, up)) / z in [-1, 1]. Texel (tx, ty) of mip m covers
// x in [-1 + 2 tx / res, ...], y from +1 downwards (row 0 on top), res = 128 x 2^m. Stored in the atlas: the reversed-Z
// face depth d = n (f - z) / ((f - n) z) of the nearest caster (hardware depth test; 0 = no caster); lookups use the key
// vsmEncode(-z) (vsmLocalKeyOfDepth).
//
// Penumbra geometry (a light of radius r_L: sphere, disk, or the half-diagonal of a rect / the half-length of a tube,
// seen from the receiver as a disk): a caster point at depth z_b occludes part of the light for a receiver at depth z_r
// exactly when its tangent coordinates lie within r_L (1 / z_b - 1 / z_r) of the receiver's, the local analogue of the
// sun's d tan(theta_s) (VsmSample.hlsli); the visibility estimator has the same structure (blocker search, disk filter).
#ifndef UNX_VSM_LOCAL_HLSLI
#define UNX_VSM_LOCAL_HLSLI
#include "Passes/Shadow/VsmCommon.hlsli"

#define VSM_LOCAL_LIGHTS 128u
#define VSM_LOCAL_MIPS 7u
#define VSM_LOCAL_FACE_SLOTS 5461u  // sum over m < 7 of 4^m
#define VSM_LOCAL_LIGHT_SLOTS (6u * VSM_LOCAL_FACE_SLOTS)
#define VSM_LOCAL_SLOTS (VSM_LOCAL_LIGHTS * VSM_LOCAL_LIGHT_SLOTS)
#define VSM_TOTAL_SLOTS (VSM_SUN_SLOTS + VSM_LOCAL_SLOTS)
#define VSM_LOCAL_NONE 0xFFFFu

// Per shadow slot (48 B): the light as rendered this frame.
struct VsmLocalLight
{
    float3 position;
    float nearM;       // near plane of the faces (the emitter itself does not shadow)
    float farM;        // range + extent
    float radius;      // r_L (see above)
    uint lightIndex;   // scene light
    uint generation;   // page tag: changes when the slot's light changes or moves
    float3 pad;
    uint active;       // 1 when the slot holds a light this frame
};

uint vsmLocalMipBase(uint m) { return ((1u << (2 * m)) - 1) / 3; }
// Raster tile masks of the local views (VsmLocalCullMask, VsmSystem.cpp): mip m's view has 4^m tile bits in
// max(1, 4^m / 32) words; views packed mip after mip (offsets 0, 1, 2, 3, 5, 13, 45), face after face, light after light.
#define VSM_LOCAL_FACE_WORDS 173u
#define VSM_LOCAL_LIGHT_WORDS (6u * VSM_LOCAL_FACE_WORDS)
uint vsmLocalViewWordOffset(uint m) { return m == 0 ? 0u : m == 1 ? 1u : m == 2 ? 2u : m == 3 ? 3u : m == 4 ? 5u : m == 5 ? 13u : 45u; }
uint vsmLocalRes(uint m) { return VSM_PAGE << m; }
uint vsmLocalSlot(uint light, uint face, uint mip, uint2 page)
{
    return VSM_SUN_SLOTS + (light * 6 + face) * VSM_LOCAL_FACE_SLOTS + vsmLocalMipBase(mip) + page.y * (1u << mip) + page.x;
}
// Inverse: slot (>= VSM_SUN_SLOTS) -> light, face, mip, page.
void vsmLocalSlotParts(uint slot, out uint light, out uint face, out uint mip, out uint2 page)
{
    const uint s = slot - VSM_SUN_SLOTS;
    light = s / VSM_LOCAL_LIGHT_SLOTS;
    const uint f = s % VSM_LOCAL_LIGHT_SLOTS;
    face = f / VSM_LOCAL_FACE_SLOTS;
    uint r = f % VSM_LOCAL_FACE_SLOTS;
    mip = 0;
    [unroll] for (uint m = 1; m < VSM_LOCAL_MIPS; ++m)
        if (r >= vsmLocalMipBase(m)) mip = m;
    r -= vsmLocalMipBase(mip);
    page = uint2(r % (1u << mip), r / (1u << mip));
}

// Cube face bases (right, up, axis): +x, -x, +y, -y, +z, -z.
void vsmCubeBasis(uint face, out float3 right, out float3 up, out float3 axis)
{
    const float3 axes[6] = { float3(1, 0, 0), float3(-1, 0, 0), float3(0, 1, 0), float3(0, -1, 0), float3(0, 0, 1), float3(0, 0, -1) };
    const float3 ups[6] = { float3(0, 1, 0), float3(0, 1, 0), float3(0, 0, -1), float3(0, 0, 1), float3(0, 1, 0), float3(0, 1, 0) };
    axis = axes[face];
    up = ups[face];
    right = cross(up, axis);
}
uint vsmCubeFace(float3 d)
{
    const float3 a = abs(d);
    if (a.x >= a.y && a.x >= a.z) return d.x >= 0 ? 0u : 1u;
    if (a.y >= a.z) return d.y >= 0 ? 2u : 3u;
    return d.z >= 0 ? 4u : 5u;
}
// Face, tangent coordinates and depth of a world point.
struct VsmLocalPoint
{
    uint face;
    float2 xy;   // tangent coordinates in [-1, 1]
    float z;     // depth along the face axis
};
VsmLocalPoint vsmLocalProject(VsmLocalLight l, float3 world)
{
    const float3 d = world - l.position;
    VsmLocalPoint q;
    q.face = vsmCubeFace(d);
    float3 right, up, axis;
    vsmCubeBasis(q.face, right, up, axis);
    q.z = max(dot(d, axis), 1e-6);
    q.xy = float2(dot(d, right), dot(d, up)) / q.z;
    return q;
}
// Texel coordinates (continuous) of tangent coordinates at mip m.
float2 vsmLocalTexel(float2 xy, uint m) { return float2(xy.x * 0.5 + 0.5, 0.5 - xy.y * 0.5) * vsmLocalRes(m); }
// Mip whose texel is not larger than 'footprint' metres at depth z (a texel spans 2 z / res at the face centre).
uint vsmLocalMip(float footprint, float z)
{
    const float res = 2 * z / max(footprint, 1e-6);
    return (uint)clamp(ceil(log2(max(res, 1.0) / VSM_PAGE)), 0.0, float(VSM_LOCAL_MIPS - 1));
}

#endif
