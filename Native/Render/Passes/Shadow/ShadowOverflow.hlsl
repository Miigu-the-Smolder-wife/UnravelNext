// unx-kernel: cs_6_6 main
// unx-variants: MODE=0,1
// Overflow list of the shadow visibility (INTERFACES 7.3, v1.20): the shadow-casting lights past the third of each
// pixel's froxel list, for the tiles ShadowVisibility.hlsl listed (indirect, one 64-thread group per 8x8 tile).
// Each pixel recounts its lights past the third (list only); a groupshared scan gives the runs' offsets in the tile
// block (64 pixel words, then ceil(count/4) words per pixel, in pixel order).
// MODE 0 (count; RENDERER_REDESIGN_V2 14.3-3, L3 stage 3): the tile's need in words to the need buffer (P[3].w, 4 B per
// tile of the view; ShadowVisibility wrote 0 for every tile) and the frame's statistics. ShadowOverflowScan then turns
// the needs into exclusive offsets in tile order, so a tile's block start depends only on the frame's content (the
// atomic allocation before it placed blocks in the order the groups happened to run).
// MODE 1 (fill): the same count; the block start = the tile's offset (P[3].w) + its block of 2048 tiles' prefix (P[4].w);
// a tile that fits writes its head 1 + block start and evaluates each light with shadowLocalVisibilityAtReceiver
// (ShadowVisibility.hlsli), the computation of slots 1-3; a tile past the capacity (start + need > capacity: in tile
// order, the tail) writes 0xFFFFFFFF and goes to the fallback list (M evaluates it with the same function).
// P[0].x depth SRV, P[0].y G-buffer SRV, P[0].z tile heads UAV (R32_UINT), P[0].w VSM constants CBV
// P[1].x overflow tile list SRV (raw: count, args, tiles y << 16 | x), P[1].y page table SRV (raw), P[1].z pool SRV,
// P[1].w blocks SRV (raw)
// P[2].x overflow UAV (raw), P[2].y capacity (words), P[2].z fallback tile list UAV (raw), P[2].w statistics UAV (raw:
// word 16 need in words summed over the frame's views (capacity), 17 tiles over capacity, 18 their overflow pixels, 19
// lights past the third over all pixels)
// P[3].x froxel lists SRV (raw), P[3].y local lights SRV, P[3].z slot of light SRV, P[3].w MODE 0: the need UAV (raw,
// 4 B per tile), MODE 1: the offsets SRV (raw: the scanned needs). P[4].x planar mask SRV (R8_UINT; 0xFFFFFFFF: every
// pixel): other pixels have no lights past the third. P[4].z tiles per row of the view, P[4].w (MODE 1) block sums SRV
// (raw: the prefix of each block of 2048 tiles, ShadowOverflowScan).
// Frame constants of the view.
#include "Frame.hlsli"
#include "Scene.hlsli"
#include "Passes/Atmosphere/Froxel.hlsli"
#include "Passes/Shadow/ShadowVisibility.hlsli"

groupshared uint gs_scan[64];
groupshared uint gs_pixels, gs_lights, gs_head;

// Queue only the filter stage; exhaustion executes that same stage here. The
// output byte is zero while queued and is ORed exactly once by the consumer.
float overflowVisibility(ShadowSrvs ss, uint li, ShadowPixelReceiver pr, uint2 pixel, uint address, uint shift)
{
    if (P[4].y == 0xFFFFFFFFu) return shadowLocalVisibilityAtReceiver(ss, li, pr);
    StructuredBuffer<uint> slotOf = ResourceDescriptorHeap[ss.pad0];
    const uint slot = slotOf[li];
    if (slot == VSM_LOCAL_NONE) return 1;
    StructuredBuffer<VsmLocalLight> lights = ResourceDescriptorHeap[ss.lights];
    const VsmLocalLight light = lights[slot];
    ConstantBuffer<VsmConstants> c = ResourceDescriptorHeap[ss.constants];
    VsmLocalResources r;
    r.table = ResourceDescriptorHeap[ss.pageTable]; r.pool = ResourceDescriptorHeap[vsmAtlasSrv(ss.constants)];
    r.blocks = ResourceDescriptorHeap[ss.blocks];
    VsmLocalFilter filter;
    const float value = vsmLocalClassify(r, light, slot, pr.world, pr.normal, pr.footprint,
        c.receiverBiasTexels, c.maxReceiverSlope, c.searchTaps, c.filterTaps, filter);
    if (filter.mip == VSM_LOCAL_NONE) return value;
    RWByteAddressBuffer queue = ResourceDescriptorHeap[P[4].y];
    uint item;
    queue.InterlockedAdd(0, 1, item);
    if (item >= queue.Load(4))
        return vsmLocalFilterVisibility(r, light, slot, pr.world, pr.normal, filter, c.filterTaps);
    const uint at = 16 + item * 48;
    queue.Store4(at, uint4(pixel.x | (pixel.y << 16), slot, address, shift));
    queue.Store4(at + 16, uint4(asuint(filter.centre), asuint(filter.radius)));
    queue.Store4(at + 32, uint4(asuint(filter.tolerance), filter.mip, filter.face, 0));
    return 0;
}

[numthreads(64, 1, 1)]
void main(uint3 gid : SV_GroupID, uint t : SV_GroupIndex)
{
    ByteAddressBuffer tileList = ResourceDescriptorHeap[P[1].x];
    const uint tw = tileList.Load(16 + gid.x * 4);
    const uint2 tile = uint2(tw & 0xFFFFu, tw >> 16);
    const uint2 px = tile * 8 + uint2(t & 7u, t >> 3);

    // The pixel's receiver and list range (as ShadowVisibility.hlsl classifyPixel).
    FroxelSrvs f;
    f.lights = P[3].x;
    f.lightIndices = P[3].x;
    f.scattering = 0;
    f.pad = 0;
    Texture2D<float> depthTex = ResourceDescriptorHeap[P[0].x];
    bool inside = px.x < g_viewWidth && px.y < g_viewHeight;
    if (inside && P[4].x != 0xFFFFFFFFu)
    {
        Texture2D<uint> mask = ResourceDescriptorHeap[P[4].x];
        inside = mask.Load(int3(px, 0)) != 0;
    }
    const float depth = inside ? depthTex.Load(int3(px, 0)) : 0.0;
    uint2 range = 0;
    if (depth > 0) range = froxelLightRange(f, px, linearDepth(depth));
    const uint indexBase = froxelIndexBase(f);
    uint count = 0;
    {
        uint ordinal = 0;
        uint4 lightWords = 0;
        [loop] for (uint i = 0; i < range.y; ++i)
            if (lightCastsShadow(loadLight(froxelLightBuffered(f, indexBase, range, i, lightWords)))) ++ordinal;
        count = ordinal > 3 ? ordinal - 3 : 0;
    }
    const uint words = (count + 3) / 4;

    // Inclusive scan of the run words over the tile's pixels (fixed order: the block layout is deterministic).
    if (t == 0)
    {
        gs_pixels = 0;
        gs_lights = 0;
    }
    gs_scan[t] = words;
    GroupMemoryBarrierWithGroupSync();
    if (count)
    {
        InterlockedAdd(gs_pixels, 1u);
        InterlockedAdd(gs_lights, count);
    }
    [unroll] for (uint o = 1; o < 64; o <<= 1)
    {
        const uint v = t >= o ? gs_scan[t - o] : 0;
        GroupMemoryBarrierWithGroupSync();
        gs_scan[t] += v;
        GroupMemoryBarrierWithGroupSync();
    }
    const uint runStart = 64 + gs_scan[t] - words;  // word offset in the tile block
    const uint tileIndex = tile.y * P[4].z + tile.x;

#if MODE == 0
    if (t == 0)
    {
        RWByteAddressBuffer stats = ResourceDescriptorHeap[P[2].w];
        RWByteAddressBuffer needBuf = ResourceDescriptorHeap[P[3].w];
        const uint need = 64 + gs_scan[63];
        needBuf.Store(tileIndex * 4, need);
        stats.InterlockedAdd(64, need);
        stats.InterlockedAdd(76, gs_lights);
    }
    return;
#else
    if (t == 0)
    {
        RWByteAddressBuffer stats = ResourceDescriptorHeap[P[2].w];
        RWTexture2D<uint> heads = ResourceDescriptorHeap[P[0].z];
        const uint need = 64 + gs_scan[63];
        ByteAddressBuffer offsets = ResourceDescriptorHeap[P[3].w];
        ByteAddressBuffer blockSums = ResourceDescriptorHeap[P[4].w];
        const uint start = offsets.Load(tileIndex * 4) + blockSums.Load((tileIndex / 2048u) * 4);
        uint head = 1 + start;
        if (start + need > P[2].y)
        {
            head = 0xFFFFFFFFu;
            RWByteAddressBuffer fallback = ResourceDescriptorHeap[P[2].z];
            uint at;
            fallback.InterlockedAdd(0, 1, at);
            fallback.InterlockedAdd(4, 1);  // dispatch args: one group per tile
            fallback.Store(16 + at * 4, tw);
            stats.InterlockedAdd(68, 1);
            stats.InterlockedAdd(72, gs_pixels);
        }
        heads[tile] = head;
        gs_head = head;
    }
    GroupMemoryBarrierWithGroupSync();
#endif
    const uint head = gs_head;
    if (head == 0xFFFFFFFFu) return;
    RWByteAddressBuffer overflow = ResourceDescriptorHeap[P[2].x];
    const uint block = head - 1;
    overflow.Store((block + t) * 4, count ? (count << 24) | runStart : 0u);
    if (count == 0) return;

    // The lights past the third, list order, 4 visibilities per word (low byte first; 255 without a shadow slot).
    const ShadowPixelReceiver pr = shadowPixelReceiver(px, P[0].x, P[0].y);
    ShadowSrvs ss;
    ss.pageTable = P[1].y;
    ss.pool = P[1].z;
    ss.blocks = P[1].w;
    ss.searchBound = 0;
    ss.constants = P[0].w;
    ss.lights = P[3].y;
    ss.pad0 = P[3].z;
    ss.layers = 0xFFFFFFFFu;
    uint ordinal = 0, n = 0, packed = 0;
    uint4 lightWords = 0;
    [loop] for (uint i = 0; i < range.y; ++i)
    {
        const uint li = froxelLightBuffered(f, indexBase, range, i, lightWords);
        if (!lightCastsShadow(loadLight(li))) continue;
        if (++ordinal <= 3) continue;
        const uint v = (uint)round(saturate(overflowVisibility(ss, li, pr, px,
            (block + runStart + n / 4) * 4, 8 * (n & 3u))) * 255.0);
        packed |= v << (8 * (n & 3u));
        if ((++n & 3u) == 0 || n == count)
        {
            overflow.Store((block + runStart + (n - 1) / 4) * 4, packed);
            packed = 0;
        }
    }
}
