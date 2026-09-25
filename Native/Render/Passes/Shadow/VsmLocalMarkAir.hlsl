// unx-kernel: cs_6_6 main
// Page requests of the air for the local lights (FroxelIntegrate's shadowed air in-scattering): for every froxel and
// every light of its list with a shadow slot (entry bit 15), the pages under the 24 angle midpoints the integration
// tests on the tile-centre segment (FroxelIntegrate.hlsl AIR_SHADOW_POINTS), at the mip whose texel matches the
// froxel's lateral resolution. One group per tile, one thread per depth slice. Runs after the froxel lists (shadowPages).
// P[0].x requests UAV (raw), P[0].y froxel lists SRV (raw), P[0].z local lights SRV, P[0].w slot of light SRV
// P[1].x shadow texels per tile (float bits). Frame constants of the main view.
#include "Bindless.hlsli"
#include "Frame.hlsli"
#include "Scene.hlsli"
#include "Passes/Atmosphere/FroxelCommon.hlsli"
#include "Passes/Shadow/VsmLocal.hlsli"

#define AIR_SHADOW_POINTS 24u

[numthreads(64, 1, 1)]
void main(uint3 gid : SV_GroupID, uint s : SV_GroupIndex)
{
    const FroxelGrid g = froxelGrid(P[0].y);
    if (s >= g.slices) return;
    const uint2 tile = gid.xy;
    ByteAddressBuffer lists = ResourceDescriptorHeap[P[0].y];
    const uint h = lists.Load(g.headerBase + froxelIndex(g, tile, s) * 4);
    const uint first = h >> 6, count = h & 63u;
    if (count == 0) return;
    RWByteAddressBuffer requests = ResourceDescriptorHeap[P[0].x];
    StructuredBuffer<VsmLocalLight> locals = ResourceDescriptorHeap[P[0].z];
    StructuredBuffer<uint> slotOf = ResourceDescriptorHeap[P[0].w];
    const float3 ray = froxelTileRay(g, tile);
    const float toRay = length(ray);
    const float3 dir = ray / toRay;
    const float z0 = froxelNodeDepth(g, s), z1 = froxelNodeDepth(g, s + 1);
    const float3 o = g_cameraPosition + dir * (z0 * toRay);
    const float len = (z1 - z0) * toRay;
    const float width = froxelTileWidth(g, 0.5 * (z0 + z1)) / asfloat(P[1].x);
    [loop] for (uint i = 0; i < count; ++i)
    {
        const uint w = lists.Load(g.indexBase + ((first + i) >> 1) * 4);
        const uint entry = ((first + i) & 1) ? w >> 16 : w & 0xFFFFu;
        if ((entry & 0x8000u) == 0) continue;
        const uint slot = slotOf[entry & 0x7FFFu];
        if (slot == VSM_LOCAL_NONE) continue;
        const VsmLocalLight l = locals[slot];
        const GpuLight gl = loadLight(entry & 0x7FFFu);
        const float tc = dot(l.position - o, dir);
        const float hd = max(length(l.position - (o + dir * tc)), max(max(gl.size.x, gl.size.y), 0.01));
        const float th0 = atan(-tc / hd), th1 = atan((len - tc) / hd);
        const float mid = 0.5 * (th0 + th1), half = 0.5 * (th1 - th0);
        [loop] for (uint k = 0; k < AIR_SHADOW_POINTS; ++k)
        {
            const float t = tc + hd * tan(mid + half * ((k + 0.5) / AIR_SHADOW_POINTS * 2 - 1));
            const VsmLocalPoint q = vsmLocalProject(l, o + dir * t);
            if (q.z <= l.nearM || q.z >= l.farM) continue;
            const uint m = vsmLocalMip(width, q.z);
            const uint2 page = min(uint2(vsmLocalTexel(q.xy, m)) >> VSM_PAGE_SHIFT, (1u << m) - 1);
            requests.Store(vsmLocalSlot(slot, q.face, m, page) * 4, VSM_REQ_PIXEL);
        }
    }
}
