// unx-kernel: cs_6_6 main
// Page requests of the fog's volume (FogVolume.hlsli, FogScatter.hlsl): every cell the view sees into requests, at the
// air level of its width, the pages its centre ray crosses from half a slice before the cell to half a slice past it
// (the scatter's segment is the cell's slab moved by the frame's jitter: within that range, at this level), no farther
// than the farthest surface of the cell's pixels. One thread per cell. Written with InterlockedOr of
// VSM_REQ_PROPAGATED | VSM_REQ_AIR, as the air's (VsmMarkAir.hlsl).
// P[0] = { requests UAV (raw), VSM constants CBV, grid x | y << 16, z | cell px << 16 }
// P[1] = { asuint(far m), asuint(k), asuint(b), asuint(shadow texels per cell) }
// P[2] = { depth pyramid SRV (UNX_NONE: every cell), VSM stats UAV (raw; error word), 0, 0 }
// Frame constants of the main view.
#include "Bindless.hlsli"
#include "Frame.hlsli"
#include "Passes/Atmosphere/AtmosphereCommon.hlsli"
#include "Passes/Atmosphere/FroxelCommon.hlsli"
#include "Passes/Shadow/VsmAir.hlsli"
#include "Passes/Atmosphere/FogVolume.hlsli"

[numthreads(4, 4, 4)]
void main(uint3 id : SV_DispatchThreadID)
{
    const FogGrid g = fogGrid(uint4(P[0].z, P[0].w, P[1].x, P[1].y), P[1].z);
    if (any(id >= uint3(g.x, g.y, g.z))) return;
    float farthest = g.farM;
    if (P[2].x != 0xFFFFFFFFu)
    {
        Texture2D<float> hiz = ResourceDescriptorHeap[P[2].x];
        farthest = min(farthest, g_nearPlane / max(hiz.Load(int3(id.xy, fogHizMip(g))), 1e-30));
    }
    const float z0 = fogDepthOfSlice(g, float(id.z)), z1 = fogDepthOfSlice(g, float(id.z) + 1.0);
    if (z0 >= farthest) return;
    ConstantBuffer<VsmConstants> c = ResourceDescriptorHeap[P[0].y];
    uint k;
    if (!vsmAirLevel(c, fogCellWidth(g, 0.5 * (z0 + z1)), asfloat(P[1].w), k)) return;
    const float za = max(fogDepthOfSlice(g, float(id.z) - 0.5), 0.0), zb = min(fogDepthOfSlice(g, float(id.z) + 1.5), farthest);
    if (!(zb > za)) return;
    const float3 ray = froxelRayAt((float2(id.xy) + 0.5) * float(g.cellPx));
    const float3 p0 = vsmLightSpaceAt(c, g_cameraPosition + ray * za, k), p1 = vsmLightSpaceAt(c, g_cameraPosition + ray * zb, k);
    const float texel = vsmTexel(k);
    const float2 A = p0.xy / texel, D = (p1.xy - p0.xy) / texel;
    RWByteAddressBuffer requests = ResourceDescriptorHeap[P[0].x];
    VsmAirWalk w = vsmAirWalkBegin(A, D, VSM_PAGE, 0, 1);
    float ta, tb;
    int2 page;
    [loop] for (uint guard = 0; guard < 64 && vsmAirWalkNext(w, ta, tb, page); ++guard)
        if (vsmInWindow(c, page, k)) requests.InterlockedOr(vsmSlot(page, k) * 4, VSM_REQ_PROPAGATED | VSM_REQ_AIR);
    if (w.t < w.tEnd)  // (a cell's segment is a few texels of its level: the cap is far above it)
    {
        RWByteAddressBuffer stats = ResourceDescriptorHeap[P[2].y];
        stats.InterlockedOr(VSM_STATS_ERROR_BYTE, VSM_ERR_MARK_AIR_WALK);
    }
}
