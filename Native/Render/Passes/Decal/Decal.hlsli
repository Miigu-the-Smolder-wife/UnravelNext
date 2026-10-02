// Projected decals (FEATURES_GAME 5; A7). Owner: E. Readers: M's material resolve and R's hit shading, through
// decalApply (the coverage layer's fragments take none: CoverageShade.hlsli covFragmentMaterial).
//
// A decal is an oriented box and an ordinary scene material (its textures go through M's pipeline: footprint-filtered
// base colour with alpha = opacity, rough/metal, LEAN slope moments for the normal). The box is world space, or the
// object space of one instance (then it moves with the instance and paints only that instance). Surfaces inside the
// box whose geometric normal faces its +Z axis get the material as an upper layer: every parameter blends towards
// the decal's by a = texture alpha x opacity x angle fade x edge fade (FEATURES_GAME 5.2: the pixel material is
// modified in material resolve, before the G-buffer; the band-limited roughness then filters the result).
// As Unreal's decals: a decal changes only the parts of the material its channels name (base colour, normal,
// roughness and metallic: a normal-only or roughness-only decal), its base colour is tinted by its colour, its opacity
// fades with its size on screen and over its lifetime (DecalSetup.hlsl STEP 1 folds both into the frame record's
// opacity), and an instance flagged INSTANCE_NO_DECALS takes none. A stain multiplies the base colour instead of
// replacing it (base x lerp(1, decal, a): Unreal 4's DBuffer stain); an emissive decal adds its material's emission
// x a to the pixel's (Unreal draws those into the scene colour after the base pass; here the resolve adds them to the
// pixel's emission, which the shading reads - they light nothing else, as there).
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
// DecalRecord / DecalFrame::channels (decal::DecalChannels)
#define DECAL_CHANNEL_BASE_COLOR 1u
#define DECAL_CHANNEL_NORMAL 2u       // the normal and its slope variance
#define DECAL_CHANNEL_ROUGH_METAL 4u  // roughness and metallic
#define DECAL_CHANNEL_EMISSIVE 8u     // the decal material's emission, added
#define DECAL_STAIN (1u << 8)         // the base colour is multiplied, not replaced

// CPU record (128 B): the box maps the unit cube [-1, 1]^3 to its space: p = box * (u, 1) (columns: the half-extent axes
// X, Y, Z, then the centre).
struct DecalRecord
{
    float4 box[3];
    uint material, instance;   // instance: DECAL_NONE = world space
    int priority;
    uint order;                // creation order (age)
    float opacity, cosFadeStart, cosFadeEnd, edge;  // angle fade from cosFadeStart (full) to cosFadeEnd (none); edge: soft
                                                     // fraction of the box depth at +-Z
    float3 color; uint channels;                     // tint of the base colour; DECAL_CHANNEL_* the decal changes
    // Fades (DecalSetup.hlsl STEP 1). fadeScreenSize: Unreal's FadeScreenSize (0: none). Lifetime, on the frame's clock
    // (g_time, s): in over [fadeInStart, fadeInStart + fadeInDuration], out over [fadeOutStart, + fadeOutDuration]; a
    // duration of 0: no such fade.
    float fadeScreenSize, fadeInStart, fadeInDuration, fadeOutStart;
    float fadeOutDuration, emissive; float2 pad;     // emissive: scale of the decal material's emission
};
// Per-frame record (160 B): camera-relative box and its inverse.
struct DecalFrame
{
    float4 toDecal[3];         // camera-relative position -> unit cube coordinates
    float3 centre; uint material;
    float3 axisX; uint instance;
    float3 axisY; int priority;
    float3 axisZ; uint order;
    float opacity, cosFadeStart, cosFadeEnd, edge;  // opacity: the record's x its screen-size and lifetime fades
    float3 color; uint channels;                     // channels: DECAL_CHANNEL_* | DECAL_STAIN
    float emissive; float3 pad;
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
    float3 emissive;  // what the emissive decals add to the pixel's emission (the caller starts it at 0)
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
    float3 base = m.baseColor * d.color;
    float roughness = m.roughness, metallic = m.metallic;
    float3 n = s.geometricNormal;
    float variance = s.geometricVariance;
    float3 emission = (d.channels & DECAL_CHANNEL_EMISSIVE) != 0 ? m.emissive * d.emissive : float3(0, 0, 0);
    if (c.materialTable != DECAL_NONE)
    {
        const MTextureSet ts = mLoadTextureSet(c.materialTable, d.material);
        if (ts.emissive != UNX_NONE && (d.channels & DECAL_CHANNEL_EMISSIVE) != 0)
        {
            Texture2D<float4> t = ResourceDescriptorHeap[ts.emissive];
            emission *= mSampleGrad(t, true, uv, duvdx, duvdy).rgb;
        }
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
    layer.emissive = emission;
    return saturate(a);
}

// The given decals (indices into the frame records) over the material, lowest (priority, order) first.
// The caller owns this scratch list and never reads its original order again.
// inout lets the sort use that storage instead of an HLSL by-value array copy.
void decalApplyList(DecalContext c, inout uint ids[DECAL_PER_TILE], uint count, DecalSurface s, inout DecalMaterial m)
{
    // an instance that takes no decals (scene::InstanceNoDecals)
    if (s.instance != DECAL_NONE && (loadInstance(s.instance).flags & INSTANCE_NO_DECALS) != 0) return;
    StructuredBuffer<DecalFrame> frames = ResourceDescriptorHeap[c.frames];
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
        const DecalFrame d = frames[ids[i]];
        const float a = decalLayer(c, d, s, layer);
        if (!(a > 0)) continue;
        // (the decal's channels: the parts of the material it changes)
        if ((d.channels & DECAL_CHANNEL_BASE_COLOR) != 0)
            m.baseColor = (d.channels & DECAL_STAIN) != 0 ? m.baseColor * lerp(1.0f, layer.baseColor, a) : lerp(m.baseColor, layer.baseColor, a);
        m.emissive += layer.emissive * a;  // (0 without the emissive channel)
        if ((d.channels & DECAL_CHANNEL_ROUGH_METAL) != 0)
        {
            m.roughness = lerp(m.roughness, layer.roughness, a);
            m.metallic = lerp(m.metallic, layer.metallic, a);
        }
        if ((d.channels & DECAL_CHANNEL_NORMAL) != 0)
        {
            m.normal = normalize(lerp(m.normal, layer.normal, a));
            m.variance = lerp(m.variance, layer.variance, a);
        }
    }
}
// Every decal of the pixel's tile over the material (M's resolve).
void decalApply(DecalContext c, uint2 pixel, DecalSurface s, inout DecalMaterial m)
{
    if (c.frames == DECAL_NONE || c.tiles == DECAL_NONE) return;
    ByteAddressBuffer tiles = ResourceDescriptorHeap[c.tiles];
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
    decalApplyList(c, ids, count, s, m);
}
// A ray hit's decals (R's hit shading, FEATURES_GAME 5.2): the candidates R's decal-AABB query collected (at most
// DECAL_PER_TILE; the box test of each happens here), over the hit's material. c.tiles is not read. The AABB of decal i
// is its frame record's camera-relative box: centre +- (|axisX| + |axisY| + |axisZ|) per component.
void decalApplyHit(DecalContext c, inout uint ids[DECAL_PER_TILE], uint count, DecalSurface s, inout DecalMaterial m)
{
    if (c.frames == DECAL_NONE || count == 0) return;
    decalApplyList(c, ids, min(count, DECAL_PER_TILE), s, m);
}
#endif
