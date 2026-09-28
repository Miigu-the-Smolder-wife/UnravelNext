// unx-kernel: cs_6_6 main
// Page requests of the local-light shadows from a view's depth (ARCHITECTURE 2.4): every visible surface requests, for
// each shadowed light of its froxel list that reaches it, the page under it at the mip whose texel is not larger than
// its pixel footprint (vsmLocalMip), and at the two coarser mips (the estimator's wide taps fall back to them); at each
// of these mips also the pages half a page away in the four face directions, re-projected through the cube (a tap near
// a face edge reads the neighbouring face). Of each axis's two half-page neighbours one is always the centre's own page
// (vsmHalfStepStays): only the other is projected and requested - the same requests, 3 projections per mip instead of 5.
// P[0].x depth SRV (Texture2D<float>), P[0].y requests UAV (raw), P[0].z local lights SRV, P[0].w slot of light SRV
// (StructuredBuffer<uint>: scene light -> shadow slot or VSM_LOCAL_NONE)
// P[1] = FroxelSrvs (lights, lightIndices, scattering, pad). Frame constants of the view.
#include "Frame.hlsli"
#include "Scene.hlsli"
#include "Passes/Atmosphere/Froxel.hlsli"
#include "Passes/Shadow/VsmLocal.hlsli"

// The request of direction 'dir' (unit depth) at mip; returns the page's continuous coordinates (texel / 128) on its face.
float2 request(RWByteAddressBuffer requests, VsmLocalLight l, uint light, float3 dir, uint mip)
{
    const VsmLocalPoint q = vsmLocalProject(l, l.position + dir);
    const float2 t = vsmLocalTexel(q.xy, mip) / VSM_PAGE;
    const uint2 page = min(uint2(t), (1u << mip) - 1);
    requests.Store(vsmLocalSlot(light, q.face, mip, page) * 4, VSM_REQ_PIXEL);
    return t;
}
// Whether moving by half a page from page coordinate u towards 'sign' stays in the same page with a margin (1/1024 of a
// page: far above the rounding of the re-projection): that neighbour's request is the centre's and is skipped. Half a
// page to one side always stays in the page (the side away from the nearer edge), so each axis requests at most one
// neighbour; near an edge (within the margin) both are requested as before.
bool vsmHalfStepStays(float u, float sign)
{
    const float f = frac(u);
    return sign > 0 ? f + 0.5 < 1.0 - 1.0 / 1024.0 : f - 0.5 > 1.0 / 1024.0;
}

[numthreads(8, 8, 1)]
void main(uint2 px : SV_DispatchThreadID)
{
    if (px.x >= g_viewWidth || px.y >= g_viewHeight) return;
    Texture2D<float> depthTex = ResourceDescriptorHeap[P[0].x];
    const float depth = depthTex.Load(int3(px, 0));
    if (depth <= 0) return;
    const float3 world = worldFromDepth(float2(px), depth);
    const float z = linearDepth(depth);
    const float footprint = 2 * z * g_tanHalfFovY / g_viewHeight;
    FroxelSrvs f;
    f.lights = P[1].x;
    f.lightIndices = P[1].y;
    f.scattering = P[1].z;
    f.pad = 0;
    const uint2 range = froxelLightRange(f, px, z);
    if (range.y == 0) return;
    RWByteAddressBuffer requests = ResourceDescriptorHeap[P[0].y];
    StructuredBuffer<VsmLocalLight> lights = ResourceDescriptorHeap[P[0].z];
    StructuredBuffer<uint> slotOf = ResourceDescriptorHeap[P[0].w];
    [loop] for (uint i = 0; i < range.y; ++i)
    {
        const uint slot = slotOf[froxelLight(f, range.x + i)];
        if (slot == VSM_LOCAL_NONE) continue;
        const VsmLocalLight l = lights[slot];
        const float3 d = world - l.position;
        if (dot(d, d) >= l.farM * l.farM) continue;
        const VsmLocalPoint q = vsmLocalProject(l, world);
        if (q.z <= l.nearM) continue;
        float3 right, up, axis;
        vsmCubeBasis(q.face, right, up, axis);
        const uint m = vsmLocalMip(footprint, q.z);
        [unroll] for (uint j = 0; j < 3; ++j)
        {
            if (j > m) break;
            const uint mip = m - j;
            // Half a page of mip in tangent units: 128 / res x 2 / 2.
            const float h = float(VSM_PAGE) / vsmLocalRes(mip);
            const float3 c = axis + q.xy.x * right + q.xy.y * up;  // direction at unit depth
            const float2 t = request(requests, l, slot, c, mip);
            // (texel x grows with right, texel y grows against up: vsmLocalTexel)
            if (!vsmHalfStepStays(t.x, 1)) request(requests, l, slot, c + h * right, mip);
            if (!vsmHalfStepStays(t.x, -1)) request(requests, l, slot, c - h * right, mip);
            if (!vsmHalfStepStays(t.y, -1)) request(requests, l, slot, c + h * up, mip);
            if (!vsmHalfStepStays(t.y, 1)) request(requests, l, slot, c - h * up, mip);
        }
    }
}
