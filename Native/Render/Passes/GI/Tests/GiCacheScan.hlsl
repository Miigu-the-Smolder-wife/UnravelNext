// unx-kernel: cs_6_6 main
// GI cache diagnostic (tests): every live entry with at least one update against a uniform radiance field L (a white
// furnace). Per entry: SH irradiance at the anchor normal / (pi L) and the texels' mean and minimum / L. Counts the
// entries off by more than the tolerance and records the first 64 of them.
// P[0] = { cache SRV (raw), result UAV (raw), L (float), tolerance (float) }
// P[1] = { free-room bounds min xyz, position tolerance }, P[2] = { bounds max xyz, unused }.
// Result word 5 counts anchors outside these bounds, independently of convergence.
// Result: { live updated, E off, texel mean off, texel minimum off } then 64 records of 64 B:
//   { anchor xyz, level | normal class << 8, E / (pi L), texel mean / L, texel min / L, updates, history, last used frame,
//     last update frame, entry, anchor normal xyz, 0 }
#include "Passes/GI/GiInternal.hlsli"

[numthreads(64, 1, 1)]
void main(uint entry : SV_DispatchThreadID)
{
    ByteAddressBuffer b = ResourceDescriptorHeap[P[0].x];
    RWByteAddressBuffer result = ResourceDescriptorHeap[P[0].y];
    const GiHeader h = giHeader(b);
    if (entry >= h.capacity) return;
    const uint4 meta = b.Load4(h.offMeta + entry * 16);
    if (meta.y == 0) return;
    const uint sh = h.offSh + entry * GI_SH_STRIDE;
    const uint updates = b.Load(sh + GI_SH_UPDATES);
    if (updates == 0) return;
    const float L = asfloat(P[0].z), tolerance = asfloat(P[0].w);
    const float3 position = giAnchorPosition(b, h, entry);
    // Deterministic anchors encode each component across 2*cellSize in
    // 14 bits. Include its half-step rounding error in the spatial oracle.
    const float positionTolerance = asfloat(P[1].w) + giCellSize(h, meta.x & 31u) / 16383.0;
    const bool outside = any(position < asfloat(P[1].xyz) - positionTolerance) || any(position > asfloat(P[2].xyz) + positionTolerance);
    if (outside)
    {
        uint ignored;
        result.InterlockedAdd(20, 1u, ignored);
    }
    const float3 n = giAnchorNormal(b, h, entry);
    float sv;
    const float e = dot(giShIrradiance(b, h, entry, n, sv), 1.0 / 3.0) / (3.14159265 * L);
    float mean = 0, least = 1e30;
    [loop] for (uint t = 0; t < GI_TEXEL_COUNT; ++t)
    {
        const uint2 v = b.Load2(h.offTexels + (entry * GI_TEXEL_COUNT + t) * 8);
        const float x = dot(float3(f16tof32(v.x), f16tof32(v.x >> 16), f16tof32(v.y)), 1.0 / 3.0) * GI_LOAD_SCALE / L;
        mean += x / GI_TEXEL_COUNT;
        least = min(least, x);
    }
    uint previous;
    result.InterlockedAdd(0, 1u, previous);
    const bool badE = abs(e - 1) > tolerance, badMean = abs(mean - 1) > tolerance, badMin = abs(least - 1) > tolerance;
    if (badE) result.InterlockedAdd(4, 1u, previous);
    if (badMean) result.InterlockedAdd(8, 1u, previous);
    if (badMin) result.InterlockedAdd(12, 1u, previous);
    if (!(badE || badMean || badMin)) return;
    uint slot;
    result.InterlockedAdd(16, 1u, slot);
    if (slot >= 64) return;
    const uint level = meta.x & 31u, normalClass = (meta.x >> 5) & 7u;
    const uint a = 32 + slot * 64;
    result.Store4(a, uint4(asuint(giAnchorPosition(b, h, entry)), level | (normalClass << 8)));
    result.Store4(a + 16, uint4(asuint(e), asuint(mean), asuint(least), updates));
    result.Store4(a + 32, uint4(b.Load(sh + GI_SH_HISTORY), meta.z, b.Load(sh + GI_SH_LAST_UPDATE), entry));
    result.Store4(a + 48, uint4(asuint(n), 0));
}
