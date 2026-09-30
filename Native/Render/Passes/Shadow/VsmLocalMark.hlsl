// unx-kernel: cs_6_6 main
// Page requests of the local-light shadows from a view's depth (ARCHITECTURE 2.4): every visible surface requests, for
// each shadowed light of its froxel list that reaches it, the page under it at the mip whose texel is not larger than
// its pixel footprint (vsmLocalMip), and at the two coarser mips (the estimator's wide taps fall back to them); at each
// of these mips also the pages half a page away in the four face directions, re-projected through the cube (a tap near
// a face edge reads the neighbouring face). Of each axis's two half-page neighbours one is always the centre's own page
// (vsmHalfStepStays): only the other is requested - the same requests; the centre is projected once for the three mips,
// and the other neighbour is projected only near a page edge (vsmHalfStepCrosses: elsewhere its page is the next one).
// Duplicate requests within a wave store once.
// P[0].x depth SRV (Texture2D<float>), P[0].y requests UAV (raw), P[0].z local lights SRV, P[0].w slot of light SRV
// (StructuredBuffer<uint>: scene light -> shadow slot or VSM_LOCAL_NONE)
// P[1] = FroxelSrvs (lights, lightIndices, scattering, pad). Frame constants of the view.
#include "Frame.hlsli"
#include "Scene.hlsli"
#include "Passes/Atmosphere/Froxel.hlsli"
#include "Passes/Shadow/VsmLocal.hlsli"

// The request of page 'page' of the face at mip.
void requestPage(RWByteAddressBuffer requests, uint light, uint face, uint mip, uint2 page)
{
    const uint at = vsmLocalSlot(light, face, mip, page);
    // Duplicate requests within the wave's active lanes store once (VsmMark): the same word, the same value.
    if (at != WaveReadLaneFirst(at) || WaveIsFirstLane()) requests.Store(at * 4, VSM_REQ_PIXEL);
}
// The request of the projected point q at mip; returns the page's continuous coordinates (texel / 128) on its face.
float2 requestAt(RWByteAddressBuffer requests, uint light, VsmLocalPoint q, uint mip)
{
    const float2 t = vsmLocalTexel(q.xy, mip) / VSM_PAGE;
    requestPage(requests, light, q.face, mip, min(uint2(t), (1u << mip) - 1));
    return t;
}
// The request of direction 'dir' (unit depth) at mip.
float2 request(RWByteAddressBuffer requests, VsmLocalLight l, uint light, float3 dir, uint mip)
{
    return requestAt(requests, light, vsmLocalProject(l, l.position + dir), mip);
}
// Whether moving by half a page from page coordinate u towards 'sign' stays in the same page with a margin: that
// neighbour's request is the centre's and is skipped. Half a page to one side always stays in the page (the side away
// from the nearer edge), so each axis requests at most one neighbour; near an edge (within the margin) both are requested
// as before. The margin bounds the rounding of the re-projection: vsmLocalProject subtracts the light's position from
// position + direction (absolute error ~ ulp(|position|) in tangent units), which is 2^mip / 2 pages per tangent unit:
// margin = 1/1024 + 2^mip x 4 ulp(max |position|) (at mip 6 and 1000 m from the origin: 1/1024 + 0.016).
float vsmMarkMargin(float3 position, uint mip)
{
    const float a = max(abs(position.x), max(abs(position.y), abs(position.z)));
    const float ulp = asfloat(asuint(max(a, 1.0)) + 1u) - max(a, 1.0);
    return 1.0 / 1024.0 + float(1u << mip) * 4 * ulp;
}
bool vsmHalfStepStays(float u, float sign, float margin)
{
    const float f = frac(u);
    return sign > 0 ? f + 0.5 < 1.0 - margin : f - 0.5 > margin;
}
// The other case of the same bound: half a page towards 'sign' crosses into the next page with the margin (and that page
// is on the face), while the other axis stays inside its page with the margin: the re-projected neighbour is that page
// (the re-projection moves the point by half a page along the axis up to the rounding the margin bounds), so it is
// requested without projecting. Within the margin of any page edge, or at the face's edge, it is projected as before.
bool vsmHalfStepCrosses(float u, float sign, float margin, uint page, uint pages)
{
    const float f = frac(u);
    return sign > 0 ? f + 0.5 > 1.0 + margin && page + 1 < pages : f - 0.5 < -margin && page > 0;
}
bool vsmInsidePage(float u, float margin)
{
    const float f = frac(u);
    return f > margin && f < 1.0 - margin;
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
    const float footprint = outputPixelFootprint(z);
    FroxelSrvs f;
    f.lights = P[1].x;
    f.lightIndices = P[1].y;
    f.scattering = P[1].z;
    f.pad = 0;
    const uint2 range = froxelLightRange(f, px, z);
    const uint indexBase = froxelIndexBase(f);
    if (range.y == 0) return;
    RWByteAddressBuffer requests = ResourceDescriptorHeap[P[0].y];
    StructuredBuffer<VsmLocalLight> lights = ResourceDescriptorHeap[P[0].z];
    StructuredBuffer<uint> slotOf = ResourceDescriptorHeap[P[0].w];
    [loop] for (uint i = 0; i < range.y; ++i)
    {
        const uint slot = slotOf[froxelLightAt(f, indexBase, range.x + i)];
        if (slot == VSM_LOCAL_NONE) continue;
        const VsmLocalLight l = lights[slot];
        const float3 d = world - l.position;
        if (dot(d, d) >= l.farM * l.farM) continue;
        const VsmLocalPoint q = vsmLocalProject(l, world);
        if (q.z <= l.nearM) continue;
        float3 right, up, axis;
        vsmCubeBasis(q.face, right, up, axis);
        const uint m = vsmLocalMip(footprint, q.z);
        const float3 c = axis + q.xy.x * right + q.xy.y * up;  // direction at unit depth
        const VsmLocalPoint qc = vsmLocalProject(l, l.position + c);  // (the same projection at every mip)
        [unroll] for (uint j = 0; j < 3; ++j)
        {
            if (j > m) break;
            const uint mip = m - j;
            // Half a page of mip in tangent units: 128 / res x 2 / 2.
            const float h = float(VSM_PAGE) / vsmLocalRes(mip);
            const float2 t = requestAt(requests, slot, qc, mip);
            const float margin = vsmMarkMargin(l.position, mip);
            // (texel x grows with right, texel y grows against up: vsmLocalTexel)
            const uint pages = 1u << mip;
            const uint2 page = min(uint2(t), pages - 1);
            const bool insideX = vsmInsidePage(t.x, margin), insideY = vsmInsidePage(t.y, margin);
            if (!vsmHalfStepStays(t.x, 1, margin))
            {
                if (insideY && vsmHalfStepCrosses(t.x, 1, margin, page.x, pages)) requestPage(requests, slot, qc.face, mip, uint2(page.x + 1, page.y));
                else request(requests, l, slot, c + h * right, mip);
            }
            if (!vsmHalfStepStays(t.x, -1, margin))
            {
                if (insideY && vsmHalfStepCrosses(t.x, -1, margin, page.x, pages)) requestPage(requests, slot, qc.face, mip, uint2(page.x - 1, page.y));
                else request(requests, l, slot, c - h * right, mip);
            }
            if (!vsmHalfStepStays(t.y, -1, margin))
            {
                if (insideX && vsmHalfStepCrosses(t.y, -1, margin, page.y, pages)) requestPage(requests, slot, qc.face, mip, uint2(page.x, page.y - 1));
                else request(requests, l, slot, c + h * up, mip);
            }
            if (!vsmHalfStepStays(t.y, 1, margin))
            {
                if (insideX && vsmHalfStepCrosses(t.y, 1, margin, page.y, pages)) requestPage(requests, slot, qc.face, mip, uint2(page.x, page.y + 1));
                else request(requests, l, slot, c - h * up, mip);
            }
        }
    }
}
