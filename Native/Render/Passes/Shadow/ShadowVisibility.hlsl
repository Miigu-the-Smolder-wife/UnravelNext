// unx-kernel: cs_6_6 main
// Shadow visibility pass (ARCHITECTURE 2.11): a small kernel separate from shading, so the page-table -> pool
// dependent loads are hidden by occupancy. Writes 4 B per pixel (INTERFACES 7.3): slot 0 = sun (VsmSample.hlsli),
// slots 1-3 = the first three shadowed local lights of the pixel's froxel list (255 until local-light VSM lands).
// P[0].x depth SRV, P[0].y G-buffer SRV (RG32_UINT), P[0].z output UAV (R32_UINT), P[0].w VSM constants SRV (raw)
// P[1].x constants offset, P[1].y page table SRV (raw), P[1].z pool SRV (Texture2D<uint>), P[1].w page metadata SRV
// P[2].x blocker search taps, P[2].y penumbra filter taps. Frame constants of the view.
#include "Frame.hlsli"
#include "GBuffer.hlsli"
#include "Passes/Shadow/VsmSample.hlsli"

[numthreads(8, 8, 1)]
void main(uint2 px : SV_DispatchThreadID)
{
    if (px.x >= g_viewWidth || px.y >= g_viewHeight) return;
    Texture2D<float> depthTex = ResourceDescriptorHeap[P[0].x];
    RWTexture2D<uint> output = ResourceDescriptorHeap[P[0].z];
    const float depth = depthTex.Load(int3(px, 0));
    uint packed = 0xFFFFFFFFu;
    if (depth > 0)
    {
        Texture2D<uint2> gbuffer = ResourceDescriptorHeap[P[0].y];
        const GBufferSample g = decodeGBuffer(gbuffer.Load(int3(px, 0)));
        VsmResources r;
        r.table = ResourceDescriptorHeap[P[1].y];
        r.pool = ResourceDescriptorHeap[P[1].z];
        r.meta = ResourceDescriptorHeap[P[1].w];
        r.c = vsmLoadConstants(P[0].w, P[1].x);
        const float3 world = worldFromDepth(float2(px), depth);
        const float footprint = 2 * linearDepth(depth) * g_tanHalfFovY / g_viewHeight;
        const float sun = vsmSunVisibility(r, world, g.normal, footprint, r.c.tanSunRadius, P[2].x, P[2].y);
        packed = (uint)round(saturate(sun) * 255.0) | 0xFFFFFF00u;
    }
    output[px] = packed;
}
