// Projected decals at ray hits (FEATURES_GAME 5.2, A7; E's Passes/Decal/Decal.hlsli decalApplyHit): R's reflection and GI
// hits see the decals M's resolve applies in the direct view.
// Query: RayScene's decal TLAS (one BLAS of procedural AABBs, box i = decal i's frame record: camera-relative centre +
// camera +- |axisX| + |axisY| + |axisZ| per component, rebuilt each frame the main view has decals; RayScene::recordDecals)
// is traced inline with a point ray (length 1e-4 m) at the hit: every box containing the point is a candidate (the
// hardware's AABB test is conservative, decalLayer's exact box test decides). At most DECAL_PER_TILE candidates, as in a
// screen tile (more: the rest are not applied, the same bound as the direct view's tiles).
// Words 16..19 of the local-light grid header (RtSceneSrvs.pad, written per frame): { decal TLAS SRV, decal frames SRV,
// M's material texture table (0xFFFFFFFF: constants only), decal count }; 0xFFFFFFFF in word 16: no decals this frame.
// Condition: hit shading uses the interpolated normal (it applies no normal map, the base material's included), so a
// decal changes base colour, roughness and metallic there; its normal (LEAN moments) is the direct view's alone.
#ifndef UNX_RT_HIT_DECALS_HLSLI
#define UNX_RT_HIT_DECALS_HLSLI
#include "RayTracing/RayScene.hlsli"
#include "Passes/Decal/Decal.hlsli"

// The decals over the hit's material. footprint = the ray cone's width at the hit (texture level of detail).
void rtHitDecals(RtSceneSrvs scene, RtSurface s, float footprint, inout GpuMaterial m)
{
    if (scene.pad == 0xFFFFFFFFu) return;
    ByteAddressBuffer header = ResourceDescriptorHeap[scene.pad];
    const uint4 w = header.Load4(64);
    if (w.x == 0xFFFFFFFFu || w.w == 0) return;
    RaytracingAccelerationStructure boxes = ResourceDescriptorHeap[w.x];
    RayQuery<RAY_FLAG_SKIP_TRIANGLES> q;
    RayDesc r;
    r.Origin = s.position;
    r.Direction = float3(0, 1, 0);
    r.TMin = 0;
    r.TMax = 1e-4;
    q.TraceRayInline(boxes, RAY_FLAG_NONE, 0xFF, r);
    uint ids[DECAL_PER_TILE];
    uint count = 0;
    while (q.Proceed())
    {
        if (q.CandidateType() != CANDIDATE_PROCEDURAL_PRIMITIVE) continue;
        ids[count++] = q.CandidatePrimitiveIndex();
        if (count == DECAL_PER_TILE) q.Abort();
    }
    if (count == 0) return;
    // The footprint as two surface tangents (the screen derivatives decalLayer filters its textures with).
    const float3 n = s.geometricNormal;
    const float3 t = normalize(abs(n.y) < 0.9 ? cross(n, float3(0, 1, 0)) : cross(n, float3(1, 0, 0)));
    DecalSurface ds;
    ds.position = s.position - g_cameraPosition;
    ds.dpdx = t * footprint;
    ds.dpdy = cross(n, t) * footprint;
    ds.geometricNormal = n;
    ds.instance = s.sceneInstance;
    ds.geometricVariance = 0;
    DecalMaterial dm;
    dm.baseColor = m.baseColor;
    dm.roughness = m.roughness;
    dm.metallic = m.metallic;
    dm.normal = s.normal;
    dm.variance = 0;
    DecalContext c;
    c.frames = w.y;
    c.tiles = DECAL_NONE;
    c.materialTable = w.z;
    decalApplyHit(c, ids, count, ds, dm);
    m.baseColor = dm.baseColor;
    m.roughness = dm.roughness;
    m.metallic = dm.metallic;
}
#endif
