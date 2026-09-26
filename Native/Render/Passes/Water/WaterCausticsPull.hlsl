// unx-kernel: cs_6_6 main
// Water caustics: the coarser levels' light (WaterCaustics.hlsl causticCell, beams spread wider than WATER_CAUSTIC_BOX - 1
// texels) spread evenly over their fine texels and added to the slice: texel (x, y) gets level L's texel (x, y) >> L
// over 4^L, for every level. Fixed point 2^-16 throughout.
// P[0] caustics UAV (RWTexture2DArray<uint>), grid texels per side (a power of two), slices, level buffer UAV (raw)
#include "WaterLight.hlsli"

uint pullLevelBase(uint nc, uint L)
{
    uint base = 0;
    for (uint l = 1; l < L; ++l) base += (nc >> l) * (nc >> l);
    return base;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint nc = P[0].y;
    if (id.x >= nc || id.y >= nc || id.z >= P[0].z) return;
    RWTexture2DArray<uint> caustics = ResourceDescriptorHeap[P[0].x];
    RWByteAddressBuffer levels = ResourceDescriptorHeap[P[0].w];
    const uint top = firstbithigh(nc);
    const uint slice = id.z * pullLevelBase(nc, top + 1);
    float light = 0;
    uint base = 0;
    for (uint L = 1; L <= top; ++L)
    {
        const uint side = nc >> L;
        light += float(levels.Load(4 * (slice + base + (id.y >> L) * side + (id.x >> L)))) / float(1u << (2 * L));
        base += side * side;
    }
    if (light > 0) caustics[id] += uint(round(light));
}
