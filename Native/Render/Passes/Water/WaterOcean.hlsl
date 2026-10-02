// unx-kernel: cs_6_6 main
// The sea in the water layer (W, B7; FEATURES_GAME 1.8 B, 1.9): every pixel whose water-layer sample is the view-grid
// ocean (waterVis = COV_OCEAN_ID, in front of band A) gets the sea surface's radiance (WaterSurface.hlsli's function with
// OceanShading.hlsli's point, normal and unresolved slopes) in ViewResources::bandARadiance and, with FX's particle
// layer over it, in the shaded colour - as WaterInterior.hlsl does for the streams' pixels. The sea has no edge records
// (V lists its edge pixels for a subsample pass that does not exist yet): every sea pixel is shaded here, one sample
// each.
// P[0] .. P[7]: WaterInterior.hlsl's (the same band of rows and ray lists: a pixel is a stream's or the sea's);
// P[5].w = the sky view LUT's SRV (UNX_NONE: the mirror lobe's stand-in is the GI source's).
#define WATER_OCEAN 1
#include "WaterSurface.hlsli"
#include "Passes/Visibility/CoverageTiles.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint2 pixel = uint2(id.x, id.y + P[7].x);
    if (id.y >= P[7].y || any(pixel >= uint2(g_viewWidth, g_viewHeight))) return;
    Texture2D<uint> vis = ResourceDescriptorHeap[P[1].x];
    if (vis[pixel] != COV_OCEAN_ID) return;
    Texture2D<float> water = ResourceDescriptorHeap[P[1].y];
    Texture2D<float> bandA = ResourceDescriptorHeap[P[0].z];
    if (!(water[pixel] < g_nearPlane / max(bandA[pixel], 1e-30))) return;

    WaterShadeSrvs s;
    s.source = P[0].y;
    s.bandADepth = P[0].z;
    s.statistics = P[0].w;
    s.waterVis = P[1].x;
    s.waterDepth = P[1].y;
    s.slots = P[1].z;
    s.giCache = P[1].w;
    s.pad = 0;
    s.atm.transmittance = P[2].x;
    s.atm.multiScatter = P[2].y;
    s.atm.skyView = P[5].w;
    s.atm.aerial = P[2].z;
    s.shadow.pageTable = P[3].x;
    s.shadow.pool = UNX_NONE;
    s.shadow.blocks = P[3].y;
    s.shadow.searchBound = P[3].z;
    s.shadow.constants = P[3].w;
    s.shadow.lights = UNX_NONE;
    s.shadow.pad0 = UNX_NONE;
    s.shadow.layers = P[4].x;
    uint stat;
    WaterRayTerms rays;
    const float3 radiance = waterSurfaceShade(s, pixel, COV_OCEAN_SLOT, 0, stat, rays);
    waterAppendRays(rays, pixel.x | (pixel.y << 16), P[0].w);
    if (P[5].x != UNX_NONE)
    {
        RWTexture2D<float4> bandARadiance = ResourceDescriptorHeap[P[5].x];
        bandARadiance[pixel] = float4(radiance, 1);
    }
    RWTexture2D<float4> colour = ResourceDescriptorHeap[P[0].x];
    colour[pixel] = float4(shParticles(radiance, pixel, P[5].y, P[5].z), 1);
    if (P[0].w != UNX_NONE)
    {
        RWByteAddressBuffer statistics = ResourceDescriptorHeap[P[0].w];
        statistics.InterlockedAdd(4 * stat, 1);
    }
    if (P[2].w != UNX_NONE)
    {
        RWTexture2D<uint> status = ResourceDescriptorHeap[P[2].w];
        status[pixel] = stat + 1;
    }
}
