// unx-kernel: cs_6_6 main
// r.sc.pairs.trace with surface_cache.direct_pairs_inline (SurfaceCacheLightPairs.hlsli): the same pass as
// SurfaceCacheLightPairsTrace.hlsl as a compute kernel with an inline ray query - no ray pipeline, no hit groups, no
// continuation of a shader across the trace. One thread per pair record, at most one query per TLAS; the dispatch is a
// band of at most 262,144 threads (rows of 64 x 4096 groups). Non-opaque candidates take the alpha test here, at most
// SCP_INLINE_CANDIDATES of them a TLAS; more: the ray counts as blocked (the cap only ends a ray that would not end -
// SurfaceCacheLight.hlsl scVisibleInline's rule). Bit 11 of the flags: no alpha test (every caster opaque).
// P[0] = { cache UAV, budget, frame, flags }, P[4] = { cells SRV (raw), pairs UAV (raw), the dispatch's first pair, 0 }
// P[6], P[7] = RtSceneSrvs
#include "RayTracing/RayScene.hlsli"
#include "Passes/SurfaceCache/SurfaceCacheLightPairs.hlsli"

#define SCP_INLINE_CANDIDATES 64u

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint pair = id.x + P[4].z;
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
    if (!scpRayOk(ray.Origin, ray.Direction) || !scpIntervalOk(ray.TMin, ray.TMax)) return;
    const RtSceneSrvs scene = rtSceneSrvs(P[6], P[7]);
    const uint mask = sun ? RT_MASK_HIT_SHADOW : RT_MASK_SHADOW;
    const uint rayFlags = (P[0].w & 2048u) != 0 ? RAY_FLAG_FORCE_OPAQUE : RAY_FLAG_NONE;
    bool visible = true;
    [loop] for (uint tlas = 0; tlas < 2 && visible; ++tlas)
    {
        RaytracingAccelerationStructure structure = ResourceDescriptorHeap[tlas == 0 ? scene.tlasStatic : scene.tlasDynamic];
        RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
        q.TraceRayInline(structure, rayFlags, mask, ray);
        uint candidates = 0;
        [loop] while (q.Proceed())
        {
            if (q.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE) continue;  // (the emitters' boxes are never committed: mask)
            if (++candidates > SCP_INLINE_CANDIDATES)
            {
                visible = false;
                q.Abort();
                break;
            }
            // (the any-hit shader's rule: see-through geometry does not stop a shadow ray - RayScene.hlsli)
            if (rtCandidateStops(scene, mask, q.CandidateInstanceID(), q.CandidateGeometryIndex(), q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics()))
                q.CommitNonOpaqueTriangleHit();
        }
        if (q.CommittedStatus() != COMMITTED_NOTHING) visible = false;
    }
    if (visible) pairs.Store(at + 28, flags | SCP_VISIBLE);
}
