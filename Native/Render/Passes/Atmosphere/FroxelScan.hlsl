// unx-kernel: cs_6_6 main
// unx-variants: MODE=0,1
// Froxel light lists, allocation (FroxelLists.hlsl MODE=0 counted, MODE=1 fills): exclusive prefix sums of the counts
// rounded up to even (every run then starts at an even entry), in two levels of 2048 - two of them: over all lights
// (the frame's allocation) and over the scene lights alone (the allocation of a frame whose need exceeds the buffer: the
// FX particle lights are left out that frame, FroxelLists.hlsl).
//   MODE=0: one group per block of 2048 froxels (two per thread): the header's second word (scene count | count << 16)
//           -> the exclusive prefixes within the block: all lights into the header's first word, scene lights into the
//           scene allocation buffer; the block totals into blocks[block] and blocks[2048 + block].
//   MODE=1: one group: the exclusive scans of the block totals in place (<= 2048 blocks: 4 M froxels), the grand total
//           into FroxelGrid::needed (word 28) and 0 into FroxelGrid::indexCount (word 44; FroxelLists MODE=1 adds the
//           entries actually stored).
// P[0].x froxelLights UAV (raw), P[0].y blocks UAV (raw, 2 x 2048 x 4 B), P[0].z element count (froxels / blocks),
// P[0].w scene allocation UAV (raw, 4 B per froxel; MODE=0).
#include "Bindless.hlsli"

groupshared uint gs_sum[1024], gs_sumScene[1024];

[numthreads(1024, 1, 1)]
void main(uint group : SV_GroupID, uint lane : SV_GroupIndex)
{
    RWByteAddressBuffer b = ResourceDescriptorHeap[P[0].x];
    RWByteAddressBuffer blocks = ResourceDescriptorHeap[P[0].y];
    const uint n = P[0].z;
    uint v0 = 0, v1 = 0, s0 = 0, s1 = 0;
#if MODE == 0
    const uint headerBase = b.Load(32);  // FroxelGrid::headerBase
    const uint i0 = group * 2048 + lane * 2;
    if (i0 < n)
    {
        const uint c = b.Load(headerBase + i0 * 8 + 4);
        v0 = ((c >> 16) + 1u) & ~1u;
        s0 = ((c & 0xFFFFu) + 1u) & ~1u;
    }
    if (i0 + 1 < n)
    {
        const uint c = b.Load(headerBase + (i0 + 1) * 8 + 4);
        v1 = ((c >> 16) + 1u) & ~1u;
        s1 = ((c & 0xFFFFu) + 1u) & ~1u;
    }
#else
    const uint i0 = lane * 2;
    if (i0 < n) { v0 = blocks.Load(i0 * 4); s0 = blocks.Load(8192 + i0 * 4); }
    if (i0 + 1 < n) { v1 = blocks.Load((i0 + 1) * 4); s1 = blocks.Load(8192 + (i0 + 1) * 4); }
#endif
    const uint pair = v0 + v1, pairScene = s0 + s1;
    gs_sum[lane] = pair;
    gs_sumScene[lane] = pairScene;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint step = 1; step < 1024; step *= 2)
    {
        const uint add = lane >= step ? gs_sum[lane - step] : 0u, addScene = lane >= step ? gs_sumScene[lane - step] : 0u;
        GroupMemoryBarrierWithGroupSync();
        gs_sum[lane] += add;
        gs_sumScene[lane] += addScene;
        GroupMemoryBarrierWithGroupSync();
    }
    const uint inclusive = gs_sum[lane], exclusive = inclusive - pair;
    const uint inclusiveScene = gs_sumScene[lane], exclusiveScene = inclusiveScene - pairScene;
#if MODE == 0
    RWByteAddressBuffer scene = ResourceDescriptorHeap[P[0].w];
    if (i0 < n) { b.Store(headerBase + i0 * 8, exclusive); scene.Store(i0 * 4, exclusiveScene); }
    if (i0 + 1 < n) { b.Store(headerBase + (i0 + 1) * 8, exclusive + v0); scene.Store((i0 + 1) * 4, exclusiveScene + s0); }
    if (lane == 1023) { blocks.Store(group * 4, inclusive); blocks.Store(8192 + group * 4, inclusiveScene); }
#else
    if (i0 < n) { blocks.Store(i0 * 4, exclusive); blocks.Store(8192 + i0 * 4, exclusiveScene); }
    if (i0 + 1 < n) { blocks.Store((i0 + 1) * 4, exclusive + v0); blocks.Store(8192 + (i0 + 1) * 4, exclusiveScene + s0); }
    if (lane == 1023)
    {
        b.Store(28, inclusive);  // FroxelGrid::needed: entries the frame's lists take (the exact allocation)
        b.Store(44, 0u);         // FroxelGrid::indexCount: the fill pass accumulates the stored entries
    }
#endif
}
