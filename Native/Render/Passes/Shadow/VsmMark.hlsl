// unx-kernel: cs_6_6 main
// Page requests from a view's depth (ARCHITECTURE 2.3): every visible surface requests the page of the finest level
// whose texel is not larger than its pixel footprint. Duplicate requests within a wave store once.
// P[0].x depth SRV (Texture2D<float>), P[0].y requests UAV (raw), P[0].z VSM constants CBV, P[0].w 1: sub-tile statistics
// (shadow.vsm.subtile_stats, measurement only: each pixel also sets bit 16 + its 32^2 sub-tile (4 x 4 per page) in the
// request word, VsmAllocate counts them)
// P[1].x page dilation (float bits; shadow.vsm.page_dilation, the reference's PageDilationBorderSizeDirectional): a pixel
// within this fraction of a page of its page's border also requests the page across it - the pages at -+ the border along
// one diagonal, the diagonal alternating by pixel parity, so every border and corner has pixels that look across it. The
// taps of a pixel near a page border then find the neighbour on the pixel's own level, not only its coarser ancestors.
// 0: the pixel's page alone.
// P[1].y 1: the depth is a layer's linear view depth, +inf where the layer has nothing (V's water layer: the water
// surface's points ask for their pages as the opaque surface under them does - the sun's glint on the water and what
// the water scatters read the shadow at the surface, which no opaque pixel may have asked for).
// Frame constants of the view.
#include "Frame.hlsli"
#include "Passes/Shadow/VsmCommon.hlsli"

[numthreads(8, 8, 1)]
void main(uint2 px : SV_DispatchThreadID)
{
    if (px.x >= g_viewWidth || px.y >= g_viewHeight) return;
    Texture2D<float> depthTex = ResourceDescriptorHeap[P[0].x];
    float depth = depthTex.Load(int3(px, 0));
    if (P[1].y != 0)
    {
        if (!(depth < 3.0e38)) return;  // no layer sample here
        depth = g_nearPlane / max(depth, 1e-30);  // (reversed-Z infinite: device depth of a view depth)
    }
    if (depth <= 0) return;  // sky
    ConstantBuffer<VsmConstants> c = ResourceDescriptorHeap[P[0].z];
    const float3 world = worldFromDepth(float2(px), depth);
    const float footprint = 2 * linearDepth(depth) * g_tanHalfFovY / g_viewHeight;
    const uint k = vsmLevelForFootprint(c, footprint);
    const float3 ls = vsmLightSpaceAt(c, world, k);
    const int2 page = vsmAbsPage(vsmAbsTexel(c, ls.xy, k));
    if (!vsmInWindow(c, page, k)) return;
    const uint slot = vsmSlot(page, k);
    const float border = asfloat(P[1].x);
    if (border > 0)
    {
        const float2 at = ls.xy / vsmPageSize(k);
        const float2 reach = border * float2((px.x & 1u) ? 1.0 : -1.0, (px.y & 1u) ? 1.0 : -1.0);
        [unroll] for (uint side = 0; side < 2; ++side)
        {
            const int2 across = int2(floor(side == 0 ? at + reach : at - reach));
            if (all(across == page) || !vsmInWindow(c, across, k)) continue;
            RWByteAddressBuffer requests = ResourceDescriptorHeap[P[0].y];
            // (the same word every pixel mark stores; the statistics mode ORs its bits in)
            if (P[0].w == 1) requests.InterlockedOr(vsmSlot(across, k) * 4, VSM_REQ_PIXEL);
            else requests.Store(vsmSlot(across, k) * 4, VSM_REQ_PIXEL);
        }
    }
    if (P[0].w == 1)
    {
        const uint2 local = uint2(vsmAbsTexel(c, ls.xy, k) & (int)(VSM_PAGE - 1)) >> 5;
        RWByteAddressBuffer requests = ResourceDescriptorHeap[P[0].y];
        requests.InterlockedOr(slot * 4, VSM_REQ_PIXEL | (1u << (16 + local.y * 4 + local.x)));
        return;
    }
    if (slot != WaveReadLaneFirst(slot) || WaveIsFirstLane())
    {
        RWByteAddressBuffer requests = ResourceDescriptorHeap[P[0].y];
        requests.Store(slot * 4, VSM_REQ_PIXEL);
    }
}
