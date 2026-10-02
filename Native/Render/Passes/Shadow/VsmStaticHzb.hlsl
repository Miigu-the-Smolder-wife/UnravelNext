// unx-kernel: cs_6_6 main
// shadow.vsm.static_hzb_cull: the HZB of a sun page's static copy (the reference's per-page HZB of the physical pool, its
// static layer). One group per page drawn anew this frame (the page list), after the static casters' raster: per block
// of 8 texels the farthest stored depth (the smallest: reversed Z, 0 = a texel without caster, so a block with one
// hides nothing), then the blocks of 16, 32, 64 and the page, at the VsmBlock offsets (VsmCommon.hlsli vsmBlockOffset:
// 0, 256, 320, 336, 340; VSM_BLOCK_ENTRIES floats per physical page). V's cull of the movable casters' views reads it
// (VisibilityCommon.hlsli tilesOcclude): a movable caster under the static surface of every page it would be drawn into
// leaves no texel after the merge. A kept page's HZB is the one built when its static copy was drawn. Local lights'
// pages: with shadow.vsm.local_static_separate (P[0].w = 1) they have static copies and their movable casters' views
// read the HZB; without it they have neither and are skipped.
// P[0].x page list SRV (raw: count, pad, (slot, page) pairs), P[0].y static atlas SRV (Texture2D<float>), P[0].z HZB UAV
// (raw), P[0].w 1: the local lights' pages too
#include "Passes/Shadow/VsmCommon.hlsli"

groupshared float g_farthest[256];

[numthreads(256, 1, 1)]
void main(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    ByteAddressBuffer list = ResourceDescriptorHeap[P[0].x];
    const uint2 entry = list.Load2(8 + group.x * 8);
    if (entry.x >= VSM_SUN_SLOTS && P[0].w == 0) return;  // (uniform over the group)
    const uint phys = entry.y;
    Texture2D<float> atlas = ResourceDescriptorHeap[P[0].y];
    RWByteAddressBuffer hzb = ResourceDescriptorHeap[P[0].z];
    const uint base = phys * VSM_BLOCK_ENTRIES;
    // the 8-texel block of this lane (16 x 16 per page, row major): its 64 texels
    const uint2 corner = uint2(lane % 16, lane / 16) * 8;
    float farthest = 1;
    [loop] for (uint t = 0; t < 64; ++t) farthest = min(farthest, atlas.Load(vsmAtlasTexel(phys, corner + uint2(t % 8, t / 8))));
    g_farthest[lane] = farthest;
    hzb.Store((base + lane) * 4, asuint(farthest));
    GroupMemoryBarrierWithGroupSync();
    // The coarser blocks: level m has n x n blocks (n = 16 >> m), each the smallest of its 2 x 2 children. Every lane reads
    // its children before any lane writes the level (the parents take the children's first slots).
    [unroll] for (uint m = 1; m <= 4; ++m)
    {
        const uint n = 16u >> m;
        float v = 1;
        if (lane < n * n)
        {
            const uint2 child = uint2(lane % n, lane / n) * 2;
            [unroll] for (uint c = 0; c < 4; ++c) v = min(v, g_farthest[(child.y + (c >> 1)) * (2 * n) + child.x + (c & 1)]);
        }
        GroupMemoryBarrierWithGroupSync();
        if (lane < n * n)
        {
            g_farthest[lane] = v;
            hzb.Store((base + vsmBlockOffset(m) + lane) * 4, asuint(v));
        }
        GroupMemoryBarrierWithGroupSync();
    }
}
