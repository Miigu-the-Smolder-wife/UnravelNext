// The surface cache's direct light as (cell, light) pairs (surface_cache.direct_pairs; owner A for S2's surface cache,
// 2026-10-02). SurfaceCacheLight.hlsl's SurfaceCacheCellsGen lights a cell inside one ray generation thread: it picks the
// lights, evaluates them and traces up to ten shadow rays in loops, with all of that state alive across every trace (the
// bath lounge's device-hung report was located in those rays). This path does the same lighting in the structure
// MegaLights uses (which ran in the lounge): no thread traces more than one ray, and nothing but a ray is alive across it.
//   select  SurfaceCacheLightPairsSelect.hlsl, compute, one thread per cell of the frame's budget: the cell (the same
//           order as SurfaceCacheCellsGen), its 8 strongest lights by unshadowed importance, each one's unshadowed
//           irradiance by its integral over the light (mlLightUnshadowed: surface_cache.direct_analytic's value) and the
//           ray toward its centre; the sun's irradiance and its ray. No ray is traced. 9 pair records per cell.
//   trace   SurfaceCacheLightPairsTrace.hlsl (ray generation, rtVisible) or SurfaceCacheLightPairsTraceInline.hlsl
//           (compute, an inline query): one thread per pair record, at most one shadow ray, in dispatches of at most
//           262,144 threads; a pair that needs no ray returns at once. An unblocked pair gets its visible bit.
//   store   SurfaceCacheLightPairsStore.hlsl, compute, one thread per cell: the sum of the visible pairs' irradiance, the
//           sun's, the cell's indirect light from the 3 x 3 probes (as SurfaceCacheCellsGen), stored; the cell is lit.
// Not in this path (SurfaceCacheCellsGen's other modes): surface_cache.direct_stochastic, direct_analytic = false (a
// random point on each light), remainder_light.
// Cells buffer (raw): 4 B per budget index = the cell's slot (SC_NONE: no cell, or one without a valid point).
// Pairs buffer (raw): SCP_PAIR_BYTES per (budget index x SCP_PAIRS + k): { direction xyz, TMax } { irradiance rgb, flags }.
#ifndef UNX_SURFACE_CACHE_LIGHT_PAIRS_HLSLI
#define UNX_SURFACE_CACHE_LIGHT_PAIRS_HLSLI
#include "Passes/SurfaceCache/SurfaceCache.hlsli"

#define SCP_LIGHTS 8u        // SurfaceCacheLight.hlsl SC_LIGHTS_PER_CELL
#define SCP_PAIRS 9u         // the lights, then the sun
#define SCP_PAIR_BYTES 32u
#define SCP_NEEDS_RAY 1u     // a shadow ray decides the pair
#define SCP_VISIBLE 2u       // the pair's irradiance reaches the cell
#define SCP_SUN 4u           // the sun's pair: the GI mask, the ray from the normal's side, TMin 0

// Whether a ray may be launched (SurfaceCacheLight.hlsl scRayOk): finite origin, unit direction, 0 <= TMin <= TMax finite.
bool scpRayOk(float3 origin, float3 direction)
{
    const float d = dot(direction, direction);
    return all(abs(origin) < 1e9) && d > 0.98 && d < 1.02;  // (comparisons are false for NaN)
}
bool scpIntervalOk(float tMin, float tMax) { return tMin >= 0 && tMax >= tMin && tMax < 1e30; }

// The offset a cell's rays start with (SurfaceCacheCellsGen's bias).
float scpBias(ScLayout l, float3 position) { return 1e-3 + 2e-4 * distance(position, l.camera); }

// The item a thread works on - SurfaceCacheLight.hlsl scPick / scPickCell, the same order (they must stay equal: both
// paths relight the same cells of a frame).
uint scpPick(RWByteAddressBuffer b, uint index, uint budget, uint frame, uint lit, uint fresh, uint count, uint listBase)
{
    if (index < fresh) return b.Load(listBase + (count - 1 - index) * 4);
    const uint j = index - fresh;
    if (j >= lit) return SC_NONE;
    const uint room = budget - min(fresh, budget);
    const uint at = lit > room ? (j + (frame % lit) * (room % lit)) % lit : j;
    return b.Load(listBase + at * 4);
}
uint scpPickCell(RWByteAddressBuffer b, uint index, uint budget, uint frame, uint n, bool feedback)
{
    const uint lit = b.Load(8), fresh = b.Load(12);
    if (!feedback || index < fresh) return scpPick(b, index, budget, frame, lit, fresh, n, scListOffset(n, 0));
    const uint wanted = b.Load(52);
    const uint room = budget - min(fresh, budget);
    const uint share = min(room / 2, wanted);
    const uint j = index - fresh;
    if (j < share)
    {
        const uint at = wanted > share ? (j + (frame % wanted) * (share % wanted)) % wanted : j;
        return b.Load(scFeedbackListOffset(n, at));
    }
    return scpPick(b, index - share, budget - share, frame, lit, fresh, n, scListOffset(n, 0));
}

struct ScpPair
{
    float3 direction;
    float tMax;
    float3 irradiance;
    uint flags;
};
uint scpPairOffset(uint cell, uint k) { return (cell * SCP_PAIRS + k) * SCP_PAIR_BYTES; }
#endif
