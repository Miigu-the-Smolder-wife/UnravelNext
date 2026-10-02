// unx-kernel: ms_6_6 main
// Clear of the atlas pages drawn this frame (VsmSystem.cpp, sun page cache): with pages kept from earlier frames the
// atlas is not cleared as a whole; each page of this frame's page list (VsmScan: the pages to draw) gets one quad at
// depth 0 (= no caster), depth test ALWAYS (the pipeline). 32 pages per group; groups past the list emit nothing.
// P[0] = { page list SRV (raw: count, pad, (slot, page) pairs), atlas width, atlas height (texels), 0 }
// P[1].x = a page's texels in this atlas (0: CLEAR_PAGE_TEXELS; the tint atlas: VSM_TINT_PAGE)
#include "Bindless.hlsli"

#define CLEAR_PAGES 32u
#define CLEAR_PAGE_TEXELS 128.0
#define CLEAR_PAGES_PER_ROW 128u

struct ClearVertex
{
    float4 position : SV_Position;
};

[outputtopology("triangle")]
[numthreads(CLEAR_PAGES, 1, 1)]
void main(uint tid : SV_GroupThreadID, uint3 gid : SV_GroupID, out vertices ClearVertex verts[4 * CLEAR_PAGES], out indices uint3 tris[2 * CLEAR_PAGES])
{
    ByteAddressBuffer list = ResourceDescriptorHeap[P[0].x];
    const uint count = list.Load(0), first = gid.x * CLEAR_PAGES;
    const uint n = first < count ? min(CLEAR_PAGES, count - first) : 0u;
    SetMeshOutputCounts(4 * n, 2 * n);
    if (tid >= n) return;
    const uint page = list.Load(8 + (first + tid) * 8 + 4);
    const float2 atlas = float2(P[0].yz);
    const float texels = P[1].x != 0 ? float(P[1].x) : CLEAR_PAGE_TEXELS;
    const float2 lo = float2(page % CLEAR_PAGES_PER_ROW, page / CLEAR_PAGES_PER_ROW) * texels;
    [unroll] for (uint c = 0; c < 4; ++c)
    {
        const float2 px = lo + float2(c & 1, c >> 1) * texels;
        ClearVertex v;
        v.position = float4(px.x / atlas.x * 2 - 1, 1 - px.y / atlas.y * 2, 0, 1);
        verts[4 * tid + c] = v;
    }
    tris[2 * tid] = uint3(4 * tid, 4 * tid + 1, 4 * tid + 2);
    tris[2 * tid + 1] = uint3(4 * tid + 1, 4 * tid + 3, 4 * tid + 2);
}
