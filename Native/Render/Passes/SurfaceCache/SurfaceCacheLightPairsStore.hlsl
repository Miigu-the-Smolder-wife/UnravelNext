// unx-kernel: cs_6_6 main
// r.sc.pairs.store (SurfaceCacheLightPairs.hlsli): one thread per cell of the frame's budget. The cell's direct light =
// the sum of its visible light pairs' irradiance, its sun light = the sun pair's when visible; its indirect light from
// the 3 x 3 probes around it on its face (SurfaceCacheLight.hlsl SurfaceCacheCellsGen's gather, the same weights). The
// values replace the cell's and the cell counts as lit, exactly as SurfaceCacheCellsGen stores them.
// P[0] = { cache UAV, budget, frame, flags (bit 0: local lights and sun, bit 1: radiosity) }
// P[4] = { cells SRV (raw), pairs SRV (raw), 0, 0 }
#include "Passes/SurfaceCache/SurfaceCacheLightPairs.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint cell = id.x;
    if (cell >= P[0].y) return;
    ByteAddressBuffer cells = ResourceDescriptorHeap[P[4].x];
    const uint slot = cells.Load(cell * 4);
    if (slot == SC_NONE) return;
    RWByteAddressBuffer b = ResourceDescriptorHeap[P[0].x];
    const ScLayout l = scLayout(b);
    const uint n = l.entries;
    if (n == 0) return;
    const uint4 data = b.Load4(scDataOffset(n, slot));
    const float3 position = asfloat(data.xyz), normal = scUnpackOct(data.w);
    const uint3 before = b.Load3(scLightOffset(n, slot));
    float3 direct = scUnpackRgb(before.x), sun = scUnpackRgb(before.y), indirect = scUnpackRgb(before.z);
    if (P[0].w & 1u)
    {
        ByteAddressBuffer pairs = ResourceDescriptorHeap[P[4].y];
        direct = 0;
        sun = 0;
        [loop] for (uint k = 0; k < SCP_PAIRS; ++k)
        {
            const uint4 tail = pairs.Load4(scpPairOffset(cell, k) + 16);
            if ((tail.w & SCP_VISIBLE) == 0) continue;
            if (k < SCP_LIGHTS) direct += asfloat(tail.xyz);
            else sun = asfloat(tail.xyz);
        }
    }
    if (P[0].w & 2u)
    {
        // the 3 x 3 probes around the cell on its face
        const uint level = scLevel(l, position);
        const uint face = scFace(normal);
        const int3 centre = scCoord(level, position, SC_PROBE_SPACING);
        const int3 du = face < 2 ? int3(0, 1, 0) : int3(1, 0, 0), dv = face < 4 ? int3(0, 0, 1) : int3(0, 1, 0);
        const float reach = 1.5 * scLevelSize(level) * SC_PROBE_SPACING;
        const uint probes = scProbeCount(n);
        float3 sum = 0;
        float weights = 0;
        [loop] for (int k = 0; k < 9; ++k)
        {
            const uint probe = scFind(b, scProbeKeysOffset(n, 0), probes, scKeyAt(level, centre + du * (k % 3 - 1) + dv * (k / 3 - 1), face));
            if (probe == SC_NONE) continue;
            if (((b.Load(scProbeHeadsOffset(n, probe)) >> 8) & 0xFFu) == 0) continue;  // not lit yet
            const uint4 pd = b.Load4(scProbeDataOffset(n, probe));
            const float3 offset = asfloat(pd.xyz) - position;
            const float off = abs(dot(offset, normal));
            const float w = saturate(1 - length(offset - normal * dot(offset, normal)) / reach) * exp2(-8.0 * off / reach) * saturate(dot(normal, scUnpackOct(pd.w)));
            if (!(w > 0)) continue;
            sum += w * scUnpackRgb(b.Load(scProbeLightOffset(n, probe)));
            weights += w;
        }
        if (weights > 0) indirect = sum / weights;
    }
    if (any(isnan(direct)) || any(isinf(direct)) || any(isnan(sun)) || any(isinf(sun)) || any(isnan(indirect)) || any(isinf(indirect))) return;
    b.Store3(scLightOffset(n, slot), uint3(scPackRgb(direct), scPackRgb(sun), scPackRgb(indirect)));
    const uint headOffset = scHeadsOffset(n, slot);
    if (((b.Load(headOffset) >> 8) & 0xFFu) == 0) b.InterlockedAdd(headOffset, 1u << 8);  // lit (atomic: marks set bit 0 of this word)
}
