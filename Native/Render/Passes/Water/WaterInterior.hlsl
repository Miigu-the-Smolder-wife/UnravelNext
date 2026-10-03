// unx-kernel: cs_6_6 main
// Water layer interior (W, FEATURES_GAME 1.9 stage 1): every pixel whose water-layer sample (waterVis: a triangle
// stream, slots 0..62) covers the pixel whole - not a water edge by V's rule - gets the water surface's radiance
// (WaterSurface.hlsli) in place of band A: in ViewResources::bandARadiance (what the composites put behind fragments over
// the water) and, with FX's particle layer over it (shParticles), in the shaded colour (exposed linear float; INTERFACES
// v1.75). Edge pixels keep band A: M's composite adds their water records (WaterRecords.hlsl) over it.
// P[0] colour UAV, source SRV (band A copy), band A depth SRV, statistics UAV (raw, WATER_STAT_COUNT words; UNX_NONE)
// P[1] waterVis SRV, waterDepth SRV, slot table SRV (WaterSurface.hlsli), GI cache SRV (UNX_NONE)
// P[2] atmosphere transmittance, multi-scatter, air volume (UNX_NONE: no air), status UAV (tests: R8_UINT per pixel,
//      the sample's WATER_STAT_* + 1; UNX_NONE)
// P[3] VSM page table (UNX_NONE: no sun shadow), blocks, search bound, constants CBV; P[4] VSM transmittance layers,
// debug image UAV (tests: RGBA32F, WaterSurface.hlsli g_waterMarchDebug; UNX_NONE)
// P[5] bandARadiance UAV (RGBA16F), particle layer SRV, particle edges SRV (UNX_NONE: no particle layer)
// P[6] stage 3 (UNX_NONE: none): jobs UAV (raw, FrameServices::traceRefractions: head 16 B { count, x, y, z }, 48 B per
//      job), results UAV (raw, 8 B per job), samples UAV (raw: head 16 B { count }, 64 B per sample, WaterRayApply.hlsl),
//      job capacity; P[7] first row of the band, rows in it, sample capacity, bit 0: the layer's edge pixels are shaded
//      too (a view without coverage records - a planar reflection view: one sample for a pixel the water covers in part)
// With stage 3 the pass runs once per band of rows; each interior sample writes its stage 1 value as without it, and its
// jobs (a reflection job where the surface's lobe is sharper than the GI cache, a refraction job for a fallback) with
// their results zeroed (alpha 0: not traced, the stage 1 value stays) and one sample record for the apply pass.
#include "WaterSurface.hlsli"

// V's water-edge rule (CoverageLayer.hlsli coverageWaterEdge, v1.64) for the stream `slot`: a pixel is an edge when in
// its 3 x 3 block (clamped) some pixel is not this stream's water in front of band A, or the water depth bends (second
// difference along x or y above 1e-3 of the depth). The same expression, so interior and edge pixels partition exactly.
bool waterEdge(Texture2D<uint> vis, Texture2D<float> water, Texture2D<float> bandA, uint2 pixel, uint slot)
{
    const int2 hi = int2(g_viewWidth, g_viewHeight) - 1;
    float d[3][3];
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            const int2 q = clamp(int2(pixel) + int2(x, y), 0, hi);
            const uint v = vis[q];
            if ((v >> 30) != 3u || ((v >> 24) & 0x3Fu) != slot) return true;
            const float w = water[q];
            if (!(w < g_nearPlane / max(bandA[q], 1e-30))) return true;
            d[y + 1][x + 1] = w;
        }
    const float c = d[1][1];
    return abs(d[1][0] + d[1][2] - 2 * c) > 1e-3 * c || abs(d[0][1] + d[2][1] - 2 * c) > 1e-3 * c;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint2 pixel = uint2(id.x, id.y + P[7].x);
    if (id.y >= P[7].y || any(pixel >= uint2(g_viewWidth, g_viewHeight))) return;
    Texture2D<uint> vis = ResourceDescriptorHeap[P[1].x];
    const uint v = vis[pixel];
    if ((v >> 30) != 3u) return;
    const uint slot = (v >> 24) & 0x3Fu, tri = v & 0xFFFFFFu;
    if (slot == 63u) return;  // the sea (COV_OCEAN_SLOT): its own surface, not a stream triangle
    Texture2D<float> water = ResourceDescriptorHeap[P[1].y];
    Texture2D<float> bandA = ResourceDescriptorHeap[P[0].z];
    if ((P[7].w & 1u) == 0)
    {
        if (waterEdge(vis, water, bandA, pixel, slot)) return;
    }
    else if (!(water[pixel] < g_nearPlane / max(bandA[pixel], 1e-30))) return;  // (the water behind band A)

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
    s.atm.skyView = UNX_NONE;
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
    const float3 radiance = waterSurfaceShade(s, pixel, slot, tri, stat, rays);
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
    if (P[4].y != UNX_NONE)
    {
        RWTexture2D<float4> debugImage = ResourceDescriptorHeap[P[4].y];
        debugImage[pixel] = g_waterMarchDebug;
    }
    if (P[2].w != UNX_NONE)
    {
        RWTexture2D<uint> status = ResourceDescriptorHeap[P[2].w];
        status[pixel] = stat + 1;
    }
}
