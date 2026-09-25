// unx-kernel: cs_6_6 main
// Medium cells of the tick's live volume particles: thread per cell of the volume ranges (active NV_VOLUME rows:
// {first cell, cells, grid, program}, sorted by first cell, built by the CPU from its emitter table mirror). The n^3
// consecutive threads of a particle read its record (Particles.hlsli, VolumeRecord, at its first cell) and write
// consecutive 96 B cells, so the stores are coalesced. A cell whose record is not of this tick (no live particle at that
// birth) is left as it is, as before. The range search is a bounded binary search.
#include "Passes/FX/Particles.hlsli"

struct VolumeRange { uint first, cells, grid, program; };

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint c = id.x;
    if (c >= g_cellCapacity || g_volumeRangeCount == 0u) return;
    FX_BUFFER(VolumeRange, ranges, g_volumeRanges);
    // last range with first <= c
    uint lo = 0u, hi = g_volumeRangeCount;
    [loop] for (uint guard = 0u; guard < 32u && hi - lo > 1u; ++guard)
    {
        const uint mid = (lo + hi) >> 1;
        if (ranges[mid].first <= c) lo = mid; else hi = mid;
    }
    const VolumeRange r = ranges[lo];
    if (c < r.first || c - r.first >= r.cells) return;
    const uint n = r.grid, cells = n * n * n, local = c - r.first, k = local % cells, first = c - k;
    FX_RWBUFFER(VolumeRecord, records, g_volumeList);
    const VolumeRecord v = records[first];
    if (v.serial != g_serial) return;
    FX_BUFFER(StreamProgram, programs, g_programs);
    const StreamProgram p = programs[r.program];
    const uint3 q = uint3(k % n, (k / n) % n, k / (n * n));
    const float3 lo3 = v.centre + v.size * ((float3)q / (float)n - 0.5f), hi3 = v.centre + v.size * ((float3)(q + 1u) / (float)n - 0.5f);
    const float volume = (hi3.x - lo3.x) * (hi3.y - lo3.y) * (hi3.z - lo3.z);
    const float density = v.colour.a * fxTentMass(q.x, n) * fxTentMass(q.y, n) * fxTentMass(q.z, n) / max(volume, 1e-30f);
    MediumCell mc;
    mc.cell = int4(v.cell, 0);
    mc.low = float4(lo3, 0);
    mc.high = float4(hi3, 0);
    mc.absorption = float4(p.mediumAbsorption * density, p.mediumPhase);
    mc.scattering = float4(p.mediumScattering * density, 0);
    mc.emission = float4(p.mediumEmission * density * v.colour.rgb, 0);
    FX_RWBUFFER(MediumCell, cellsOut, g_mediumCells);
    cellsOut[c] = mc;
}
