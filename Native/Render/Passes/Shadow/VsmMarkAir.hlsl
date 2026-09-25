// unx-kernel: cs_6_6 main
// Page requests of the air (froxel integration, VsmAir.hlsli): every froxel's tile-centre segment requests, at its air
// level, the pages it crosses (the lookup walks exactly these). One group per tile, one thread per depth slice. Written
// with InterlockedOr of VSM_REQ_PROPAGATED (the pixel marks' VSM_REQ_PIXEL stay; air pages need no neighbourhood
// propagation).
// P[0].x requests UAV (raw), P[0].y VSM constants CBV, P[0].z gridX | gridY << 16, P[0].w slices | tilePx << 16
// P[1].x nearM (float bits), P[1].y farM (float bits), P[1].z shadow texels per tile (float bits). Frame constants of the
// main view.
#include "Bindless.hlsli"
#include "Frame.hlsli"
#include "Passes/Atmosphere/FroxelCommon.hlsli"
#include "Passes/Shadow/VsmAir.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 gid : SV_GroupID, uint s : SV_GroupIndex)
{
    FroxelGrid g = (FroxelGrid)0;
    g.gridX = P[0].z & 0xFFFFu;
    g.gridY = P[0].z >> 16;
    g.slices = P[0].w & 0xFFFFu;
    g.tilePx = P[0].w >> 16;
    g.nearM = asfloat(P[1].x);
    g.farM = asfloat(P[1].y);
    g.logRatio = log2(g.farM / g.nearM);
    if (s >= g.slices) return;
    ConstantBuffer<VsmConstants> c = ResourceDescriptorHeap[P[0].y];
    RWByteAddressBuffer requests = ResourceDescriptorHeap[P[0].x];
    const float3 ray = froxelTileRay(g, gid.xy);
    const float z0 = froxelNodeDepth(g, s), z1 = froxelNodeDepth(g, s + 1);
    uint k;
    if (!vsmAirLevel(c, froxelTileWidth(g, 0.5 * (z0 + z1)), asfloat(P[1].z), k)) return;
    // The pages the segment crosses (the lookup's own page walk).
    const float3 p0 = vsmLightSpaceAt(c, g_cameraPosition + ray * z0, k), p1 = vsmLightSpaceAt(c, g_cameraPosition + ray * z1, k);
    const float texel = vsmTexel(k);
    const float2 A = p0.xy / texel, D = (p1.xy - p0.xy) / texel;
    VsmAirWalk w = vsmAirWalkBegin(A, D, VSM_PAGE, 0, 1);
    float ta, tb;
    int2 page;
    [loop] for (uint guard = 0; guard < 512 && vsmAirWalkNext(w, ta, tb, page); ++guard)
        if (vsmInWindow(c, page, k)) requests.InterlockedOr(vsmSlot(page, k) * 4, VSM_REQ_PROPAGATED | VSM_REQ_AIR);
}
