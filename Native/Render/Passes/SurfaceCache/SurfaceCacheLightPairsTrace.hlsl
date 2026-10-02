// unx-kernel: lib_6_6 main
// r.sc.pairs.trace (SurfaceCacheLightPairs.hlsli): one ray generation thread per pair record, at most ONE shadow ray, and
// nothing alive across it but the pair's address - the structure of MegaLights' m.ml.trace. The dispatch is a band of at
// most 262,144 threads (ReflectionSystem / SurfaceCacheLightPairs.cpp). A pair that needs no ray returns at once.
// A light's ray: from the cell's point moved by the bias to the light's side of its surface, TMin = the bias, shadow
// casters only (RT_MASK_SHADOW), as mlSampleVisible. The sun's: from the normal's side, TMin 0, the casters too
// (RT_MASK_HIT_SHADOW, as SurfaceCacheCellsGen's sun ray).
// P[0] = { cache UAV, budget, frame, flags (bit 11: alpha-tested casters taken as opaque) }
// P[4] = { cells SRV (raw), pairs UAV (raw), the dispatch's first pair, 0 }
// P[6], P[7] = RtSceneSrvs
#include "RayTracing/RayShaders.hlsli"
#include "Passes/SurfaceCache/SurfaceCacheLightPairs.hlsli"

[shader("raygeneration")]
void SurfaceCachePairsTraceGen()
{
    const uint pair = DispatchRaysIndex().x + P[4].z;
    const uint cell = pair / SCP_PAIRS;
    if (cell >= P[0].y) return;
    RWByteAddressBuffer pairs = ResourceDescriptorHeap[P[4].y];
    const uint at = pair * SCP_PAIR_BYTES;
    const uint flags = pairs.Load(at + 28);
    if ((flags & SCP_NEEDS_RAY) == 0) return;
    ByteAddressBuffer cells = ResourceDescriptorHeap[P[4].x];
    const uint slot = cells.Load(cell * 4);
    if (slot == SC_NONE) return;
    RWByteAddressBuffer b = ResourceDescriptorHeap[P[0].x];
    const ScLayout l = scLayout(b);
    const uint4 data = b.Load4(scDataOffset(l.entries, slot));
    const float3 position = asfloat(data.xyz), normal = scUnpackOct(data.w);
    const uint4 head = pairs.Load4(at);
    const float bias = scpBias(l, position);
    const bool sun = (flags & SCP_SUN) != 0;
    RayDesc ray;
    ray.Direction = asfloat(head.xyz);
    ray.Origin = position + normal * (!sun && dot(normal, ray.Direction) < 0 ? -bias : bias);
    ray.TMin = sun ? 0.0 : bias;
    ray.TMax = asfloat(head.w);
    // (the select pass checked the ray; the cell's point may have been rewritten by a mark since - checked again)
    if (!scpRayOk(ray.Origin, ray.Direction) || !scpIntervalOk(ray.TMin, ray.TMax)) return;
    const uint rayFlags = (P[0].w & 2048u) != 0 ? RAY_FLAG_FORCE_OPAQUE : RAY_FLAG_NONE;
    if (rtVisible(rtScene(), ray, sun ? RT_MASK_HIT_SHADOW : RT_MASK_SHADOW, rayFlags)) pairs.Store(at + 28, flags | SCP_VISIBLE);
}
