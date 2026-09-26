// unx-kernel: cs_6_6 main
// W3 seam (engine 2): writes one record of the fluid surface's basin table (FluidSurface.hlsli FsBasin), one group per
// basin before the count: P[0] = (table UAV, index, 0, 0), P[1] = (centre xyz, eta field SRV), P[2] = (cos, sin, size x,
// size z). The record's band is this frame's max |eta| over the whole field (the pool's 257^2 samples; the surface is
// their bilinear interpolation, so no point of it leaves the band): the count and emit decide from it which cells can
// meet the water. 48 B records: (centre xyz, field), (cos, sin, size x, size z), (band, 0, 0, 0).
#include "Bindless.hlsli"
groupshared float g_max[256];
[numthreads(256, 1, 1)]
void main(uint t : SV_GroupThreadID)
{
    Texture2D<float4> field = ResourceDescriptorHeap[P[1].w];
    float m = 0;
    for (uint k = t; k < 257u * 257u; k += 256u) m = max(m, abs(field.Load(int3(k % 257u, k / 257u, 0)).x));
    g_max[t] = m;
    GroupMemoryBarrierWithGroupSync();
    for (uint s = 128u; s > 0u; s >>= 1)
    {
        if (t < s) g_max[t] = max(g_max[t], g_max[t + s]);
        GroupMemoryBarrierWithGroupSync();
    }
    if (t != 0u) return;
    RWByteAddressBuffer table = ResourceDescriptorHeap[P[0].x];
    table.Store4(P[0].y * 48, P[1]);
    table.Store4(P[0].y * 48 + 16, P[2]);
    table.Store4(P[0].y * 48 + 32, uint4(asuint(g_max[0]), 0, 0, 0));
}
