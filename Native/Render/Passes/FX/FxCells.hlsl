// unx-kernel: cs_6_6 main
// Medium cells of the tick's live volume particles: thread per cell of the volume ranges (active NV_VOLUME rows:
// {first cell, cells, grid, program}, sorted by first cell, built by the CPU from its emitter table mirror). The n^3
// consecutive threads of a particle read its record (Particles.hlsli, VolumeRecord, at its first cell) and write
// consecutive 96 B cells, so the stores are coalesced. A cell whose record is not of this tick (no live particle at that
// birth) is left as it is, as before. The range search is a bounded binary search.
#include "Passes/FX/Particles.hlsli"

struct VolumeRange { uint first, cells, grid, program; };

groupshared float4 gs_cells[64 * 6];

// The group's 64 cells are formed in group memory and written as 384 consecutive float4 (the buffer is viewed as float4):
// each store instruction covers contiguous 16 B per lane instead of one 16 B piece of a 96 B row per lane.
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 gtid : SV_GroupThreadID, uint3 gid : SV_GroupID)
{
    const uint c = id.x, t = gtid.x;
    bool written = false;
    if (c < g_cellCapacity && g_volumeRangeCount != 0u)
    {
        FX_BUFFER(VolumeRange, ranges, g_volumeRanges);
        // last range with first <= c
        uint lo = 0u, hi = g_volumeRangeCount;
        [loop] for (uint guard = 0u; guard < 32u && hi - lo > 1u; ++guard)
        {
            const uint mid = (lo + hi) >> 1;
            if (ranges[mid].first <= c) lo = mid; else hi = mid;
        }
        const VolumeRange r = ranges[lo];
        if (c >= r.first && c - r.first < r.cells)
        {
            const uint n = r.grid, cells = n * n * n, local = c - r.first, k = local % cells, first = c - k;
            FX_RWBUFFER(VolumeRecord, records, g_volumeList);
            const VolumeRecord v = records[first];
            if (v.serial == g_serial)
            {
                FX_BUFFER(StreamProgram, programs, g_programs);
                const StreamProgram p = programs[r.program];
                const uint3 q = uint3(k % n, (k / n) % n, k / (n * n));
                const float3 lo3 = v.centre + v.size * ((float3)q / (float)n - 0.5f), hi3 = v.centre + v.size * ((float3)(q + 1u) / (float)n - 0.5f);
                const float volume = (hi3.x - lo3.x) * (hi3.y - lo3.y) * (hi3.z - lo3.z);
                const float density = v.colour.a * fxTentMass(q.x, n) * fxTentMass(q.y, n) * fxTentMass(q.z, n) / max(volume, 1e-30f);
                gs_cells[t * 6u + 0u] = asfloat(int4(v.cell, 0));
                gs_cells[t * 6u + 1u] = float4(lo3, 0);
                gs_cells[t * 6u + 2u] = float4(hi3, 0);
                gs_cells[t * 6u + 3u] = float4(p.mediumAbsorption * density, p.mediumPhase);
                gs_cells[t * 6u + 4u] = float4(p.mediumScattering * density, 0);
                gs_cells[t * 6u + 5u] = float4(p.mediumEmission * density * v.colour.rgb, 0);
                written = true;
            }
        }
    }
    // a cell without a live record of this tick keeps its old contents: its six words are marked and skipped
    if (!written) gs_cells[t * 6u].w = asfloat(0xFFFFFFFFu);
    GroupMemoryBarrierWithGroupSync();
    FX_RWBUFFER(float4, out4, g_mediumCells);
    const uint base = gid.x * 64u;
    [unroll] for (uint j = 0u; j < 6u; ++j)
    {
        const uint w = j * 64u + t, cell = base + w / 6u;
        if (cell < g_cellCapacity && asuint(gs_cells[(w / 6u) * 6u].w) != 0xFFFFFFFFu) out4[base * 6u + w] = gs_cells[w];
    }
}
