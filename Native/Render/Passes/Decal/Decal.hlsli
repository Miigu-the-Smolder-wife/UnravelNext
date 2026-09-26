// Projected decals (FEATURES_GAME 5; A7). Owner: E. Readers: M's material resolve (and its coverage fragments), R's hit
// shading, through decalApply.
//
// A decal is an oriented box and an ordinary scene material (its textures go through M's pipeline: footprint-filtered
// base colour with alpha = opacity, rough/metal, LEAN slope moments for the normal). The box is world space, or the
// object space of one instance (then it moves with the instance and paints only that instance). Surfaces inside the
// box whose geometric normal faces its +Z axis get the material as an upper layer: every parameter blends towards
// the decal's by a = texture alpha x opacity x angle fade x edge fade (FEATURES_GAME 5.2: the pixel material is
// modified in material resolve, before the G-buffer; the band-limited roughness then filters the result).
// Composition order: priority, then creation order (older first, a newer decal on top) - independent of list order.
//
// Per frame (Passes/Decal/Decals.cpp): DecalFrame records (camera-relative) and the 16 x 16 px tile lists, at most
// DECAL_PER_TILE decals per tile (more: counted, status bit DECAL_STATUS_TILE_FULL; FEATURES_GAME 5.2's bound).
#ifndef UNX_DECAL_HLSLI
#define UNX_DECAL_HLSLI
#include "Bindless.hlsli"
#include "Frame.hlsli"
#include "Scene.hlsli"
#include "Passes/Material/MaterialInternal.hlsli"

#define DECAL_NONE 0xFFFFFFFFu
#define DECAL_TILE_PX 16u
#define DECAL_PER_TILE 16u
#define DECAL_TILE_WORDS 9u            // count + 16 x uint16
#define DECAL_TILES_HEADER_BYTES 16u   // tilesX, tilesY, status, overflowing tiles
#define DECAL_STATUS_TILE_FULL 1u

// CPU record (80 B): the box maps the unit cube [-1, 1]^3 to its space: p = box * (u, 1) (columns: the half-extent axes
// X, Y, Z, then the centre).
struct DecalRecord
{
    float4 box[3];
    uint material, instance;   // instance: DECAL_NONE = world space
    int priority;
    uint order;                // creation order (age)
    float opacity, cosFadeStart, cosFadeEnd, edge;  // angle fade from cosFadeStart (full) to cosFadeEnd (none); edge: soft
                                                     // fraction of the box depth at +-Z
};
// Per-frame record (128 B): camera-relative box and its inverse.
struct DecalFrame
{
    float4 toDecal[3];         // camera-relative position -> unit cube coordinates
    float3 centre; uint material;
    float3 axisX; uint instance;
    float3 axisY; int priority;
    float3 axisZ; uint order;
    float opacity, cosFadeStart, cosFadeEnd, edge;
};

// What decalApply reads: the frame records and tile lists (DECAL_NONE: no decals) and M's material texture table
// (DECAL_NONE: constants only).
struct DecalContext
{
    uint frames, tiles, materialTable;
};
// The pixel's surface: camera-relative position and its screen derivatives, unit geometric normal, the instance, and
// the geometric part of the slope variance (the decal's normal replaces the surface's normal map where it covers).
struct DecalSurface
{
    float3 position, dpdx, dpdy;
    float3 geometricNormal;
    uint instance;
    float geometricVariance;
};
// The pixel material decals modify (M's resolve values before the G-buffer: linear base colour, perceptual roughness,
// metallic, unit shading normal, slope variance trace).
struct DecalMaterial
{
    float3 baseColor;
    float roughness, metallic;
    float3 normal;
    float variance;
};

float3 decalToUnit(DecalFrame d, float3 p)
{
    return float3(dot(d.toDecal[0].xyz, p) + d.toDecal[0].w, dot(d.toDecal[1].xyz, p) + d.toDecal[1].w, dot(d.toDecal[2].xyz, p) + d.toDecal[2].w);
}
float3 decalToUnitVector(DecalFrame d, float3 v) { return float3(dot(d.toDecal[0].xyz, v), dot(d.toDecal[1].xyz, v), dot(d.toDecal[2].xyz, v)); }

// Coverage of one decal at the surface (0: none) and its material there.
float decalLayer(DecalContext c, DecalFrame d, DecalSurface s, out DecalMaterial layer)
{
    layer = (DecalMaterial)0;
    if (d.instance != DECAL_NONE && d.instance != s.instance) return 0;
    const float3 u = decalToUnit(d, s.position);
    if (any(abs(u) > 1.0f)) return 0;
    const float3 z = normalize(d.axisZ);
    const float cosAngle = dot(s.geometricNormal, z);
    const float angleFade = saturate((cosAngle - d.cosFadeEnd) / max(d.cosFadeStart - d.cosFadeEnd, 1e-6f));
    const float edgeFade = d.edge > 0 ? saturate((1.0f - abs(u.z)) / d.edge) : 1.0f;
    float a = d.opacity * angleFade * edgeFade;
    if (!(a > 0)) return 0;
    // uv: +x right, +y up in the box -> v down (texture rows)
    const float2 uv = float2(0.5f + 0.5f * u.x, 0.5f - 0.5f * u.y);
    const float3 ux = decalToUnitVector(d, s.dpdx), uy = decalToUnitVector(d, s.dpdy);
    const float2 duvdx = float2(0.5f * ux.x, -0.5f * ux.y), duvdy = float2(0.5f * uy.x, -0.5f * uy.y);
    const GpuMaterial m = loadMaterial(d.material);
    float3 base = m.baseColor;
    float roughness = m.roughness, metallic = m.metallic;
    float3 n = s.geometricNormal;
    float variance = s.geometricVariance;
    if (c.materialTable != DECAL_NONE)
    {
        const MTextureSet ts = mLoadTextureSet(c.materialTable, d.material);
        if (ts.baseColor != UNX_NONE)
        {
            Texture2D<float4> t = ResourceDescriptorHeap[ts.baseColor];
            const float4 v = mSampleGrad(t, true, uv, duvdx, duvdy);
            base *= v.rgb;
            a *= v.a;
        }
        if (ts.roughMetal != UNX_NONE)
        {
            Texture2D<float4> t = ResourceDescriptorHeap[ts.roughMetal];
            const float2 rm = mSampleGrad(t, true, uv, duvdx, duvdy).xy;
            roughness *= rm.x;
            metallic *= rm.y;
        }
        if (ts.moments != UNX_NONE)
        {
            // Tangent frame of the decal's texture on the surface: T along +u (box +X), B along +v (box -Y), both
            // projected onto the surface.
            Texture2D<float4> t = ResourceDescriptorHeap[ts.moments];
            const MSlopeMoments mm = mNormalMoments(t, uv, duvdx, duvdy, ts.slopeRange, true);
            const float3 g = s.geometricNormal;
            float3 T = d.axisX - g * dot(d.axisX, g);
            float3 B = -d.axisY - g * dot(-d.axisY, g);
            T = normalize(T);
            B = normalize(B - T * dot(B, T));
            n = normalize(T * mm.mean.x + B * mm.mean.y + g);
            variance += mm.variance;
        }
    }
    layer.baseColor = base;
    layer.roughness = roughness;
    layer.metallic = metallic;
    layer.normal = n;
    layer.variance = variance;
    return saturate(a);
}

// Every decal of the pixel's tile over the material, lowest (priority, order) first.
void decalApply(DecalContext c, uint2 pixel, DecalSurface s, inout DecalMaterial m)
{
    if (c.frames == DECAL_NONE || c.tiles == DECAL_NONE) return;
    ByteAddressBuffer tiles = ResourceDescriptorHeap[c.tiles];
    StructuredBuffer<DecalFrame> frames = ResourceDescriptorHeap[c.frames];
    const uint tilesX = tiles.Load(0);
    const uint tile = (pixel.y / DECAL_TILE_PX) * tilesX + pixel.x / DECAL_TILE_PX;
    const uint base = DECAL_TILES_HEADER_BYTES + tile * DECAL_TILE_WORDS * 4u;
    const uint count = min(tiles.Load(base), DECAL_PER_TILE);
    if (count == 0) return;
    uint ids[DECAL_PER_TILE];
    [unroll] for (uint k = 0; k < DECAL_PER_TILE; ++k)
    {
        const uint w = tiles.Load(base + 4u + (k / 2u) * 4u);
        ids[k] = (w >> (16u * (k & 1u))) & 0xFFFFu;
    }
    // insertion sort by (priority, order)
    for (uint i = 1; i < count; ++i)
    {
        const uint v = ids[i];
        const DecalFrame dv = frames[v];
        uint j = i;
        for (; j > 0; --j)
        {
            const DecalFrame dj = frames[ids[j - 1]];
            if (dj.priority < dv.priority || (dj.priority == dv.priority && dj.order <= dv.order)) break;
            ids[j] = ids[j - 1];
        }
        ids[j] = v;
    }
    for (uint i = 0; i < count; ++i)
    {
        DecalMaterial layer;
        const float a = decalLayer(c, frames[ids[i]], s, layer);
        if (!(a > 0)) continue;
        m.baseColor = lerp(m.baseColor, layer.baseColor, a);
        m.roughness = lerp(m.roughness, layer.roughness, a);
        m.metallic = lerp(m.metallic, layer.metallic, a);
        m.normal = normalize(lerp(m.normal, layer.normal, a));
        m.variance = lerp(m.variance, layer.variance, a);
    }
}
#endif
