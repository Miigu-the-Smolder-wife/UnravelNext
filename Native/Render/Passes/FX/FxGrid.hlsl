// unx-kernel: cs_6_6 main
// unx-variants: STEP=0,1,2,3,4
// Collision candidate grid of the tick's surfaces (Particles.hlsli, fxSurfaceQuery): a spatial hash of cell g_gridCell
// with g_gridMask + 1 buckets. Surfaces are the anchor-space records FxSurfaces wrote.
//   STEP=0 clear: thread per bucket (count, fill = 0); thread 0 clears the large-list and entry counters.
//   STEP=1 count: thread per surface: inflated AABB -> cells; > FX_GRID_SURFACE_CELLS cells -> large list, else
//          count[bucket of each cell] += 1 (and the entry total).
//   STEP=2 scan: group per 1024 buckets, exclusive prefix of count inside the block -> start, block total -> blocks.
//   STEP=4 block offsets: one group, exclusive prefix of the block totals (a bucket's start = start + blocks[b >> 10]).
//   STEP=3 fill: thread per surface of the grid: entries[start + atomic fill] = surface (order inside a bucket is free:
//          the candidates' order never changes a result).
// The AABB is inflated by max(1 mm, 1e-5 |coordinate|), so a point the float hit test accepts is inside it, and by the
// surface's motion over the tick (Particles.hlsli, moving surfaces); a surface turning by >= 1/2 rad in a tick is large.
// Every loop is bounded by the counts; entries beyond the capacity (surfaces x 64) cannot occur.
#include "Passes/FX/Particles.hlsli"

// Box of surface i (with its motion bound); turn = |w| dt, carry = its carrier displacement bound D (Particles.hlsli).
// Returns false when the surface turns by >= 1/2 rad in the tick or its motion is not finite (large list).
bool surfaceBox(uint i, out float3 lo, out float3 hi, out float turn, out float carry)
{
    FX_RWBUFFER(StreamSurface, surfaces, g_tickSurfaces);
    const StreamSurface s = surfaces[i];
    // a, b, c are offsets from the anchor-space reference point s.origin (NativeVfxStream.h)
    float r;
    if (s.kind == 0u) { lo = s.a - s.radius; hi = s.a + s.radius; r = length(s.a) + s.radius; }
    else if (s.kind == 1u) { lo = min(s.a, s.b) - s.radius; hi = max(s.a, s.b) + s.radius; r = max(length(s.a), length(s.b)) + s.radius; }
    else { lo = min(s.a, min(s.b, s.c)); hi = max(s.a, max(s.b, s.c)); r = max(length(s.a), max(length(s.b), length(s.c))); }
    lo += s.origin;
    hi += s.origin;
    turn = 0.0f;
    carry = 0.0f;
    float grow = 0.0f;
    const bool moves = any(s.velocity != 0.0f) || any(s.angular != 0.0f);
    bool small = true;
    if (moves)
    {
        turn = length(s.angular) * g_dt;
        const float u = length(s.velocity) * g_dt;
        carry = u + turn * (r + g_separationMax);
        small = isfinite(turn) && isfinite(carry) && turn < 0.5f;
        grow = small ? (turn * r + (1.0f + turn) * u) / (1.0f - turn) : 0.0f;
        if (!isfinite(carry)) carry = asfloat(0x7F800000u);  // +inf: every query becomes exhaustive
    }
    const float3 pad = max(1e-3f, 1e-5f * max(abs(lo), abs(hi))) + grow;
    lo -= pad;
    hi += pad;
    return small;
}

#if STEP == 2 || STEP == 4
groupshared uint gs_partial[1024];
#endif

[numthreads(STEP == 2 || STEP == 4 ? 1024 : 64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 gtid : SV_GroupThreadID)
{
    FX_RWBUFFER(uint, counts, g_gridCount);
    FX_RWBUFFER(uint, fills, g_gridFill);
    FX_RWBUFFER(uint, counters, g_counters);
#if STEP == 0
    if (id.x == 0u)
    {
        counters[FX_COUNTER_LARGE] = 0u;
        counters[FX_COUNTER_GRID_ENTRIES] = 0u;
        counters[FX_COUNTER_TURN] = 0u;
        counters[FX_COUNTER_CARRY] = 0u;
    }
    if (id.x <= g_gridMask) { counts[id.x] = 0u; fills[id.x] = 0u; }
#elif STEP == 1 || STEP == 3
    if (id.x >= g_surfaceCount) return;
    float3 lo, hi;
    float turn, carry;
    const bool bounded = surfaceBox(id.x, lo, hi, turn, carry);
    int3 a, span;
    uint cells = fxGridBox(lo, hi, FX_GRID_SURFACE_CELLS, a, span);  // 0: large or not finite -> large list
    if (!bounded) cells = 0u;
    const bool small = cells != 0u;
#if STEP == 1
    {
        // the candidate filter's box (infinite for a surface without a motion bound)
        FX_RWBUFFER(float4, boxes, g_surfaceBoxes);
        const float inf = asfloat(0x7F800000u);
        boxes[2u * id.x] = float4(bounded ? lo : -inf.xxx, 0);
        boxes[2u * id.x + 1u] = float4(bounded ? hi : inf.xxx, 0);
    }
    // motion maxima of the tick (non-negative floats order as their bits; max is order independent)
    if (carry > 0.0f) InterlockedMax(counters[FX_COUNTER_CARRY], asuint(carry));
    if (small && turn > 0.0f) InterlockedMax(counters[FX_COUNTER_TURN], asuint(turn));
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
    FX_RWBUFFER(uint, entries, g_gridEntries);
    for (uint k = 0u; k < cells; ++k)
    {
        const uint bucket = fxGridHash(fxGridCellOf(a, span, k)) & g_gridMask;
        uint j;
        InterlockedAdd(fills[bucket], 1u, j);
        const uint at = fxGridStart(bucket) + j;
        if (at < g_gridEntryCapacity) entries[at] = id.x;
        else fxStatus(FX_STATUS_RANGE);
    }
#endif
#elif STEP == 2
    FX_RWBUFFER(uint, starts, g_gridStart);
    FX_RWBUFFER(uint, blocks, g_gridBlocks);
    const uint t = gtid.x, b = id.x, buckets = g_gridMask + 1u;
    const uint v = b < buckets ? counts[b] : 0u;
    gs_partial[t] = v;
    GroupMemoryBarrierWithGroupSync();
    for (uint s = 1u; s < 1024u; s <<= 1)
    {
        const uint o = t >= s ? gs_partial[t - s] : 0u;
        GroupMemoryBarrierWithGroupSync();
        gs_partial[t] += o;
        GroupMemoryBarrierWithGroupSync();
    }
    if (b < buckets) starts[b] = gs_partial[t] - v;
    if (t == 1023u) blocks[b >> 10] = gs_partial[t];
#else  // STEP == 4
    FX_RWBUFFER(uint, blocks, g_gridBlocks);
    const uint t = gtid.x, count = ((g_gridMask + 1u) + 1023u) / 1024u;
    const uint v = t < count ? blocks[t] : 0u;
    gs_partial[t] = v;
    GroupMemoryBarrierWithGroupSync();
    for (uint s = 1u; s < 1024u; s <<= 1)
    {
        const uint o = t >= s ? gs_partial[t - s] : 0u;
        GroupMemoryBarrierWithGroupSync();
        gs_partial[t] += o;
        GroupMemoryBarrierWithGroupSync();
    }
    if (t < count) blocks[t] = gs_partial[t] - v;
#endif
}
