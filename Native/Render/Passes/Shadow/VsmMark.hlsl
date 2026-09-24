// unx-kernel: cs_6_6 main
// Page requests from a view's depth (ARCHITECTURE 2.3): every visible surface requests the page of the finest level
// whose texel is not larger than its pixel footprint. Duplicate requests within a wave store once.
// P[0].x depth SRV (Texture2D<float>), P[0].y requests UAV (raw), P[0].z VSM constants SRV (raw), P[0].w constants offset
// Frame constants of the view.
#include "Frame.hlsli"
#include "Passes/Shadow/VsmCommon.hlsli"

[numthreads(8, 8, 1)]
void main(uint2 px : SV_DispatchThreadID)
{
    if (px.x >= g_viewWidth || px.y >= g_viewHeight) return;
    Texture2D<float> depthTex = ResourceDescriptorHeap[P[0].x];
    const float depth = depthTex.Load(int3(px, 0));
    if (depth <= 0) return;  // sky
    const VsmConstants c = vsmLoadConstants(P[0].z, P[0].w);
    const float3 world = worldFromDepth(float2(px), depth);
    const float footprint = 2 * linearDepth(depth) * g_tanHalfFovY / g_viewHeight;
    const uint k = vsmLevelForFootprint(c, footprint);
    const float3 ls = vsmLightSpace(c, world);
    const int2 page = vsmAbsPage(vsmAbsTexel(c, ls.xy, k));
    if (!vsmInWindow(c, page, k)) return;
    const uint slot = vsmSlot(page, k);
    if (slot != WaveReadLaneFirst(slot) || WaveIsFirstLane())
    {
        RWByteAddressBuffer requests = ResourceDescriptorHeap[P[0].y];
        requests.Store(slot * 4, VSM_REQ_PIXEL);
    }
}
