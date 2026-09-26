// unx-kernel: cs_6_6 main
// unx-variants: MODE=0,1
// Ocean edge pixels (v1.73, kind 4 of the water layer's handoff to W): pixels of the water layer whose vis id is the
// ocean (COV_OCEAN_ID) and that are an edge by the water-edge rule (CoverageLayer.hlsli coverageWaterEdge: a 3 x 3
// neighbour that is not the ocean or lies behind band A, or a depth bend), or that are not the ocean but have an ocean
// neighbour (the outline's outer side: the sea may cover part of the pixel without its centre). W solves their 32
// subsamples and appends coverage records (coverageAppend) before V's count.
//   MODE=0 (8 x 8 pixels per group): append qualifying pixels (y << 16 | x) after the 4-word header; the need counts in
//          the cull state word VS_OCEAN_EDGES (it may exceed the capacity: OVERFLOW_OCEAN_EDGES).
//   MODE=1 (1 thread): header { count clamped, (count + 63) / 64, 1, 1 }.
//   P[0].x waterVis SRV, P[0].y waterDepth SRV, P[0].z band A depth SRV, P[0].w list UAV (raw)
//   P[1].x cull state UAV (raw), capacity (pixels), width, height
#include "Frame.hlsli"
#include "Passes/Visibility/VisibilityCommon.hlsli"
#include "Passes/Visibility/CoverageTiles.hlsli"

bool isOcean(Texture2D<uint> vis, Texture2D<float> water, Texture2D<float> bandA, int2 q)
{
    return vis[q] == COV_OCEAN_ID && water[q] < g_nearPlane / max(bandA[q], 1e-30);
}

[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    RWByteAddressBuffer state = ResourceDescriptorHeap[P[1].x];
    RWByteAddressBuffer list = ResourceDescriptorHeap[P[0].w];
#if MODE == 1
    const uint count = min(state.Load(4 * VS_OCEAN_EDGES), P[1].y);
    list.Store4(0, uint4(count, (count + 63) / 64, 1, 1));
#else
    const uint width = P[1].z, height = P[1].w;
    bool edge = false;
    if (pixel.x < width && pixel.y < height)
    {
        Texture2D<uint> vis = ResourceDescriptorHeap[P[0].x];
        Texture2D<float> water = ResourceDescriptorHeap[P[0].y];
        Texture2D<float> bandA = ResourceDescriptorHeap[P[0].z];
        const int2 hi = int2(width, height) - 1;
        const bool self = isOcean(vis, water, bandA, int2(pixel));
        bool anyOcean = self, anyOther = false;
        float d[3][3] = { { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 } };
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x)
            {
                const int2 q = clamp(int2(pixel) + int2(x, y), 0, hi);
                const bool o = isOcean(vis, water, bandA, q);
                anyOcean = anyOcean || o;
                anyOther = anyOther || !o;
                d[y + 1][x + 1] = o ? water[q] : 0;
            }
        if (self)
        {
            const float c = d[1][1];
            edge = anyOther || abs(d[1][0] + d[1][2] - 2 * c) > 1e-3 * c || abs(d[0][1] + d[2][1] - 2 * c) > 1e-3 * c;
        }
        else
            edge = anyOcean;  // outer side of the outline
    }
    const uint slot = waveAppend(state, VS_OCEAN_EDGES, edge ? 1 : 0, P[1].y, OVERFLOW_OCEAN_EDGES);
    if (edge && slot < P[1].y) list.Store(4 * (4 + slot), pixel.y << 16 | pixel.x);
#endif
}
