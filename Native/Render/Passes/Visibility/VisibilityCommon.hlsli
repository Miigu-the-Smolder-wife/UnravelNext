// V internal: cull views, cull state layout, culling math (frustum, LOD, HiZ occlusion, bands). Owner: V.
// C++ mirror: Passes/Visibility/VisibilityInternal.h (sizes and word offsets checked there).
#ifndef UNX_VISIBILITY_COMMON_HLSLI
#define UNX_VISIBILITY_COMMON_HLSLI
#include "Bindless.hlsli"
#include "Deformation.hlsli"
#include "Scene.hlsli"
#include "Passes/Visibility/ClusterHierarchy.hlsli"

// One view of a cull run (main view: one; depth raster service: one per RasterView). 320 B.
struct CullView
{
    row_major float4x4 viewProj;
    row_major float4x4 prevViewProj;  // phase 1 occlusion against the previous frame's HiZ
    float4 planes[6];                  // world frustum planes, normalised (keep dot(n, p) + w >= 0)
    float4 clipPlane;                  // world clip plane (keep >= 0); all zero = none
    float3 position;                   // camera position (perspective)
    float lodScale;                    // perspective: pixels per metre at distance 1; orthographic: pixels per metre
    float lodThreshold;                // pixels
    uint orthographic;
    float nearPlane;
    uint flags;                        // CULL_VIEW_*
    float2 viewportSize;               // pixels
    float2 viewportOffset;
    float4 viewDirection;              // orthographic: world direction the view looks along (xyz)
    uint cullMaskOffset;               // tile mask words of this view (UNX_NONE = no mask)
    uint userData;
    uint tilesX;                       // ceil(viewport width / tilePx)
    uint tilePx;
};

#define CULL_VIEW_OCCLUSION 1u   // HiZ occlusion (two-phase) for this view
#define CULL_VIEW_CULL_BACK 2u   // back faces of one-sided materials are culled (cone test allowed)

// Cull state words (RWByteAddressBuffer, 4 B each).
#define VS_NODE_WRITE 0u
#define VS_NODE_BEGIN 1u
#define VS_NODE_END 2u
#define VS_GROUP_WRITE 3u
#define VS_GROUP_BEGIN 4u
#define VS_VISIBLE 5u
#define VS_DEFER_INSTANCES 6u
#define VS_DEFER_NODES 7u
#define VS_DEFER_CLUSTERS 8u
#define VS_LIST_COUNT 9u      // + list (VS_LISTS lists): entries appended so far (both phases)
#define VS_LIST_PHASE1 15u    // + list: entries of phase 1 (snapshot)
#define VS_OVERFLOW 21u       // bits: capacity exceeded (1 nodes, 2 groups, 4 visible, 8 deferred instances, 16 deferred nodes, 32 deferred clusters)
#define VS_STAT_INSTANCES 22u // instances that reached the node pass
#define VS_STAT_NODES 23u     // node items processed
#define VS_STAT_CLUSTERS 24u  // clusters tested
#define VS_STAT_TRIANGLES 25u // + band (3): triangles of visible clusters per band A, B, C
#define VS_GROUP_END 28u      // group items of the current cluster pass: [VS_GROUP_BEGIN, VS_GROUP_END)
#define VS_WORDS 32u

#define VS_LISTS 6u
#define LIST_A_BACK 0u        // band A, opaque, back faces culled
#define LIST_A_NONE 1u        // band A, opaque, two-sided (or cull none requested)
#define LIST_A_ALPHA_BACK 2u  // band A, alpha tested
#define LIST_A_ALPHA_NONE 3u
#define LIST_B 4u             // coverage layer
#define LIST_C 5u             // aggregate bricks

// Indirect argument words (3 per dispatch).
#define VA_NODES 0u
#define VA_GROUPS 3u
#define VA_DEFERRED_CLUSTERS 6u
#define VA_DEFERRED_INSTANCES 9u
#define VA_SEED_NODES 12u
#define VA_MESH 15u           // + 3 * list
#define VA_WORDS 33u

uint packItem(uint index, uint view) { return index | (view << 24); }
uint itemIndex(uint packed) { return packed & 0xFFFFFFu; }
uint itemView(uint packed) { return packed >> 24; }

float instanceScale(GpuInstance inst) { return length(inst.objectToWorld[0].xyz); }

// Object-space sphere -> world sphere, inflated by the wind displacement bound.
float4 worldSphere(GpuInstance inst, float4 rows[3], float4 objectSphere)
{
    const float scale = instanceScale(inst);
    const float wind = windOffsetBound(inst, objectSphere.xyz, objectSphere.w);
    return float4(transformPoint(rows, objectSphere.xyz), (objectSphere.w + wind) * scale);
}

bool frustumVisible(CullView v, float4 s)
{
    [unroll] for (uint i = 0; i < 6; ++i)
        if (dot(v.planes[i].xyz, s.xyz) + v.planes[i].w < -s.w) return false;
    if (any(v.clipPlane != 0) && dot(v.clipPlane.xyz, s.xyz) + v.clipPlane.w < -s.w) return false;
    return true;
}

// Screen-space error in pixels of an object-space error measured on a world sphere.
float projectedError(CullView v, float4 s, float worldError)
{
    if (worldError >= 3.0e38) return 3.0e38;
    if (v.orthographic) return worldError * v.lodScale;
    const float d = max(length(s.xyz - v.position) - s.w, v.nearPlane);
    return worldError * v.lodScale / d;
}

// Distance-free projected size in pixels of a world length at the sphere (for bands).
float projectedLength(CullView v, float4 s, float worldLength)
{
    if (v.orthographic) return worldLength * v.lodScale;
    const float d = max(length(s.xyz - v.position) - s.w, v.nearPlane);
    return worldLength * v.lodScale / d;
}

// Screen rectangle (pixels, inclusive) and nearest device depth of a world sphere under viewProj; false when the
// sphere's box reaches the near plane (then it can never be occluded).
bool projectSphere(row_major float4x4 viewProj, float2 viewportSize, float4 s, out float4 rect, out float nearestDepth)
{
    float2 lo = 1e30, hi = -1e30;
    nearestDepth = 0;
    rect = 0;
    [unroll] for (uint k = 0; k < 8; ++k)
    {
        const float3 corner = s.xyz + s.w * float3((k & 1) ? 1 : -1, (k & 2) ? 1 : -1, (k & 4) ? 1 : -1);
        const float4 clip = mul(viewProj, float4(corner, 1));
        if (clip.w <= 1e-6 || clip.z > clip.w) return false;  // behind the camera or in front of the near plane
        const float3 ndc = clip.xyz / clip.w;
        lo = min(lo, ndc.xy);
        hi = max(hi, ndc.xy);
        nearestDepth = max(nearestDepth, ndc.z);  // reversed Z: nearer = larger
    }
    // NDC -> pixels (y down).
    rect = float4((lo.x * 0.5 + 0.5) * viewportSize.x, (0.5 - hi.y * 0.5) * viewportSize.y, (hi.x * 0.5 + 0.5) * viewportSize.x, (0.5 - lo.y * 0.5) * viewportSize.y);
    return true;
}

// HiZ: texel (i, j) of mip m holds the farthest (minimum reversed-Z) depth of pixels [i 2^(m+1), (i+1) 2^(m+1)) x ...
bool hizOccluded(uint hizSrv, uint hizMips, uint2 hizSize, row_major float4x4 viewProj, float2 viewportSize, float4 s)
{
    float4 rect;
    float nearest;
    if (!projectSphere(viewProj, viewportSize, s, rect, nearest)) return false;
    rect = clamp(rect, 0, float4(viewportSize, viewportSize) - 1);
    if (rect.z < rect.x || rect.w < rect.y) return false;
    // Level where the rectangle spans at most 2 x 2 texels: texels are 2^(m+1) pixels.
    // A segment no longer than the texel size touches at most two texels: 2^(m+1) >= extent.
    const float extent = max(rect.z - rect.x, rect.w - rect.y);
    const uint mip = min((uint)max(0.0, ceil(log2(max(extent, 1.0))) - 1.0), hizMips - 1);
    const uint shift = mip + 1;
    const uint2 a = uint2(rect.xy) >> shift, b = uint2(rect.zw) >> shift;
    const uint2 size = (hizSize + (1u << mip) - 1) >> mip;  // mip sizes round up (HiZ.hlsl)
    Texture2D<float> hiz = ResourceDescriptorHeap[hizSrv];
    float farthest = 1;
    [unroll] for (uint k = 0; k < 4; ++k)
    {
        const uint2 p = min(uint2((k & 1) ? b.x : a.x, (k & 2) ? b.y : a.y), size - 1);
        farthest = min(farthest, hiz.Load(int3(p, mip)));
    }
    return nearest < farthest;
}

// Tile mask of a raster-service view: true when the sphere's viewport rectangle covers a set bit (or no mask).
bool tileMaskCovered(CullView v, uint maskSrv, float4 s)
{
    if (v.cullMaskOffset == UNX_NONE) return true;
    float4 rect;
    float nearest;
    if (!projectSphere(v.viewProj, v.viewportSize, s, rect, nearest)) return true;
    rect = clamp(rect, 0, float4(v.viewportSize, v.viewportSize) - 1);
    if (rect.z < rect.x || rect.w < rect.y) return false;
    const uint tilesY = ((uint)v.viewportSize.y + v.tilePx - 1) / v.tilePx;
    const uint2 a = uint2(rect.xy) / v.tilePx, b = min(uint2(rect.zw) / v.tilePx, uint2(v.tilesX, tilesY) - 1);
    // Performance filter only: a rectangle over more than 64 tiles is drawn without looking (conservative).
    if ((b.x - a.x + 1) * (b.y - a.y + 1) > 64) return true;
    ByteAddressBuffer mask = ResourceDescriptorHeap[maskSrv];
    for (uint y = a.y; y <= b.y; ++y)
        for (uint x = a.x; x <= b.x; ++x)
        {
            const uint bit = y * v.tilesX + x;
            if (mask.Load(4 * (v.cullMaskOffset + (bit >> 5))) & (1u << (bit & 31))) return true;
        }
    return false;
}

#endif
