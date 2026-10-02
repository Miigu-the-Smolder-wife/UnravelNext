// unx-kernel: ms_6_6 main
// The software rasteriser's pages into a raster request's atlas (visibility.software_raster; DepthRasterSw.hlsl): one
// quad per page over its tile's atlas slot, DSW_MERGE_PAGES pages per group; groups past the pages emit nothing. The
// pixel kernel (DepthSwMerge.ps) writes the page's depth under the depth test.
//   P[0] page slots SRV (raw: DSP_*), page capacity, tile px, atlas tiles per row
//   P[1] atlas size (w | h << 16), page depth SRV (raw; the pixel kernel), 0, 0
#include "Bindless.hlsli"
#include "Passes/Visibility/DepthRasterSw.hlsli"

struct VertexOut
{
    float4 position : SV_Position;
};

struct PrimitiveOut
{
    uint page : PAGE;
    uint2 origin : ORIGIN;  // the slot's first atlas pixel
};

[outputtopology("triangle")]
[numthreads(DSW_MERGE_PAGES, 1, 1)]
void main(uint lane : SV_GroupThreadID, uint3 group : SV_GroupID, out vertices VertexOut verts[4 * DSW_MERGE_PAGES],
          out primitives PrimitiveOut prims[2 * DSW_MERGE_PAGES], out indices uint3 tris[2 * DSW_MERGE_PAGES])
{
    ByteAddressBuffer slots = ResourceDescriptorHeap[P[0].x];
    const uint pages = min(slots.Load(4 * DSP_TILES), P[0].y), first = group.x * DSW_MERGE_PAGES;
    const uint n = first < pages ? min(DSW_MERGE_PAGES, pages - first) : 0u;
    SetMeshOutputCounts(4 * n, 2 * n);
    if (lane >= n) return;
    const uint page = first + lane, slot = slots.Load(4 * (DSP_FIRST + page));
    const uint tilePx = P[0].z;
    const uint2 origin = uint2(slot % P[0].w, slot / P[0].w) * tilePx;
    const float2 atlas = float2(P[1].x & 0xFFFFu, P[1].x >> 16);
    [unroll] for (uint c = 0; c < 4; ++c)
    {
        const float2 px = float2(origin) + float2(c & 1, c >> 1) * (float)tilePx;
        verts[4 * lane + c].position = float4(px.x / atlas.x * 2 - 1, 1 - px.y / atlas.y * 2, 1, 1);  // (the depth is the pixel kernel's)
    }
    tris[2 * lane] = uint3(4 * lane, 4 * lane + 1, 4 * lane + 2);
    tris[2 * lane + 1] = uint3(4 * lane + 1, 4 * lane + 3, 4 * lane + 2);
    [unroll] for (uint k = 0; k < 2; ++k)
    {
        prims[2 * lane + k].page = page;
        prims[2 * lane + k].origin = origin;
    }
}
