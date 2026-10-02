// unx-kernel: cs_6_6 main
// unx-variants: STEP=0,1
// Particle render pass, ribbon strips (ParticleLayerPass.hlsli), thread per ribbon point of this frame:
//   STEP=0: every point's link = none (slots between ribbon ranges have no point; FxRibbon then links the points of each
//           range's strips);
//   STEP=1: a point linked to its previous point is one segment: its record at stripBase + point (fixed slots after the
//           sprite records, so the order is deterministic) - the screen square around the segment's four vertices (the
//           whole view when it crosses the near plane), device depth of its nearest vertex (the tile sort key), SMALL
//           unless both ends are at least FX_LAYER_MIN_RADIUS wide on screen (thinner strips take the full-resolution
//           walk) - and the tile counts of that square. With tessellation (LayerExtra::ribbonSegments > 1,
//           ParticleLayerPass.hlsli fxStripSampleOf) the segment is a cubic's pieces: the square also holds the cubic's
//           two inner control points (the ends moved a third of the segment along their tangents) with the ends' side
//           vectors - the curve lies within its control points' hull. The record takes its point's program and look; a
//           look's strip is drawn at full resolution unless the look is marked smooth.
#include "Passes/FX/ParticleLayerPass.hlsli"

[numthreads(256, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const LayerConstants c = fxLayerConstants();
    const uint k = id.x;
    if (k >= c.ribbonCapacity) return;
    RWStructuredBuffer<uint> links = ResourceDescriptorHeap[c.ribbonLinks];
#if STEP == 0
    links[k] = FX_NONE;
#else
    RWStructuredBuffer<LayerRecord> records = ResourceDescriptorHeap[c.records];
    LayerRecord rec = (LayerRecord)0;
    const uint j = links[k];
    if (j == FX_NONE || j >= c.ribbonCapacity)
    {
        records[c.stripBase + k] = rec;
        return;
    }
    RWStructuredBuffer<FxRibbonVertex> vertices = ResourceDescriptorHeap[c.ribbonVertices];
    float3 corners[8] = { vertices[2u * j].position, vertices[2u * j + 1u].position, vertices[2u * k].position, vertices[2u * k + 1u].position,
                          float3(0, 0, 0), float3(0, 0, 0), float3(0, 0, 0), float3(0, 0, 0) };
    const LayerExtra x = fxLayerExtra();
    uint cornerCount = 4u;
    if (x.ribbonSegments > 1u)
    {
        RWStructuredBuffer<float4> tangents = ResourceDescriptorHeap[x.ribbonTangents];
        const float3 ta = tangents[j].xyz * c.streamAxes, tb = tangents[k].xyz * c.streamAxes;
        if (dot(ta, tb) < cos(FX_RIBBON_PIECE_ANGLE))
        {
            const float3 third = (0.5f * (corners[2] + corners[3]) - 0.5f * (corners[0] + corners[1])) ;
            const float reach = length(third) / 3.0f;
            corners[4] = corners[0] + ta * reach;
            corners[5] = corners[1] + ta * reach;
            corners[6] = corners[2] - tb * reach;
            corners[7] = corners[3] - tb * reach;
            cornerCount = 8u;
        }
    }
    float2 lo = float2(1e30f, 1e30f), hi = float2(-1e30f, -1e30f);
    float nearest = 0;
    uint behind = 0u;
    float distances[8];
    [loop] for (uint q = 0u; q < cornerCount; ++q)
    {
        const float3 v = mul((float3x3)g_view, corners[q]);
        distances[q] = -v.z;
        if (!(distances[q] > g_nearPlane)) { ++behind; continue; }
        const float4 clip = mul(g_proj, float4(v, 1));
        const float2 s = float2((clip.x / clip.w + 1) * 0.5f * g_viewWidth, (1 - clip.y / clip.w) * 0.5f * g_viewHeight);
        lo = min(lo, s);
        hi = max(hi, s);
        nearest = max(nearest, g_nearPlane / distances[q]);
    }
    if (behind == cornerCount)
    {
        records[c.stripBase + k] = rec;
        return;
    }
    if (behind > 0u)
    {
        // the segment crosses the near plane: its visible part can reach any pixel
        lo = float2(0, 0);
        hi = float2(g_viewWidth, g_viewHeight);
        nearest = 1.0f;
    }
    lo -= 1.0f;  // the pixel-centre sample of a pixel the segment's edge touches
    hi += 1.0f;
    if (hi.x < 0 || hi.y < 0 || lo.x > g_viewWidth || lo.y > g_viewHeight)
    {
        records[c.stripBase + k] = rec;
        return;
    }
    // half widths on screen at both ends (world width x the projection's pixels per unit at their distance)
    const float scale = g_proj[1][1] * 0.5f * g_viewHeight;
    const float hA = behind ? 0.0f : 0.5f * length(corners[1] - corners[0]) * scale / min(distances[0], distances[1]);
    const float hB = behind ? 0.0f : 0.5f * length(corners[3] - corners[2]) * scale / min(distances[2], distances[3]);
    rec.centre = 0.5f * (lo + hi);
    rec.radius = 0.5f * length(hi - lo);
    rec.depth = nearest;
    rec.radianceAlpha = uint2(k, j);
    // (the point's program and look, FxLayerSetup's ribbonPoint)
    RWStructuredBuffer<FxRibbonPoint> points = ResourceDescriptorHeap[c.ribbonPoints];
    const FxRibbonPoint own = points[k];
    bool small = min(hA, hB) < FX_LAYER_MIN_RADIUS;
    if (own.look != 0u && x.looks != UNX_NONE)
    {
        StructuredBuffer<FxSpriteLook> looks = ResourceDescriptorHeap[x.looks];
        small = small || (looks[own.look - 1u].flags & FX_LOOK_SMOOTH) == 0u;
    }
    rec.flags = FX_LAYER_RECORD_STRIP | (small ? FX_LAYER_RECORD_SMALL : 0u) | (own.look << 8);
    rec.program = own.program;
    records[c.stripBase + k] = rec;
    uint2 t0, t1;
    if (!fxLayerTiles(c, rec, t0, t1)) return;
    RWStructuredBuffer<uint> counts = ResourceDescriptorHeap[c.tileCounts];
    [loop] for (uint y = t0.y; y <= t1.y; ++y)
        [loop] for (uint x = t0.x; x <= t1.x; ++x) InterlockedAdd(counts[y * c.tilesX + x], 1u);
    const uint n = WaveActiveCountBits(true);
    RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[c.counters];
    if (WaveIsFirstLane()) InterlockedAdd(counters[FX_LAYER_COUNTER_DRAWN], n);
#endif
}
