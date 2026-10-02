// unx-kernel: cs_6_6 main
// unx-variants: STEP=0,1
// Test kernels of the particle render pass (ParticleLayerTests.cpp), thread per full-resolution pixel:
//   STEP=0 reference: every record of the pixel's tile list that covers the pixel centre and is in front of the pixel's
//          opaque depth, sorted per pixel (insertion sort of up to 128; more sets the reference overflow bit in P[0].z's
//          buffer), composited front to back at full resolution -> RGBA32F (L, T). Independent of the tile kernel's
//          sort, 1/4 layer and edge classification; shares the records, the tile lists and the sample function (sprite
//          profile, strip hit).
//   STEP=1 compose: the pass's output at the pixel through fxParticleLayerAt (the function M's shading calls) -> RGBA32F.
// P[0] = (LayerConstants, output UAV, flags UAV (uint), 0). Records without a look (the round profile, strips): a
// look's sprites are not in the reference.
#include "Passes/FX/ParticleLayerPass.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const LayerConstants c = fxLayerConstants();
    if (id.x >= g_viewWidth || id.y >= g_viewHeight) return;
    RWTexture2D<float4> output = ResourceDescriptorHeap[P[0].y];
#if STEP == 0
    RWStructuredBuffer<uint> counts = ResourceDescriptorHeap[c.tileCounts];
    RWStructuredBuffer<uint> starts = ResourceDescriptorHeap[c.tileStarts];
    RWStructuredBuffer<uint2> entries = ResourceDescriptorHeap[c.entries];
    RWStructuredBuffer<LayerRecord> records = ResourceDescriptorHeap[c.records];
    Texture2D<float> depth = ResourceDescriptorHeap[c.depth];
    const uint2 tile2 = id.xy / (FX_LAYER_TILE * FX_LAYER_SCALE);
    const uint tile = tile2.y * c.tilesX + tile2.x;
    const float2 p = float2(id.xy) + 0.5f;
    const float d = depth.Load(int3(id.xy, 0));
    uint idx[128];
    float key[128];
    uint n = 0u;
    const uint count = counts[tile], start = starts[tile];
    for (uint i = 0u; i < count; ++i)
    {
        const uint r = entries[start + i].y;
        const LayerRecord rec = records[r];
        if (!(rec.depth > d)) continue;
        float a0, depth0;
        float3 unused;
        if (!fxLayerSample(c, rec, p, a0, unused, depth0) || !(depth0 > d)) continue;
        if (n == 128u)
        {
            RWStructuredBuffer<uint> flags = ResourceDescriptorHeap[P[0].z];
            InterlockedOr(flags[0], 1u);
            break;
        }
        // insertion: nearer first, ties by record index (the tile kernel's order)
        uint k = n;
        while (k > 0u && (key[k - 1u] < rec.depth || (key[k - 1u] == rec.depth && idx[k - 1u] > r)))
        {
            key[k] = key[k - 1u];
            idx[k] = idx[k - 1u];
            --k;
        }
        key[k] = rec.depth;
        idx[k] = r;
        ++n;
    }
    float3 L = 0;
    float T = 1;
    for (uint j = 0u; j < n; ++j)
    {
        const LayerRecord rec = records[idx[j]];
        float a, sampleDepth;
        float3 colour;
        fxLayerSample(c, rec, p, a, colour, sampleDepth);
        L += T * a * colour;
        T *= 1.0f - a;
    }
    output[id.xy] = float4(L, T);
#else
    Texture2D<float4> layer = ResourceDescriptorHeap[c.layerSrv];
    ByteAddressBuffer edges = ResourceDescriptorHeap[c.edgeBlocksSrv];
    output[id.xy] = fxParticleLayerAt(layer, edges, id.xy);
#endif
}
