// unx-kernel: cs_6_6 main
// unx-variants: MODE=0,1
// Overflow list allocation (INTERFACES 7.3; RENDERER_REDESIGN_V2 14.3-3, L3 stage 3): the exclusive prefix sum, in tile
// order, of the words each listed tile needs (ShadowOverflow.hlsl MODE0 wrote them; 0 for the other tiles), in two
// levels of 2048 (at most 2048 x 2048 tiles). The allocation then depends only on the frame's content, not on the
// order the tiles' groups ran (the atomic allocation before it put the same tiles at different offsets from frame to
// frame and, over the capacity, sent a different set of tiles to the fallback path).
//   MODE=0: one group per block of 2048 tiles (two per thread): the needs -> the exclusive prefixes within the block
//           in place; the block totals into blocks[block].
//   MODE=1: one group: the exclusive scan of the block totals in place (<= 2048 blocks); the grand total (the view's
//           need in words) into blocks[2048].
// P[0].x need / offset UAV (raw, 4 B per tile), P[0].y block sums UAV (raw, 2049 x 4 B), P[0].z element count
// (tiles in MODE0, blocks in MODE1)
#include "Bindless.hlsli"

groupshared uint gs_sum[1024];

[numthreads(1024, 1, 1)]
void main(uint group : SV_GroupID, uint lane : SV_GroupIndex)
{
    RWByteAddressBuffer need = ResourceDescriptorHeap[P[0].x];
    RWByteAddressBuffer blocks = ResourceDescriptorHeap[P[0].y];
    const uint n = P[0].z;
#if MODE == 0
    const uint i0 = group * 2048 + lane * 2;
    const uint v0 = i0 < n ? need.Load(i0 * 4) : 0u;
    const uint v1 = i0 + 1 < n ? need.Load((i0 + 1) * 4) : 0u;
#else
    const uint i0 = lane * 2;
    const uint v0 = i0 < n ? blocks.Load(i0 * 4) : 0u;
    const uint v1 = i0 + 1 < n ? blocks.Load((i0 + 1) * 4) : 0u;
#endif
    const uint pair = v0 + v1;
    gs_sum[lane] = pair;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint step = 1; step < 1024; step *= 2)
    {
        const uint add = lane >= step ? gs_sum[lane - step] : 0u;
        GroupMemoryBarrierWithGroupSync();
        gs_sum[lane] += add;
        GroupMemoryBarrierWithGroupSync();
    }
    const uint inclusive = gs_sum[lane], exclusive = inclusive - pair;
#if MODE == 0
    if (i0 < n) need.Store(i0 * 4, exclusive);
    if (i0 + 1 < n) need.Store((i0 + 1) * 4, exclusive + v0);
    if (lane == 1023) blocks.Store(group * 4, inclusive);
#else
    if (i0 < n) blocks.Store(i0 * 4, exclusive);
    if (i0 + 1 < n) blocks.Store((i0 + 1) * 4, exclusive + v0);
    if (lane == 1023) blocks.Store(2048 * 4, inclusive);
#endif
}
