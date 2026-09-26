// unx-kernel: cs_6_6 main
// Track W view grid probe (ViewGridTests.cpp): thread i evaluates grid point i exactly as the scatter does
// (viewGridVertex, or viewGridNearVertex of near level P[1].y when P[1].x = 1) and stores (x, y in 1/256 px,
// asuint(1 / depth), flag) for the CPU raster reference.
// P[0] params SRV, displacement SRV, output UAV (raw, 16 B per point), points; P[1] near, level, slopes SRV
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
    if (P[1].x)
    {
        const uint n = viewGridNearLevel(p, P[1].y).points;
        viewGridNearVertex(p, P[1].y, int2(i % n, i / n), P[0].y, P[1].z, xy, inverseDepth, flag, x0);
    }
    else
        viewGridVertex(p, int2(i % p.columns, i / p.columns), P[0].y, P[1].z, xy, inverseDepth, flag, x0);
    RWByteAddressBuffer o = ResourceDescriptorHeap[P[0].z];
    o.Store4(i * 16, uint4(asuint(xy), asuint(inverseDepth), flag));
}
