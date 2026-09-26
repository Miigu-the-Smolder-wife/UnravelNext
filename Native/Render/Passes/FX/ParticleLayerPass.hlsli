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
    uint edgeBlocksSrv, pad0, pad1, pad2;
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
#endif
