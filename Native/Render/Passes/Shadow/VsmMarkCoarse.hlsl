// unx-kernel: cs_6_6 main
// Coarse sun pages requested every frame (shadow.vsm.coarse_pages; the reference's coarse pages,
// r.Shadow.Virtual.MarkCoarsePagesDirectional: the clipmaps nest, so the pages at the centre of the coarse levels are a
// low-resolution copy of everything around the camera). On each level of [first, last] the n x n pages around the
// camera's position in the level's grid are requested (n = 2: the four pages the reference marks, at least half a page
// in every direction), whether a pixel asked for them or not. They are ordinary pages - assigned by the scan, kept by the
// page cache while no caster changes under them - so a lookup that finds no page on its own level (a frame whose
// requests ran past the atlas after a cut, a reflection or GI hit outside the view, a planar view) walks up to one of
// them (vsmHeightAt, vsmFetchQuad, shadowSunResidentLevel) and gets a shadow at that level's texel.
// One thread per (level, page). After the frame's other marks (it only ORs its bit in).
// P[0].x requests UAV (raw), P[0].y VSM constants CBV, P[0].z first level | last level << 8 | pages per axis << 16
#include "Passes/Shadow/VsmCommon.hlsli"

[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    const uint first = P[0].z & 0xFFu, last = min((P[0].z >> 8) & 0xFFu, VSM_LEVELS - 1), n = (P[0].z >> 16) & 0xFFu;
    if (n == 0 || first > last) return;
    const uint perLevel = n * n;
    if (i >= (last - first + 1) * perLevel) return;
    ConstantBuffer<VsmConstants> c = ResourceDescriptorHeap[P[0].y];
    const uint k = first + i / perLevel, j = i % perLevel;
    // The camera in pages of level k; the n pages nearest to it per axis start half an extent below it.
    const float2 camera = float2(c.level[k].cameraU, c.level[k].cameraV) / vsmPageSize(k);
    const int2 page = int2(floor(camera - 0.5 * float(n - 1))) + int2(j % n, j / n);
    if (!vsmInWindow(c, page, k)) return;
    RWByteAddressBuffer requests = ResourceDescriptorHeap[P[0].x];
    requests.InterlockedOr(vsmSlot(page, k) * 4, VSM_REQ_COARSE);
}
