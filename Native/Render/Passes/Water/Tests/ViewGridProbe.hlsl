// unx-kernel: cs_6_6 main
// Track W view grid probe (ViewGridTests.cpp): thread i evaluates grid point i exactly as the scatter does and stores
// (x, y in 1/256 px, asuint(1 / depth), flag) for the CPU raster reference: P[1].x = 0 the far field (viewGridVertex),
// 2 the adaptive near field's drawn blocks (entries (level, block x, z, 0) of 16 B at P[1].w; thread i = entry x 121 +
// vertex, the block's 11 x 11 vertices from quad 8 block - 1: viewGridNearVertex).
// P[0] params SRV, displacement SRV, output UAV (raw, 16 B per point), points; P[1] mode, 0, slopes SRV, entries SRV
#include "../ViewGrid.hlsli"

[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    if (i >= P[0].w) return;
    const ViewGridParams p = viewGridParams(P[0].x);
    int2 xy;
    float inverseDepth;
    uint flag;
    float2 x0;
    if (P[1].x == 2)
    {
        ByteAddressBuffer entries = ResourceDescriptorHeap[P[1].w];
        const uint3 e = entries.Load3(16 * (i / 121));
        const uint k = i % 121;
        viewGridNearVertex(p, e.x, asint(e.yz) * 8 - 1 + int2(k % 11, k / 11), P[0].y, P[1].z, xy, inverseDepth, flag, x0);
    }
    else
        viewGridVertex(p, int2(i % p.columns, i / p.columns), P[0].y, P[1].z, xy, inverseDepth, flag, x0);
    RWByteAddressBuffer o = ResourceDescriptorHeap[P[0].z];
    o.Store4(i * 16, uint4(asuint(xy), asuint(inverseDepth), flag));
}
