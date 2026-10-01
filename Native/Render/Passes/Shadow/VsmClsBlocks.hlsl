// unx-kernel: cs_6_6 main
// Block hierarchy of the classification pages (RENDERER_REDESIGN_V2 14.3-1, L3; owner A): per page (active light a,
// face f: page a x 6 + f, VsmCls.hlsli), the 16 x 16 blocks of 8 x 8 texels, each the nearest caster's device depth in
// it (max; 0 = no caster). One group per page, one thread per block.
// P[0] = { classification atlas SRV (raw), blocks UAV (raw: page x 256 floats), atlas width in texels, pages per row }
#include "Bindless.hlsli"
#include "Passes/Shadow/VsmCls.hlsli"

[numthreads(16, 16, 1)]
void main(uint3 gid : SV_GroupID, uint2 tid : SV_GroupThreadID)
{
    ByteAddressBuffer atlas = ResourceDescriptorHeap[P[0].x];
    RWByteAddressBuffer blocks = ResourceDescriptorHeap[P[0].y];
    const uint page = gid.x;
    const uint2 origin = vsmClsPageOrigin(page, P[0].w) + tid * 8;
    float nearest = 0;
    [unroll] for (uint y = 0; y < 8; ++y)
        [unroll] for (uint x = 0; x < 8; ++x)
            nearest = max(nearest, asfloat(atlas.Load(((origin.y + y) * P[0].z + origin.x + x) * 4)));
    blocks.Store((page * 256 + tid.y * 16 + tid.x) * 4, asuint(nearest));
}
