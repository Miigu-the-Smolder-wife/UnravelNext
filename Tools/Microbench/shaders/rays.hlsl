// Ray floor, inline RayQuery in compute (SM 6.6). Compiled in variants:
//   RQ_FLAGS  = ray flags template argument
//   ALPHA     = 1 -> candidate loop with per-primitive UV fetch + alpha texture test
// Root: t0 = TLAS. P[0].y out UAV, P[0].z alpha texture SRV, P[0].w uv buffer SRV,
// P[1].x dispatch width, P[1].y ray mode, P[1].z tMax (float bits), P[1].w seed
// P[2] = scene min x, min z, size (floats)  P[3] = sun dir (floats)
// P[4] = camera pos (floats), P[5] = camera forward, P[6] = camera right, P[7] = camera up, tan(fov/2) in P[7].w
// modes: 0 incoherent (random origin in canopy box, uniform sphere direction)
//        1 same origins, short tMax (host passes tMax)
//        2 primary camera rays over the dispatch grid
//        3 sun shadow rays from random terrain points (cone jitter 0.25 deg)

#ifndef RQ_FLAGS
#define RQ_FLAGS RAY_FLAG_NONE
#endif
#ifndef ALPHA
#define ALPHA 0
#endif

RaytracingAccelerationStructure Tlas : register(t0);

void makeRay(uint2 tid, out RayDesc r)
{
    uint mode = P[1].y;
    uint s = pcg(tid.x + tid.y * P[1].x + P[1].w * 7919u);
    float2 sceneMin = float2(asfloat(P[2].x), asfloat(P[2].y));
    float size = asfloat(P[2].z);
    r.TMin = 0.0;
    r.TMax = asfloat(P[1].z);
    if (mode == 2)
    {
        float3 cam = float3(asfloat(P[4].x), asfloat(P[4].y), asfloat(P[4].z));
        float3 fwd = float3(asfloat(P[5].x), asfloat(P[5].y), asfloat(P[5].z));
        float3 right = float3(asfloat(P[6].x), asfloat(P[6].y), asfloat(P[6].z));
        float3 up = float3(asfloat(P[7].x), asfloat(P[7].y), asfloat(P[7].z));
        float tanHalf = asfloat(P[7].w);
        float W = (float)P[1].x, H = (float)P[2].w;
        float2 ndc = ((float2(tid) + 0.5) / float2(W, H)) * 2.0 - 1.0;
        ndc.y = -ndc.y;
        float aspect = W / H;
        r.Origin = cam;
        r.Direction = normalize(fwd + right * (ndc.x * tanHalf * aspect) + up * (ndc.y * tanHalf));
        return;
    }
    float x = sceneMin.x + u01(s) * size;
    float z = sceneMin.y + u01(s) * size;
    float h = terrainHeight(x, z);
    if (mode == 3)
    {
        r.Origin = float3(x, h + 0.05, z);
        float3 sun = float3(asfloat(P[3].x), asfloat(P[3].y), asfloat(P[3].z));
        // 0.25 degree cone jitter (solar disc)
        float3 t = normalize(cross(sun, abs(sun.y) < 0.9 ? float3(0, 1, 0) : float3(1, 0, 0)));
        float3 b = cross(sun, t);
        float ang = 0.00436 * sqrt(u01(s));
        float phi = 6.28318530718 * u01(s);
        r.Direction = normalize(sun + t * (ang * cos(phi)) + b * (ang * sin(phi)));
        return;
    }
    r.Origin = float3(x, h + 0.3 + u01(s) * 14.0, z);
    r.Direction = sphereDir(s);
}

[numthreads(8, 8, 1)]
void RayCS(uint2 tid : SV_DispatchThreadID)
{
    RWStructuredBuffer<float> outb = ResourceDescriptorHeap[P[0].y];
    RayDesc r; makeRay(tid, r);
    RayQuery<RQ_FLAGS> q;
    q.TraceRayInline(Tlas, RAY_FLAG_NONE, 0xff, r);
#if ALPHA
    Texture2D<float> alphaTex = ResourceDescriptorHeap[P[0].z];
    StructuredBuffer<float2> uvs = ResourceDescriptorHeap[P[0].w];
    SamplerState lin = SamplerDescriptorHeap[0];
    while (q.Proceed())
    {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
        {
            uint prim = q.CandidatePrimitiveIndex();
            float2 b = q.CandidateTriangleBarycentrics();
            float2 uv = uvs[prim * 3] * (1 - b.x - b.y) + uvs[prim * 3 + 1] * b.x + uvs[prim * 3 + 2] * b.y;
            if (alphaTex.SampleLevel(lin, uv, 0) > 0.5) q.CommitNonOpaqueTriangleHit();
        }
    }
#else
    q.Proceed();
#endif
    float t = (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ? q.CommittedRayT() : -1.0;
    outb[tid.y * P[1].x + tid.x] = t;
}

// ---------------------------------------------------------------- sub-pixel identity census
// pass 0 (P[1].w == 0): identity of the pixel-centre ray -> out UAV (P[0].y)
// pass 1 (P[1].w == 1): 16 stratified sub-samples per pixel; out = distinct identities (low 8 bits)
//                       | (sub-samples whose identity is absent from the 3x3 centre set) << 8. Centre ids in P[3].w SRV.
uint traceIdentity(float2 pix)
{
    float3 cam = float3(asfloat(P[4].x), asfloat(P[4].y), asfloat(P[4].z));
    float3 fwd = float3(asfloat(P[5].x), asfloat(P[5].y), asfloat(P[5].z));
    float3 right = float3(asfloat(P[6].x), asfloat(P[6].y), asfloat(P[6].z));
    float3 up = float3(asfloat(P[7].x), asfloat(P[7].y), asfloat(P[7].z));
    float tanHalf = asfloat(P[7].w);
    float W = (float)P[1].x, H = (float)P[2].w;
    float2 ndc = (pix / float2(W, H)) * 2.0 - 1.0; ndc.y = -ndc.y;
    RayDesc r; r.Origin = cam; r.TMin = 0; r.TMax = asfloat(P[1].z);
    r.Direction = normalize(fwd + right * (ndc.x * tanHalf * (W / H)) + up * (ndc.y * tanHalf));
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(Tlas, RAY_FLAG_NONE, 0xff, r);
    Texture2D<float> alphaTex = ResourceDescriptorHeap[P[0].z];
    StructuredBuffer<float2> uvs = ResourceDescriptorHeap[P[0].w];
    SamplerState lin = SamplerDescriptorHeap[0];
    while (q.Proceed())
    {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
        {
            uint prim = q.CandidatePrimitiveIndex();
            float2 b = q.CandidateTriangleBarycentrics();
            float2 uv = uvs[prim * 3] * (1 - b.x - b.y) + uvs[prim * 3 + 1] * b.x + uvs[prim * 3 + 2] * b.y;
            if (alphaTex.SampleLevel(lin, uv, 0) > 0.5) q.CommitNonOpaqueTriangleHit();
        }
    }
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return 0xffffffffu;
    return pcg(q.CommittedInstanceIndex() * 7919u + q.CommittedGeometryIndex() * 104729u + q.CommittedPrimitiveIndex());
}

[numthreads(8, 8, 1)]
void EdgeCountCS(uint2 tid : SV_DispatchThreadID)
{
    RWStructuredBuffer<uint> outb = ResourceDescriptorHeap[P[0].y];
    uint W = P[1].x, H = P[2].w;
    uint idx = tid.y * W + tid.x;
    if (P[1].w == 0) { outb[idx] = traceIdentity(float2(tid) + 0.5); return; }
    StructuredBuffer<uint> centres = ResourceDescriptorHeap[P[3].w];
    uint c9[9];
    [unroll] for (int j = 0; j < 9; ++j)
    {
        int2 n = clamp(int2(tid) + int2(j % 3 - 1, j / 3 - 1), int2(0, 0), int2(W - 1, H - 1));
        c9[j] = centres[n.y * W + n.x];
    }
    uint ids[16]; uint distinct = 0, missed = 0;
    [loop] for (uint s = 0; s < 16; ++s)
    {
        float2 jit = float2(((s & 3) + 0.5) / 4.0, ((s >> 2) + 0.5) / 4.0);
        uint id = traceIdentity(float2(tid) + jit);
        bool seen = false;
        [loop] for (uint k = 0; k < distinct; ++k) if (ids[k] == id) seen = true;
        if (!seen) ids[distinct++] = id;
        bool in9 = false;
        [unroll] for (int j2 = 0; j2 < 9; ++j2) if (c9[j2] == id) in9 = true;
        if (!in9) missed++;
    }
    outb[idx] = distinct | (missed << 8);
}
