// unx-kernel: cs_6_6 main
// r.lights.fxgroups (A3, S_STATUS_KO.md 10): the FX particle lights' groups for the ray hits' local-light choice
// (HitLocalLights.hlsli rtFxWeight / rtFxChoose). The lights are gpu::Light point records at the scene light buffer's tail
// [N, N + F) (INTERFACES v1.79), F = min(count word, capacity); consecutive rows come from one emitter, so groups of
// FX_GROUP consecutive lights are spatially compact. Per group: the bounding sphere of its lights' range spheres (outside
// it no light of the group reaches) and the sum of phi_j = intensity x luminance(colour) (the grid importance's
// numerator). One thread per group, one group of 1024 threads: G = ceil(F / 32) <= 1024 at the 32,768-light limit (the
// structural bound: 32 records per thread).
// Output (raw): { F, G, 0, 0 } then per group 32 B { centre xyz, radius, sum phi, first light (0-based in the tail), count, 0 }.
// P[0] = { output UAV, 0, 0, N }, P[1].x capacity; b1 = frame constants (the lights through g_lights, F through
// g_fxLightCount: the scene's own SRVs; the graph orders this pass after the FX writer by its declared uses).
#include "Bindless.hlsli"
#include "Frame.hlsli"
#include "Scene.hlsli"

#define FX_GROUP 32u

[numthreads(1024, 1, 1)]
void main(uint t : SV_GroupIndex)
{
    RWByteAddressBuffer dst = ResourceDescriptorHeap[P[0].x];
    uint F = 0;
    if (g_fxLightCount != 0xFFFFFFFFu)
    {
        StructuredBuffer<uint> countWord = ResourceDescriptorHeap[g_fxLightCount];
        F = min(countWord[0], P[1].x);
    }
    const uint G = (F + FX_GROUP - 1) / FX_GROUP;
    if (t == 0) dst.Store4(0, uint4(F, G, 0, 0));
    if (t >= G) return;
    const uint first = t * FX_GROUP, count = min(FX_GROUP, F - first);
    float3 lo = 3.0e38, hi = -3.0e38;
    float phiSum = 0;
    for (uint k = 0; k < count; ++k)
    {
        const GpuLight l = loadLight(P[0].w + first + k);
        const float lum = max(0.2126 * l.color.x + 0.7152 * l.color.y + 0.0722 * l.color.z, 1e-6);
        phiSum += max(l.intensity, 0.0) * lum;
        const float r = max(l.range, 1e-3);
        lo = min(lo, l.position - r);
        hi = max(hi, l.position + r);
    }
    const float3 centre = 0.5 * (lo + hi);
    dst.Store4(16 + 32 * t, uint4(asuint(centre), asuint(0.5 * length(hi - lo))));
    dst.Store4(32 + 32 * t, uint4(asuint(phiSum), first, count, 0));
}
