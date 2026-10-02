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
// Soft particles (fx.particles.soft; P[0].y != 0): a sprite is a ball of its radius R around its centre, not a plane at
// the centre's depth. Where a surface cuts the ball, the sprite's optical depth at the pixel is the part of the ball's
// chord in front of the surface - with h = R sqrt(1 - q) the half chord at the pixel (q = d^2 / r^2 of the profile),
// f = saturate((z_surface - (z_centre - h)) / (2 h)) and the opacity 1 - (1 - a)^f - so the sprite meets the surface
// without an edge (Unreal: the DepthFade node, and this ball form in its spherical particle opacity). The layer takes
// a sprite whose ball is in front of every opaque pixel of the block; one whose ball reaches the block's depth range is
// an edge block's. Near fade (fx.particles.near_fade; P[0].z != 0): opacity x saturate((z_centre - R - near) / R), the
// same reference function's fade of a ball that reaches the near plane (a sprite the camera is inside of fades out
// instead of covering the view). Strips keep the test at their hit.
// Sprite looks (ParticleLayerPass.hlsli fxLookSample): a look's sprite is its quad with its texture, flipbook frames
// and per-pixel light; its ball for the soft test has the quad's shorter half axis as radius, and its opacity there is
// the texture's. Blends: alpha (L += T a c, T *= 1 - a), additive (L += T a c), premultiplied (L += T c, T *= 1 - a).
// Motion (LayerExtra::motion, RG16F at the layer's size): per layer pixel the sprites' travel on screen since the
// previous frame, weighted by what each adds to the pixel (T a); an edge block's is one of its full-resolution
// pixels'. The upscale's layer motion reads it where the particles hold the pixel (UpscaleMotion.hlsl).
#include "Passes/FX/ParticleLayerPass.hlsli"

groupshared uint2 gs_entries[FX_LAYER_TILE_ENTRIES];
groupshared uint gs_edges[64];
groupshared uint gs_edgeCount;

// entry order: nearer first (larger depth bits), then smaller record index
bool before(uint2 a, uint2 b) { return a.x != b.x ? a.x > b.x : a.y < b.y; }

// Front-to-back composite of the sorted list at full-resolution point 'p' against opaque device depth 'opaque'; with
// 'layer', only the sprites of the layer (not small) and it reports whether the block needs the full-resolution walk.
float4 composite(uint count, float2 p, float opaqueFar, float opaqueNear, bool layer, out bool edge, out float2 depthRange, out float2 motion)
{
    const LayerConstants c = fxLayerConstants();
    const LayerExtra x = fxLayerExtra();
    RWStructuredBuffer<LayerRecord> records = ResourceDescriptorHeap[c.records];
    float3 L = 0;
    float T = 1;
    float3 moved = 0;  // sum of T a x motion, sum of T a
    edge = false;
    depthRange = float2(0, 0);
    motion = 0;
    const float blockReach = layer ? 2.0f * 1.41421356f : 0.0f;  // centre to corner of a 4 x 4 block
    const bool soft = P[0].y != 0u, nearFade = P[0].z != 0u;
    const float pixelWorld = 2.0f / (g_proj[1][1] * g_viewHeight);  // a pixel's world size per unit of view depth
    // view depths of the opaque range (device depth 0: nothing there)
    const float zFar = opaqueFar > 0 ? g_nearPlane / opaqueFar : 3.0e38f, zNear = opaqueNear > 0 ? g_nearPlane / opaqueNear : 3.0e38f;
    for (uint i = 0u; i < count; ++i)
    {
        const LayerRecord r = records[gs_entries[i].y];
        const float2 d = p - r.centre;
        const float reach = r.radius + blockReach;
        if (dot(d, d) >= reach * reach) continue;          // the square test binned it; the disc misses this block/pixel
        const bool strip = (r.flags & FX_LAYER_RECORD_STRIP) != 0u;
        // a sprite as a ball: its centre's view depth and world radius (soft particles, near fade); a look's quad: its
        // shorter half axis
        const bool ball = !strip && (soft || nearFade);
        const bool looked = !strip && fxRecordLook(r) != 0u;
        float radiusPx = r.radius;
        if (looked)
        {
            const float4 axes = fxUnpackHalf4(r.axes);
            radiusPx = min(length(axes.xy), length(axes.zw));
        }
        const float zc = ball ? g_nearPlane / max(r.depth, 1e-30f) : 0.0f, R = ball ? radiusPx * pixelWorld * zc : 0.0f;
        if (ball && soft)
        {
            if (zc - R >= zFar) continue;                  // the ball is behind every opaque pixel of the block (this pixel's surface)
        }
        else if (!(r.depth > opaqueFar)) continue;         // behind every opaque pixel of the block (or this pixel's surface)
                                                           // (a strip's record depth is its nearest vertex)
        if (layer)
        {
            const bool cut = ball && soft ? zc + R > zNear : r.depth < opaqueNear;  // (a surface of the block cuts the sprite)
            if ((r.flags & FX_LAYER_RECORD_SMALL) != 0u || (!strip && cut)) { edge = true; continue; }
        }
        float a, sampleDepth;
        float3 colour;
        uint blend = FX_BLEND_ALPHA;
        float q = dot(d, d) / (r.radius * r.radius);
        if (looked)
        {
            FxLookHit hit;
            if (!fxLookSample(c, x, r, p, layer ? (float)FX_LAYER_SCALE : 1.0f, hit)) continue;
            a = hit.a;
            colour = hit.colour;
            blend = hit.blend;
            q = hit.q;
            sampleDepth = r.depth;
        }
        else if (strip)
        {
            if (!fxStripSampleOf(c, x, r, p, layer ? (float)FX_LAYER_SCALE : 1.0f, a, colour, sampleDepth, blend)) continue;
        }
        else if (!fxLayerSample(c, r, p, a, colour, sampleDepth)) continue;
        if (ball)
        {
            float fade = 1;
            if (soft && !layer)
            {
                // (zFar: this pixel's surface) the part of the ball's chord in front of it
                const float h = R * sqrt(saturate(1.0f - q));
                fade = h > 0 ? saturate((zFar - (zc - h)) / (2.0f * h)) : (zFar > zc ? 1.0f : 0.0f);
                if (!(fade > 0)) continue;
            }
            const float near = nearFade ? saturate((zc - R - g_nearPlane) / max(R, 1e-12f)) : 1.0f;
            // the faded opacity: the optical depth x the part in front (an additive sprite: its weight x the part), then
            // the near fade; a premultiplied colour follows its coverage
            const float before = a;
            if (blend == FX_BLEND_ADDITIVE) a *= fade;
            else if (fade < 1) a = 1.0f - pow(saturate(1.0f - a), fade);
            a *= near;
            if (blend == FX_BLEND_PREMULTIPLIED) colour *= before > 0 ? a / before : fade * near;
            if (!(a > 0) && !(blend == FX_BLEND_PREMULTIPLIED && any(colour > 0))) continue;
        }
        if (strip)
        {
            // a strip's depth varies over it: the test at this sample (its hit)
            if (!(sampleDepth > opaqueFar)) continue;
            if (layer && sampleDepth < opaqueNear) { edge = true; continue; }
        }
        moved += T * a * float3(fxRecordMotion(r), 1);
        L += T * (blend == FX_BLEND_PREMULTIPLIED ? colour : a * colour);
        if (blend != FX_BLEND_ADDITIVE) T *= 1.0f - a;
        depthRange = float2(depthRange.x == 0 ? sampleDepth : min(depthRange.x, sampleDepth), max(depthRange.y, sampleDepth));
    }
    if (moved.z > 0) motion = moved.xy / moved.z;
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
        float2 range, motion;
        const float4 value = composite(drawn, float2(lp * FX_LAYER_SCALE) + 2.0f, opaqueFar, opaqueNear, true, edge, range, motion);
        RWTexture2D<float4> layer = ResourceDescriptorHeap[c.layer];
        RWTexture2D<float2> depthRange = ResourceDescriptorHeap[c.depthRange];
        RWTexture2D<float2> layerMotion = ResourceDescriptorHeap[fxLayerExtra().motion];
        layer[lp] = value;
        depthRange[lp] = range;
        layerMotion[lp] = motion;
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
            float2 range, motion;
            value = composite(drawn, float2(px) + 0.5f, d, d, false, unused, range, motion);
            if (q == 5u)
            {
                // (the block's motion and depth range: this pixel's, where the layer's value left the block's cut and
                // full-resolution sprites out)
                RWTexture2D<float2> layerMotion = ResourceDescriptorHeap[fxLayerExtra().motion];
                RWTexture2D<float2> depthRange2 = ResourceDescriptorHeap[c.depthRange];
                layerMotion[blp] = motion;
                depthRange2[blp] = range;
            }
        }
        blocks.Store2(blocksOffset + blockIndex * 128u + 8u * q, fxPackHalf4(value));
    }
}
