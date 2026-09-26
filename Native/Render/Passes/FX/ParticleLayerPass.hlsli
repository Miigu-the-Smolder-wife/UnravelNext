// Particle render pass (G7; request 20260926_FX_particle_render_pass.md), shared declarations of its kernels. The view's
// frame constants are the root CBV b1 (Common/Frame.hlsli); the pass's own values are one LayerConstants record whose
// index is P[0].x.
//
// Screen space: full-resolution pixel i covers [i, i + 1) (centre i + 0.5); the layer pixel (lx, ly) covers the 4 x 4 block
// [4 lx, 4 lx + 4) x [4 ly, 4 ly + 4) and is sampled at its centre (4 lx + 2, 4 ly + 2); a tile is 8 x 8 layer pixels
// (32 x 32 full-resolution pixels). Depth is the device depth of the reversed-Z infinite projection (near / view distance:
// larger = closer).
#ifndef FX_PARTICLE_LAYER_PASS_HLSLI
#define FX_PARTICLE_LAYER_PASS_HLSLI
#include "Bindless.hlsli"
#include "Frame.hlsli"
#include "Passes/FX/StreamRecords.hlsli"
#include "Passes/FX/ParticleLayer.hlsli"
#include "Passes/Atmosphere/Atmosphere.hlsli"

#define FX_LAYER_TILE 8u         // layer pixels per tile side
#define FX_LAYER_SCALE 4u        // full-resolution pixels per layer pixel side
#define FX_LAYER_TILE_ENTRIES 2048u  // sorted entries a tile holds in group memory (more: FX_LAYER_STATUS_TILE_OVERFLOW)
// A sprite is drawn in the 1/4 layer only when its radius is at least this many full-resolution pixels: the layer's
// bilinear reconstruction of the sprite profile a (1 - q)^2 (q = d^2 / r^2) errs by at most h^2 / 8 max|a''| = 1.5 (h / r)^2
// of its opacity (h = 4 px the layer spacing), so r >= 80 px keeps it <= 1/256; smaller sprites mark their blocks for the
// full-resolution walk (ParticleLayer.hlsli edge blocks).
#define FX_LAYER_MIN_RADIUS 80.0f

// counters[] of the pass
#define FX_LAYER_COUNTER_ENTRIES 0u    // tile entries of the frame (FxLayerScan)
#define FX_LAYER_COUNTER_SPARE 1u      // (the edge block count is word 0 of the edge buffer)
#define FX_LAYER_COUNTER_STATUS 2u     // status bits
#define FX_LAYER_COUNTER_DRAWN 3u      // records drawn (visible after the cull)
#define FX_LAYER_STATUS_ENTRY_OVERFLOW 1u   // the tile entries exceed the entry buffer (entries dropped: a defect)
#define FX_LAYER_STATUS_TILE_OVERFLOW 2u    // a tile holds more than FX_LAYER_TILE_ENTRIES entries (the farthest dropped)
#define FX_LAYER_STATUS_EDGE_OVERFLOW 4u    // the edge blocks exceed their buffer (those blocks keep the layer value)
#define FX_LAYER_STATUS_RANGE 8u            // an index outside its buffer
#define FX_LAYER_STATUS_MATERIAL 16u        // a sprite program's material is neither 0 (emissive) nor 1 (lit): not drawn

struct LayerConstants
{
    float3 offsetCur; float w;          // stream anchor of the latest tick - camera (float of a double difference); frame
                                        // time between the previous tick's end (w = 0) and the latest tick's end (w = 1)
    float3 offsetPrev; float dt;        // the previous tick's; dt of the latest tick
    uint threads, current, rangeCount, recordCapacity;
    uint layerWidth, layerHeight, tilesX, tilesY;
    uint posAgeCur, velocityCur, posAgePrev, velocityPrev;
    uint dynamicCur, dynamicPrev, emitters, programs;
    uint curveKeys, ranges, blocks, records;
    uint tileCounts, tileStarts, tileFill, entries;
    uint depth, layer, depthRange, edgeBlocks;
    uint counters, entryCapacity, edgeCapacity, layerSrv;  // layer / edges: UAVs; layerSrv, edgeBlocksSrv: their SRVs
    uint edgeBlocksSrv, ribbonPoints, ribbonLinks, ribbonVertices;  // ribbons (strips): this frame's points, links, vertices
    uint shadowPageTable, shadowPool, shadowBlocks, shadowSearchBound;  // S's ShadowSrvs (stage 2 lighting)
    uint shadowConstants, shadowLights, shadowSlotOfLight, shadowLayers;
    uint giCache, froxelLights, airVolume, transmittance;
    uint multiScatter, ribbonAppearance, ribbonCapacity, stripBase;  // per-point half4 appearance; points; strip records at
                                                                      // stripBase + point (after the sprite records)
    uint ribbonRows;                    // per emitter row uint2 (first point, its birth: the row's dying_birth); x = none: no ribbon
    float3 streamAxes;                  // stream space -> renderer axis signs (the Unity World: (1, 1, -1)); offsets are in stream space
};

// One render range (ParticleSystem.cpp): render threads [thread, thread + count) are births [first, first + count) of
// 'row' at stateBase + (birth - first) of the latest state (bit 31 of prevCountFlags clear; the previous tick's state of
// the same particle at prevBase + (birth - prevFirst) when that is < prevCount) or of the previous state (bit 31 set:
// particles that died in the latest tick).
struct RenderRange { uint thread, count, row, stateBase; uint first, prevBase, prevFirst, prevCountFlags; };

// A drawable particle of this frame: 32 B.
struct LayerRecord
{
    float2 centre;        // full-resolution pixel coordinates
    float radius;         // full-resolution pixels (pixel-footprint prefiltered, ParticleLayer.hlsli); 0 = not drawn
    float depth;          // device depth of the particle centre
    uint2 radianceAlpha;  // half4: radiance x exposure (the air in front applied), opacity scale
    uint flags;           // FX_LAYER_RECORD_*
    uint program;
};
#define FX_LAYER_RECORD_SMALL 1u  // radius < FX_LAYER_MIN_RADIUS: full-resolution walk only
#define FX_LAYER_RECORD_STRIP 2u  // a ribbon/beam segment: radianceAlpha = (its point, the previous point), evaluated per pixel

// Ribbon points of this frame (FxLayerSetup: the particle at the frame time, camera-relative; the layout of the stream's
// ribbon points) and the strip vertices FxRibbon builds from them (two per point, the side frame parallel-transported).
struct FxRibbonPoint { float3 position; float width; float age; uint valid; uint pad0, pad1; };  // 32 B (Particles.hlsli RibbonPoint)
struct FxRibbonVertex { float3 position; float3 normal; float2 uv; };                          // 32 B (FxRibbon.hlsl)

LayerConstants fxLayerConstants()
{
    StructuredBuffer<LayerConstants> c = ResourceDescriptorHeap[P[0].x];
    return c[0];
}
void fxLayerStatus(LayerConstants c, uint bits)
{
    RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[c.counters];
    InterlockedOr(counters[FX_LAYER_COUNTER_STATUS], bits);
}
float4 fxUnpackHalf4(uint2 v) { return float4(f16tof32(v.x), f16tof32(v.x >> 16), f16tof32(v.y), f16tof32(v.y >> 16)); }
uint2 fxPackHalf4(float4 v) { return uint2(f32tof16(v.x) | (f32tof16(v.y) << 16), f32tof16(v.z) | (f32tof16(v.w) << 16)); }

// Tiles [t0, t1] (inclusive) a record's square [centre - radius, centre + radius] overlaps.
bool fxLayerTiles(LayerConstants c, LayerRecord r, out uint2 t0, out uint2 t1)
{
    const float span = (float)(FX_LAYER_TILE * FX_LAYER_SCALE);
    const float2 lo = floor((r.centre - r.radius) / span), hi = floor((r.centre + r.radius) / span);
    t0 = (uint2)clamp(lo, 0.0f, float2(c.tilesX - 1, c.tilesY - 1));
    t1 = (uint2)clamp(hi, 0.0f, float2(c.tilesX - 1, c.tilesY - 1));
    return r.radius > 0 && all(hi >= 0.0f) && lo.x <= (float)(c.tilesX - 1) && lo.y <= (float)(c.tilesY - 1);
}

// Opacity of a record at a point (full-resolution pixel coordinates): the sprite profile alpha (1 - q)^2, q = d^2 / r^2.
float fxLayerOpacity(LayerRecord r, float2 p, float alpha)
{
    const float2 d = p - r.centre;
    const float q = dot(d, d) / (r.radius * r.radius);
    return q < 1.0f ? alpha * (1.0f - q) * (1.0f - q) : 0.0f;
}

// Ray (origin 0, camera-relative direction D) against triangle (a, b, c): distance t along D and barycentrics (u, v) of b
// and c. Two-sided.
bool fxRayTriangle(float3 D, float3 a, float3 b, float3 c, out float t, out float u, out float v)
{
    const float3 e1 = b - a, e2 = c - a, q = cross(D, e2);
    const float det = dot(e1, q);
    t = u = v = 0;
    if (abs(det) < 1e-20f) return false;
    const float inv = 1.0f / det;
    const float3 s = -a;
    u = dot(s, q) * inv;
    const float3 r = cross(s, e1);
    v = dot(D, r) * inv;
    t = dot(e2, r) * inv;
    return u >= 0 && v >= 0 && u + v <= 1 && t > 0;
}

// A strip record at full-resolution point p: the pixel-centre ray's hit on the segment's quad (the previous point's edge
// vertices A0, A1, this point's B0, B1; triangles (A0, A1, B0) and (B0, A1, B1)), s along the segment (0 at A) and e across
// (0 at edge 0). Opacity: the points' alphas interpolated along, times the profile (1 - x^2)^2 across (x = 2 e - 1; the
// sprite profile's section); radiance: the points' interpolated along, then the air between the camera and the hit (S's air
// volume at the pixel and the hit's view depth). Device depth = near / view depth of the hit.
bool fxStripSample(LayerConstants c, LayerRecord r, float2 p, out float a, out float3 colour, out float depth)
{
    a = 0;
    colour = 0;
    depth = 0;
    const uint k = r.radianceAlpha.x, j = r.radianceAlpha.y;
    RWStructuredBuffer<FxRibbonVertex> vertices = ResourceDescriptorHeap[c.ribbonVertices];  // (UAVs: written earlier in the pass)
    const float3 a0 = vertices[2u * j].position, a1 = vertices[2u * j + 1u].position;
    const float3 b0 = vertices[2u * k].position, b1 = vertices[2u * k + 1u].position;
    // the pixel-centre ray (view space at z = -1, then world axes; every projection here has no shear: mPixelRay)
    const float2 ndc = float2(p.x / g_viewWidth * 2 - 1, 1 - p.y / g_viewHeight * 2);
    const float vx = (ndc.x + g_proj[0][2] - g_proj[0][3]) / g_proj[0][0];
    const float vy = (ndc.y + g_proj[1][2] - g_proj[1][3]) / g_proj[1][1];
    const float3 D = g_view[0].xyz * vx + g_view[1].xyz * vy - g_view[2].xyz;  // view depth of D is 1
    float t, u, v, s, e;
    if (fxRayTriangle(D, a0, a1, b0, t, u, v)) { s = v; e = u; }
    else if (fxRayTriangle(D, b0, a1, b1, t, u, v)) { s = 1 - u; e = u + v; }
    else return false;
    if (!(t > g_nearPlane)) return false;
    RWStructuredBuffer<uint2> appearance = ResourceDescriptorHeap[c.ribbonAppearance];
    const float4 pa = fxUnpackHalf4(appearance[j]), pb = fxUnpackHalf4(appearance[k]);
    const float4 ca = lerp(pa, pb, saturate(s));
    const float x = 2 * saturate(e) - 1;
    a = ca.w * (1 - x * x) * (1 - x * x);
    colour = ca.rgb;
    depth = g_nearPlane / t;
    if (c.airVolume != UNX_NONE && c.transmittance != UNX_NONE)
    {
        AtmosphereSrvs atm;
        atm.transmittance = c.transmittance;
        atm.multiScatter = c.multiScatter;
        atm.skyView = UNX_NONE;
        atm.aerial = c.airVolume;
        float3 inscatter, transmittance, sunAtDepth;
        atmosphereAirView(atm, p / float2(g_viewWidth, g_viewHeight), t, inscatter, transmittance, sunAtDepth);
        colour = colour * transmittance + inscatter * g_exposure;
    }
    return a > 0;
}

// Opacity, premultiplied colour and device depth of any record at full-resolution point p.
bool fxLayerSample(LayerConstants c, LayerRecord r, float2 p, out float a, out float3 colour, out float depth)
{
    if ((r.flags & FX_LAYER_RECORD_STRIP) != 0u) return fxStripSample(c, r, p, a, colour, depth);
    const float4 ca = fxUnpackHalf4(r.radianceAlpha);
    a = fxLayerOpacity(r, p, ca.w);
    colour = ca.rgb;
    depth = r.depth;
    return a > 0;
}
#endif
