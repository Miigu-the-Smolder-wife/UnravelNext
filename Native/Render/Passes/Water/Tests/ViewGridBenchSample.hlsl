// unx-kernel: cs_6_6 main
// View grid microbench (FEATURES_GAME 1.8 B), displacement sampling alone: one thread per grid point computes its rest
// position and samples the 3 cascades (P[1].x: 0 SampleLevel at the across footprint, 1 SampleGrad anisotropic 16); a
// wave sum is stored so nothing is optimised away.
// P[0] params SRV, displacement SRV, output UAV (raw, 4 B per wave), 0; P[1] anisotropic
#include "../ViewGrid.hlsli"
#include "../WaterLinear.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 group : SV_GroupID, uint thread : SV_GroupThreadID)
{
    const uint i = waterLinear(group, thread, 64);
    const ViewGridParams p = viewGridParams(P[0].x);
    const int2 q = int2(i % p.columns, i / p.columns);
    float sum = 0;
    float2 x0;
    float d;
    if (q.y < int(p.rows) && viewGridRest(p, q, x0, d))
    {
        const float2 footprint = viewGridFootprint(p, d, q.y);
        const float2 along = normalize(x0 - p.camera.xz);
        const float3 disp = P[1].x ? viewGridDisplacementAniso(P[0].y, p.lengths, x0, footprint, along)
                                   : viewGridDisplacementTrilinear(P[0].y, p.lengths, x0, footprint.x);
        sum = disp.x + disp.y + disp.z;
    }
    sum = WaveActiveSum(sum);
    if (WaveIsFirstLane())
    {
        RWByteAddressBuffer o = ResourceDescriptorHeap[P[0].z];
        o.Store((i / WaveGetLaneCount()) * 4, asuint(sum));
    }
}
