// unx-kernel: cs_6_6 main
// Particle render pass, one group per tile (8 x 8 layer pixels = 32 x 32 full-resolution pixels; ParticleLayerPass.hlsli):
//   1. the tile's entries into group memory, sorted front to back (device depth descending; ties by record index, so
//      the order is deterministic), at most FX_LAYER_TILE_ENTRIES (the farthest beyond are dropped with
//      FX_LAYER_STATUS_TILE_OVERFLOW: a defect to redesign, never an expected state);
//   2. thread per layer pixel: its 4 x 4 block's opaque depth range; front-to-back composite at the block centre of the
//      sprites whose radius is >= FX_LAYER_MIN_RADIUS (strips as wide) and that are in front of every opaque pixel of the
//      block (a strip at its hit there):
//      L += T a c, T *= 1 - a. The block is an edge block (ParticleLayer.hlsli) when a small sprite touches it or a
//      sprite's depth lies inside the block's opaque depth range;
//   3. per edge block (16 threads each): every full-resolution pixel composites the whole sorted list at its centre,
//      against its own opaque depth -> the 128 B edge block; the layer pixel's index goes into the edge index table.
// Loops are bounded by the entry count (<= FX_LAYER_TILE_ENTRIES) and the 64 blocks of the tile.
#include "Passes/FX/ParticleLayerPass.hlsli"

groupshared uint2 gs_entries[FX_LAYER_TILE_ENTRIES];
groupshared uint gs_edges[64];
groupshared uint gs_edgeCount;

// entry order: nearer first (larger depth bits), then smaller record index
bool before(uint2 a, uint2 b) { return a.x != b.x ? a.x > b.x : a.y < b.y; }

// Front-to-back composite of the sorted list at full-resolution point 'p' against opaque device depth 'opaque'; with
// 'layer', only the sprites of the layer (not small) and it reports whether the block needs the full-resolution walk.
float4 composite(uint count, float2 p, float opaqueFar, float opaqueNear, bool layer, out bool edge, out float2 depthRange)
{
    const LayerConstants c = fxLayerConstants();
    RWStructuredBuffer<LayerRecord> records = ResourceDescriptorHeap[c.records];
    float3 L = 0;
    float T = 1;
    edge = false;
    depthRange = float2(0, 0);
    const float blockReach = layer ? 2.0f * 1.41421356f : 0.0f;  // centre to corner of a 4 x 4 block
    for (uint i = 0u; i < count; ++i)
    {
        const LayerRecord r = records[gs_entries[i].y];
        const float2 d = p - r.centre;
        const float reach = r.radius + blockReach;
        if (dot(d, d) >= reach * reach) continue;          // the square test binned it; the disc misses this block/pixel
        if (!(r.depth > opaqueFar)) continue;              // behind every opaque pixel of the block (or this pixel's surface)
                                                           // (a strip's record depth is its nearest vertex)
        const bool strip = (r.flags & FX_LAYER_RECORD_STRIP) != 0u;
        if (layer)
        {
            if ((r.flags & FX_LAYER_RECORD_SMALL) != 0u || (!strip && r.depth < opaqueNear)) { edge = true; continue; }
        }
        float a, sampleDepth;
        float3 colour;
        if (!fxLayerSample(c, r, p, a, colour, sampleDepth)) continue;
        if (strip)
        {
            // a strip's depth varies over it: the test at this sample (its hit)
            if (!(sampleDepth > opaqueFar)) continue;
            if (layer && sampleDepth < opaqueNear) { edge = true; continue; }
        }
        L += T * a * colour;
        T *= 1.0f - a;
        depthRange = float2(depthRange.x == 0 ? sampleDepth : min(depthRange.x, sampleDepth), max(depthRange.y, sampleDepth));
    }
    return float4(L, T);
}

[numthreads(64, 1, 1)]
void main(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID)
{
    const LayerConstants c = fxLayerConstants();
    const uint tile = gid.x, t = gtid.x;
    if (tile >= c.tilesX * c.tilesY) return;
    RWStructuredBuffer<uint> counts = ResourceDescriptorHeap[c.tileCounts];
    RWStructuredBuffer<uint> starts = ResourceDescriptorHeap[c.tileStarts];
    RWStructuredBuffer<uint2> entries = ResourceDescriptorHeap[c.entries];
    const uint total = counts[tile], start = starts[tile];
    const uint count = min(total, FX_LAYER_TILE_ENTRIES);
    if (t == 0u)
    {
        gs_edgeCount = 0u;
        if (total > FX_LAYER_TILE_ENTRIES) fxLayerStatus(c, FX_LAYER_STATUS_TILE_OVERFLOW);
    }

    // 1. load and sort (bitonic over the next power of two; pad entries sort last)
    uint n = 1u;
    while (n < count) n <<= 1;
    for (uint i = t; i < n; i += 64u)
    {
        const uint at = start + i;
        gs_entries[i] = i < count && at < c.entryCapacity ? entries[at] : uint2(0u, 0xFFFFFFFFu);
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint size = 2u; size <= n; size <<= 1)
        for (uint stride = size >> 1; stride > 0u; stride >>= 1)
        {
            for (uint i2 = t; i2 < n / 2u; i2 += 64u)
            {
                const uint lo = (i2 / stride) * stride * 2u + (i2 % stride), hi = lo + stride;
                const bool ascending = (lo & size) == 0u;  // "ascending" = the 'before' order
                const uint2 a = gs_entries[lo], b = gs_entries[hi];
                if (before(b, a) == ascending) { gs_entries[lo] = b; gs_entries[hi] = a; }
            }
            GroupMemoryBarrierWithGroupSync();
        }
    const uint drawn = count;  // pad entries (index 0xFFFFFFFF) sorted after them

    // 2. layer pixel
    const uint2 lp = uint2(tile % c.tilesX, tile / c.tilesX) * FX_LAYER_TILE + uint2(t % FX_LAYER_TILE, t / FX_LAYER_TILE);
    const bool inside = lp.x < c.layerWidth && lp.y < c.layerHeight;
    bool edge = false;
    if (inside)
    {
        Texture2D<float> depth = ResourceDescriptorHeap[c.depth];
        float opaqueFar = 1e30f, opaqueNear = 0.0f;  // device depth: larger = nearer
        [unroll] for (uint q = 0u; q < 16u; ++q)
        {
            const uint2 px = lp * FX_LAYER_SCALE + uint2(q & 3u, q >> 2);
            if (px.x >= g_viewWidth || px.y >= g_viewHeight) continue;
            const float d = depth.Load(int3(px, 0));
            opaqueFar = min(opaqueFar, d);
            opaqueNear = max(opaqueNear, d);
        }
        float2 range;
        const float4 value = composite(drawn, float2(lp * FX_LAYER_SCALE) + 2.0f, opaqueFar, opaqueNear, true, edge, range);
        RWTexture2D<float4> layer = ResourceDescriptorHeap[c.layer];
        RWTexture2D<float2> depthRange = ResourceDescriptorHeap[c.depthRange];
        layer[lp] = value;
        depthRange[lp] = range;
        RWByteAddressBuffer edges = ResourceDescriptorHeap[c.edgeBlocks];
        uint index = FX_PARTICLE_NO_EDGE;  // (an overflowing block keeps the count past the capacity: the reader clamps)
        if (edge)
        {
            edges.InterlockedAdd(0u, 1u, index);  // word 0: the edge block count (ParticleLayer.hlsli)
            if (index >= c.edgeCapacity) { index = FX_PARTICLE_NO_EDGE; fxLayerStatus(c, FX_LAYER_STATUS_EDGE_OVERFLOW); }
            else
            {
                uint slot;
                InterlockedAdd(gs_edgeCount, 1u, slot);
                gs_edges[slot] = (index << 12) | (t << 6);  // edge block index, layer pixel in the tile
            }
        }
        edges.Store(16u + 4u * (lp.y * c.layerWidth + lp.x), index);
    }
    GroupMemoryBarrierWithGroupSync();

    // 3. edge blocks at full resolution: 4 blocks at a time, 16 threads each
    const uint edgeCount = gs_edgeCount;
    Texture2D<float> depth2 = ResourceDescriptorHeap[c.depth];
    RWByteAddressBuffer blocks = ResourceDescriptorHeap[c.edgeBlocks];
    const uint blocksOffset = fxParticleEdgeBlocksOffset();
    for (uint b = t / 16u; b < edgeCount; b += 4u)
    {
        const uint word = gs_edges[b], blockIndex = word >> 12, pt = (word >> 6) & 63u, q = t & 15u;
        const uint2 blp = uint2(tile % c.tilesX, tile / c.tilesX) * FX_LAYER_TILE + uint2(pt % FX_LAYER_TILE, pt / FX_LAYER_TILE);
        const uint2 px = blp * FX_LAYER_SCALE + uint2(q & 3u, q >> 2);
        float4 value = float4(0, 0, 0, 1);
        if (px.x < g_viewWidth && px.y < g_viewHeight)
        {
            const float d = depth2.Load(int3(px, 0));
            bool unused;
            float2 unusedRange;
            value = composite(drawn, float2(px) + 0.5f, d, d, false, unused, unusedRange);
        }
        blocks.Store2(blocksOffset + blockIndex * 128u + 8u * q, fxPackHalf4(value));
    }
}
