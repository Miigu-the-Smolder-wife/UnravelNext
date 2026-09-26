// unx-kernel: cs_6_6 main
// View grid microbench (FEATURES_GAME 1.8 B), vertex pass of the two-pass scatter: one thread per grid point computes
// its displaced, projected vertex once (every quad that shares it reads the same bits: the mesh is watertight without
// assuming the sampler returns identical values when a point is evaluated twice).
// Vertex = (x, y in 1/256 px, asuint(+-1 / depth)); 0 marks an invalid point, a negative 1 / depth a point outside the
// water body (the scatter keeps a triangle with at least one point inside: the surface reaches one cell past the shore,
// where the terrain covers it).
// P[0] params SRV, displacement SRV, vertex UAV (raw, 12 B per point), 0; P[1] anisotropic
#include "../ViewGrid.hlsli"

[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    const ViewGridParams p = viewGridParams(P[0].x);
    if (i >= p.columns * p.rows) return;
    const int2 q = int2(i % p.columns, i / p.columns);
    uint3 v = 0;
    float2 x0;
    float d;
    if (viewGridRest(p, q, x0, d))
    {
        const float2 footprint = viewGridFootprint(p, d, q.y);
        const float2 along = normalize(x0 - p.camera.xz);
        const float3 disp = P[1].x ? viewGridDisplacementAniso(P[0].y, p.lengths, x0, footprint, along)
                                   : viewGridDisplacement(P[0].y, p.lengths, x0, footprint.x);
        const float3 s = viewGridProject(p, float3(x0.x + disp.x, p.waterLevel + disp.y, x0.y + disp.z));
        if (s.z > 0.01) v = uint3(asuint(int2(round(clamp(s.xy, -4.0e6, 4.0e6) * 256.0))), asuint((viewGridWater(p, x0, length(footprint)) ? 1.0 : -1.0) / s.z));
    }
    RWByteAddressBuffer o = ResourceDescriptorHeap[P[0].z];
    o.Store3(i * 12, v);
}
