// unx-kernel: cs_6_6 main
// Readers of the air volume per froxel tile of a view (FroxelIntegrate.hlsl integrates only the slices a reader
// reaches): the farthest surface's view depth among the tile's pixels (0: none) and whether a sky pixel is among them
// (depth 0, reversed Z: the sky correction slice is read there). One 64-thread group per tile.
// Planar reflection views: only mirror pixels (ViewDesc::planarMask, v1.22) are read; the others are neither surface nor sky.
// P[0].x depth SRV, P[0].y output UAV (RWTexture2D<float2>: max linear depth, 1 = sky), P[0].z tile px, P[0].w planar
// mask SRV (R8_UINT; 0xFFFFFFFF: every pixel), P[1].x planar tile mask SRV (R8_UINT per 8 x 8 pixels, optional: pixels
// of tiles without mirror pixels are not read).
// Frame constants of the view.
#include "Bindless.hlsli"
#include "Frame.hlsli"

groupshared uint gs_maxZ, gs_sky;

[numthreads(64, 1, 1)]
void main(uint3 gid : SV_GroupID, uint t : SV_GroupIndex)
{
    if (t == 0)
    {
        gs_maxZ = 0;
        gs_sky = 0;
    }
    GroupMemoryBarrierWithGroupSync();
    Texture2D<float> depth = ResourceDescriptorHeap[P[0].x];
    const uint tp = P[0].z;
    float zmax = 0;
    uint sky = 0;
    for (uint i = t; i < tp * tp; i += 64)
    {
        const uint2 px = gid.xy * tp + uint2(i % tp, i / tp);
        if (px.x >= g_viewWidth || px.y >= g_viewHeight) continue;
        if (P[1].x != 0xFFFFFFFFu)
        {
            Texture2D<uint> tiles = ResourceDescriptorHeap[P[1].x];
            if (tiles.Load(int3(px / 8, 0)) == 0) continue;
        }
        if (P[0].w != 0xFFFFFFFFu)
        {
            Texture2D<uint> mask = ResourceDescriptorHeap[P[0].w];
            if (mask.Load(int3(px, 0)) == 0) continue;
        }
        const float d = depth.Load(int3(px, 0));
        if (d <= 0) sky = 1;
        else zmax = max(zmax, linearDepth(d));
    }
    InterlockedMax(gs_maxZ, asuint(zmax));  // non-negative floats order as uints
    if (sky) InterlockedOr(gs_sky, 1u);
    GroupMemoryBarrierWithGroupSync();
    if (t == 0)
    {
        RWTexture2D<float2> output = ResourceDescriptorHeap[P[0].y];
        output[gid.xy] = float2(asfloat(gs_maxZ), (float)gs_sky);
    }
}
