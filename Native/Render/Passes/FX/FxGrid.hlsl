// unx-kernel: cs_6_6 main
// unx-variants: STEP=0,1,2,3
// Collision candidate grid of the tick's surfaces (Particles.hlsli, fxSurfaceQuery): a spatial hash of cell g_gridCell
// with g_gridMask + 1 buckets. Surfaces are the anchor-space records FxSurfaces wrote.
//   STEP=0 clear: thread per bucket (count, fill = 0); thread 0 clears the large-list and entry counters.
//   STEP=1 count: thread per surface: inflated AABB -> cells; > FX_GRID_SURFACE_CELLS cells -> large list, else
//          count[bucket of each cell] += 1 (and the entry total).
//   STEP=2 scan: one group, exclusive prefix of count -> start.
//   STEP=3 fill: thread per surface of the grid: entries[start + atomic fill] = surface (order inside a bucket is free:
//          the candidates' order never changes a result).
// The AABB is inflated by max(1 mm, 1e-5 |coordinate|), so a point the float hit test accepts is inside it.
// Every loop is bounded by the counts; entries beyond the capacity (surfaces x 64) cannot occur.
#include "Passes/FX/Particles.hlsli"

void surfaceBox(uint i, out float3 lo, out float3 hi)
{
    FX_RWBUFFER(StreamSurface, surfaces, g_tickSurfaces);
    const StreamSurface s = surfaces[i];
    if (s.kind == 0u) { lo = s.a - s.radius; hi = s.a + s.radius; }
    else if (s.kind == 1u) { lo = min(s.a, s.b) - s.radius; hi = max(s.a, s.b) + s.radius; }
    else { lo = min(s.a, min(s.b, s.c)); hi = max(s.a, max(s.b, s.c)); }
    const float3 pad = max(1e-3f, 1e-5f * max(abs(lo), abs(hi)));
    lo -= pad;
    hi += pad;
}

#if STEP == 2
groupshared uint gs_partial[1024];
#endif

[numthreads(STEP == 2 ? 1024 : 64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 gtid : SV_GroupThreadID)
{
    FX_RWBUFFER(uint, counts, g_gridCount);
    FX_RWBUFFER(uint, fills, g_gridFill);
    FX_RWBUFFER(uint, counters, g_counters);
#if STEP == 0
    if (id.x == 0u) { counters[FX_COUNTER_LARGE] = 0u; counters[FX_COUNTER_GRID_ENTRIES] = 0u; }
    if (id.x <= g_gridMask) { counts[id.x] = 0u; fills[id.x] = 0u; }
#elif STEP == 1 || STEP == 3
    if (id.x >= g_surfaceCount) return;
    float3 lo, hi;
    surfaceBox(id.x, lo, hi);
    int3 a, span;
    const uint cells = fxGridBox(lo, hi, FX_GRID_SURFACE_CELLS, a, span);  // 0: large or not finite -> large list
    const bool small = cells != 0u;
#if STEP == 1
    if (!small)
    {
        uint at;
        InterlockedAdd(counters[FX_COUNTER_LARGE], 1u, at);
        FX_RWBUFFER(uint, large, g_gridLarge);
        large[at] = id.x;
        return;
    }
    InterlockedAdd(counters[FX_COUNTER_GRID_ENTRIES], cells);
    for (uint k = 0u; k < cells; ++k) InterlockedAdd(counts[fxGridHash(fxGridCellOf(a, span, k)) & g_gridMask], 1u);
#else
    if (!small) return;
    FX_RWBUFFER(uint, starts, g_gridStart);
    FX_RWBUFFER(uint, entries, g_gridEntries);
    for (uint k = 0u; k < cells; ++k)
    {
        const uint bucket = fxGridHash(fxGridCellOf(a, span, k)) & g_gridMask;
        uint j;
        InterlockedAdd(fills[bucket], 1u, j);
        const uint at = starts[bucket] + j;
        if (at < g_gridEntryCapacity) entries[at] = id.x;
        else fxStatus(FX_STATUS_RANGE);
    }
#endif
#else  // STEP == 2
    FX_RWBUFFER(uint, starts, g_gridStart);
    const uint t = gtid.x, buckets = g_gridMask + 1u, per = (buckets + 1023u) / 1024u;
    uint local = 0u;
    for (uint k = 0u; k < per; ++k) { const uint at = t * per + k; if (at < buckets) local += counts[at]; }
    gs_partial[t] = local;
    GroupMemoryBarrierWithGroupSync();
    for (uint s = 1u; s < 1024u; s <<= 1)
    {
        const uint v = t >= s ? gs_partial[t - s] : 0u;
        GroupMemoryBarrierWithGroupSync();
        gs_partial[t] += v;
        GroupMemoryBarrierWithGroupSync();
    }
    uint run = gs_partial[t] - local;
    for (uint k = 0u; k < per; ++k) { const uint at = t * per + k; if (at < buckets) { starts[at] = run; run += counts[at]; } }
#endif
}
