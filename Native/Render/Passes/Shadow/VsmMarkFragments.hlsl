// unx-kernel: cs_6_6 main
// Page requests of the coverage layer's fragments (S request 20260926_S_fragment_visibility): for every main-view pixel
// with coverage records, the pages its fragment segment crosses on the levels its records are looked up on - the same
// pieces as vsmFragmentSegmentClassify (the view ray from the nearest to the farthest record, cut where the footprint
// level changes) walked page by page on their level. Requested like pixel pages (VSM_REQ_PIXEL: the propagation adds the
// neighbours the blocker search reaches), so the per-record SMRT and the classification read the level they ask for.
// P[0].x depth range SRV (R32G32_UINT, ViewResources::coverageDepthRange), P[0].y requests UAV (raw), P[0].z VSM constants
// CBV, P[0].w VSM stats UAV (raw; error word VSM_STATS_ERROR_BYTE). Frame constants of the main view.
#include "Frame.hlsli"
#include "Passes/Shadow/VsmAir.hlsli"

#define FRAGMENT_WALK_CAP 256u

[numthreads(8, 8, 1)]
void main(uint2 px : SV_DispatchThreadID)
{
    if (px.x >= g_viewWidth || px.y >= g_viewHeight) return;
    Texture2D<uint2> ranges = ResourceDescriptorHeap[P[0].x];
    const uint2 range = ranges.Load(int3(px, 0));
    if (range.x == 0 && range.y == 0xFFFFFFFFu) return;
    ConstantBuffer<VsmConstants> c = ResourceDescriptorHeap[P[0].z];
    RWByteAddressBuffer requests = ResourceDescriptorHeap[P[0].y];
    const float dNear = asfloat(range.x), dFar = asfloat(range.y);
    const float3 p0 = worldFromDepth(float2(px), dNear), p1 = worldFromDepth(float2(px), dFar);
    const float z0 = linearDepth(dNear), z1 = linearDepth(dFar), pixelScale = 2 * g_tanHalfFovY / g_viewHeight;
    uint k = vsmLevelForFootprint(c, z0 * pixelScale);
    float z = z0;
    uint steps = 0;
    bool capped = false;
    [loop] for (uint guard = 0; guard < VSM_LEVELS; ++guard)
    {
        const float zEnd = min(z1, vsmFragmentLevelEnd(c, k, pixelScale) * 1.0001);
        const float3 a = z1 > z0 ? lerp(p0, p1, (z - z0) / (z1 - z0)) : p0, b = z1 > z0 ? lerp(p0, p1, (zEnd - z0) / (z1 - z0)) : p1;
        const float texel = vsmTexel(k);
        const float2 A = vsmLightSpaceAt(c, a, k).xy / texel, D = (vsmLightSpaceAt(c, b, k).xy - vsmLightSpaceAt(c, a, k).xy) / texel;
        VsmAirWalk w = vsmAirWalkBegin(A, D, VSM_PAGE, 0, 1);
        float ta, tb;
        int2 page;
        [loop] for (; steps < FRAGMENT_WALK_CAP && vsmAirWalkNext(w, ta, tb, page); ++steps)
            if (vsmInWindow(c, page, k)) requests.InterlockedOr(vsmSlot(page, k) * 4, VSM_REQ_PIXEL);
        if (w.t < w.tEnd) capped = true;
        if (zEnd >= z1 || capped) break;
        z = zEnd / 1.0002;
        ++k;
    }
    if (capped)
    {
        RWByteAddressBuffer stats = ResourceDescriptorHeap[P[0].w];
        stats.InterlockedOr(VSM_STATS_ERROR_BYTE, VSM_ERR_MARK_FRAGMENT_WALK);
    }
}
