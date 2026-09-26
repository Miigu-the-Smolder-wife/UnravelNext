// unx-kernel: cs_6_6 main
// Track W foam test probe: the shading reader (foamLevelSample) on a 256^2 lattice over each level's window, at the
// same place relative to the window (texel origin + 4 k + (1, 1), its centre), so two runs whose windows cover the same
// ground in different coordinates (an origin rebase) are compared place by place through the parameters the kernels see.
// P[0] foam SRV (Texture2DArray), parameter SRV, output UAV (float per sample, level-major), levels
#include "../Foam.hlsli"
#include "../WaterLinear.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 group : SV_GroupID, uint thread : SV_GroupThreadID)
{
    const uint i = waterLinear(group, thread, 64);
    if (i >= 256 * 256 * P[0].w) return;
    const uint level = i / (256 * 256), k = i % (256 * 256);
    Texture2DArray<float> foam = ResourceDescriptorHeap[P[0].x];
    const FoamParams p = foamParams(P[0].y);
    const FoamLevel l = foamLevel(p, level);
    const int2 texel = l.origin + 4 * int2(k % 256, k / 256) + 1;
    float value;
    foamLevelSample(foam, p, level, (float2(texel) + 0.5) * foamSpacing(p, level), value);
    RWByteAddressBuffer result = ResourceDescriptorHeap[P[0].z];
    result.Store(4 * i, asuint(value));
}
