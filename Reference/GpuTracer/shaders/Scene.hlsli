// GPU scene access for the reference path tracer: bindings, the data functions the shared headers read, texture
// sampling, ray traversal (inline RayQuery, exact alpha test on non-opaque candidates) and surface evaluation. The
// surface and texture code is Reference/PathTracer/src/RtScene.cpp line for line; traversal replaces Embree (instance
// masks: bit 0 every instance, bit 1 shadow casters; no culling; the closest opaque hit).
#ifndef UNX_RT_SCENE_HLSLI
#define UNX_RT_SCENE_HLSLI
#include "../shared/Sampler.hlsli"
#include "../shared/Types.hlsli"

struct RtRoot
{
    uint constants;    // StructuredBuffer<RtConstants> descriptor
    uint x0;           // path kernel: pixel rectangle
    uint y0;
    uint w;
    uint h;
    uint sampleBegin;  // samples per half [begin, end)
    uint sampleEnd;
    uint pathBase;     // caustic kernel: first light path of this dispatch (per half)
    uint pathCount;
    uint passIndex;    // caustic kernel: sample pass (seeds)
    uint halfBase;     // first half of the dispatch (Z = halves): 0; a shutter epoch renders one half, 0 or 1
    uint pad1;
};
ConstantBuffer<RtRoot> g_root : register(b0);

RtConstants rtC()
{
    StructuredBuffer<RtConstants> b = ResourceDescriptorHeap[g_root.constants];
    return b[0];
}

static uint g_rays = 0;
static uint g_errors = 0;
static uint g_maxCandidates = 0;  // most candidates one ray of this thread visited (kRtCounterMaxCandidates)

float rtAlbedoTableFetch(uint i)
{
    StructuredBuffer<float> b = ResourceDescriptorHeap[rtC().b.albedoTable];
    return b[i];
}
float3 rtAtmTableFetch(uint ir, uint im)
{
    StructuredBuffer<float3> b = ResourceDescriptorHeap[rtC().b.atmTable];
    return b[ir * kRtAtmTableMu + im];
}
RtLight rtLightFetch(uint i)
{
    StructuredBuffer<RtLight> b = ResourceDescriptorHeap[rtC().b.lights];
    return b[i];
}
uint rtLightCellStart(uint cell)
{
    StructuredBuffer<uint> b = ResourceDescriptorHeap[rtC().b.cellStart];
    return b[cell];
}
uint rtLightCellLight(uint k)
{
    StructuredBuffer<uint> b = ResourceDescriptorHeap[rtC().b.cellLights];
    return b[k];
}

RtInstance rtInstance(uint i)
{
    StructuredBuffer<RtInstance> b = ResourceDescriptorHeap[rtC().b.instances];
    return b[i];
}
RtMesh rtMesh(uint i)
{
    StructuredBuffer<RtMesh> b = ResourceDescriptorHeap[rtC().b.meshes];
    return b[i];
}
RtMaterial rtMaterial(uint i)
{
    StructuredBuffer<RtMaterial> b = ResourceDescriptorHeap[rtC().b.materials];
    return b[i];
}
// float3x4::transformPoint / transformVector: m[i][0] x + m[i][1] y + m[i][2] z (+ m[i][3]), left to right.
float3 rtXformPointExact(RtInstance in_, float3 p)
{
    return float3(in_.row0.x * p.x + in_.row0.y * p.y + in_.row0.z * p.z + in_.row0.w, in_.row1.x * p.x + in_.row1.y * p.y + in_.row1.z * p.z + in_.row1.w,
                  in_.row2.x * p.x + in_.row2.y * p.y + in_.row2.z * p.z + in_.row2.w);
}
float3 rtXformVectorExact(RtInstance in_, float3 v)
{
    return float3(in_.row0.x * v.x + in_.row0.y * v.y + in_.row0.z * v.z, in_.row1.x * v.x + in_.row1.y * v.y + in_.row1.z * v.z,
                  in_.row2.x * v.x + in_.row2.y * v.y + in_.row2.z * v.z);
}

// Texture::sample: bilinear on mip 0 between the four nearest texel centres (decoded texels: sRGB already linear).
float4 rtSampleTexture(uint texture, float2 uv)
{
    StructuredBuffer<RtTexture> tb = ResourceDescriptorHeap[rtC().b.textures];
    StructuredBuffer<float4> texels = ResourceDescriptorHeap[rtC().b.texels];
    const RtTexture t = tb[texture];
    const float x = uv.x * (float)t.width - 0.5f, y = uv.y * (float)t.height - 0.5f;
    const float fx = floor(x), fy = floor(y);
    const float tx = x - fx, ty = y - fy;
    int x0 = (int)fx, y0 = (int)fy, x1 = x0 + 1, y1 = y0 + 1;
    const int w = (int)t.width, h = (int)t.height;
    if (t.wrap != 0)
    {
        x0 = ((x0 % w) + w) % w;
        x1 = ((x1 % w) + w) % w;
        y0 = ((y0 % h) + h) % h;
        y1 = ((y1 % h) + h) % h;
    }
    else
    {
        x0 = clamp(x0, 0, w - 1);
        x1 = clamp(x1, 0, w - 1);
        y0 = clamp(y0, 0, h - 1);
        y1 = clamp(y1, 0, h - 1);
    }
    const float4 a = texels[t.texelOffset + (uint)y0 * t.width + (uint)x0];
    const float4 b = texels[t.texelOffset + (uint)y0 * t.width + (uint)x1];
    const float4 c = texels[t.texelOffset + (uint)y1 * t.width + (uint)x0];
    const float4 d = texels[t.texelOffset + (uint)y1 * t.width + (uint)x1];
    return (a * (1 - tx) + b * tx) * (1 - ty) + (c * (1 - tx) + d * tx) * ty;
}

// Mesh triangle index and material of (instance, BLAS geometry = submesh, primitive).
uint rtSubmeshOf(RtInstance in_, uint geometry, out uint firstTriangle)
{
    StructuredBuffer<RtMeshSubmeshes> ms = ResourceDescriptorHeap[rtC().b.meshSubmeshes];
    StructuredBuffer<RtSubmesh> sm = ResourceDescriptorHeap[rtC().b.submeshes];
    const RtSubmesh s = sm[ms[in_.sourceMesh].first + geometry];
    firstTriangle = s.firstTriangle;
    if (in_.overrideOffset != kRtNone)
    {
        StructuredBuffer<uint> ov = ResourceDescriptorHeap[rtC().b.materialOverrides];
        return ov[in_.overrideOffset + geometry];
    }
    return s.material;
}

uint3 rtTriangleVertices(RtMesh m, uint triIndex)
{
    StructuredBuffer<uint> ib = ResourceDescriptorHeap[rtC().b.indices];
    const uint base = m.indexOffset + 3 * triIndex;
    return uint3(ib[base], ib[base + 1], ib[base + 2]);
}

float2 rtHitUv(RtMesh m, uint3 tri, float u, float v)
{
    if (m.hasUv == 0) return float2(0, 0);
    StructuredBuffer<float2> uvs = ResourceDescriptorHeap[rtC().b.uvs];
    const float2 a = uvs[m.vertexOffset + tri.x], b = uvs[m.vertexOffset + tri.y], c = uvs[m.vertexOffset + tri.z];
    const float w = 1 - u - v;
    return float2(a.x * w + b.x * u + c.x * v, a.y * w + b.y * u + c.y * v);
}

// RtScene::alphaOpaque.
bool rtAlphaOpaque(uint instance, uint geometry, uint primitive, float u, float v)
{
    const RtInstance in_ = rtInstance(instance);
    uint first;
    const uint mat = rtSubmeshOf(in_, geometry, first);
    const RtMaterial m = rtMaterial(mat);
    if (m.alphaTested == 0) return true;
    // Alpha uses the source mesh's uv streams (a deformed copy keeps them unchanged).
    const RtMesh srcMesh = rtMesh(in_.sourceMesh);
    const float2 uv = rtHitUv(srcMesh, rtTriangleVertices(srcMesh, first + primitive), u, v);
    return rtSampleTexture(m.baseColorTexture, uv).w >= m.alphaCutoff;
}

struct RtHit
{
    uint instance;
    uint geometry;
    uint primitive;  // within the geometry
    float u;
    float v;
    float t;
};

static const uint kRtMaskAll = 1u;
static const uint kRtMaskShadow = 2u;
// Candidates (non-opaque triangles resolved by the alpha test) one ray may visit: a structural term of a path iteration's
// worst cost (PathTrace.hlsl). Beyond it the ray stops with kRtErrorTraversal and the render fails - never a silent
// answer; the largest count a render met is reported (kRtCounterMaxCandidates).
static const uint kRtMaxCandidates = 4096;
static const float kRtFarT = 3.402823e38f;

bool rtIntersect(float3 o, float3 d, float tnear, float tfar, uint mask, out RtHit hit)
{
    RaytracingAccelerationStructure tlas = ResourceDescriptorHeap[rtC().b.tlas];
    RayQuery<RAY_FLAG_NONE> q;
    RayDesc r;
    r.Origin = o;
    r.Direction = d;
    r.TMin = tnear;
    r.TMax = tfar;
    q.TraceRayInline(tlas, RAY_FLAG_NONE, mask, r);
    g_rays += 1;
    uint n = 0;
    while (q.Proceed())
    {
        if (++n > kRtMaxCandidates)
        {
            g_errors |= kRtErrorTraversal;
            q.Abort();
            break;
        }
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
        {
            const float2 bc = q.CandidateTriangleBarycentrics();
            if (rtAlphaOpaque(q.CandidateInstanceID(), q.CandidateGeometryIndex(), q.CandidatePrimitiveIndex(), bc.x, bc.y)) q.CommitNonOpaqueTriangleHit();
        }
    }
    g_maxCandidates = max(g_maxCandidates, n);
    hit.instance = 0;
    hit.geometry = 0;
    hit.primitive = 0;
    hit.u = hit.v = hit.t = 0;
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return false;
    hit.instance = q.CommittedInstanceID();
    hit.geometry = q.CommittedGeometryIndex();
    hit.primitive = q.CommittedPrimitiveIndex();
    const float2 bc = q.CommittedTriangleBarycentrics();
    hit.u = bc.x;
    hit.v = bc.y;
    hit.t = q.CommittedRayT();
    return true;
}

// Shadow casters only (RtScene::occluded).
bool rtOccluded(float3 o, float3 d, float tnear, float tfar)
{
    RaytracingAccelerationStructure tlas = ResourceDescriptorHeap[rtC().b.tlas];
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
    RayDesc r;
    r.Origin = o;
    r.Direction = d;
    r.TMin = tnear;
    r.TMax = tfar;
    q.TraceRayInline(tlas, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, kRtMaskShadow, r);
    g_rays += 1;
    uint n = 0;
    while (q.Proceed())
    {
        if (++n > kRtMaxCandidates)
        {
            g_errors |= kRtErrorTraversal;
            q.Abort();
            break;
        }
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
        {
            const float2 bc = q.CandidateTriangleBarycentrics();
            if (rtAlphaOpaque(q.CandidateInstanceID(), q.CandidateGeometryIndex(), q.CandidatePrimitiveIndex(), bc.x, bc.y)) q.CommitNonOpaqueTriangleHit();
        }
    }
    g_maxCandidates = max(g_maxCandidates, n);
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

bool rtDeformed(uint instance) { return (rtInstance(instance).flags & kRtInstanceDeformed) != 0; }

// RtScene::surface.
RtSurface rtSurfaceAt(RtHit hit, float3 rayDir)
{
    const RtInstance in_ = rtInstance(hit.instance);
    const RtMesh md = rtMesh(in_.mesh);
    const bool world = (in_.flags & kRtInstanceWorld) != 0;
    uint first;
    const uint matIndex = rtSubmeshOf(in_, hit.geometry, first);
    const uint triIndex = first + hit.primitive;
    const uint3 tri = rtTriangleVertices(md, triIndex);
    const float w = 1 - hit.u - hit.v, u = hit.u, v = hit.v;
    StructuredBuffer<float3> P = ResourceDescriptorHeap[rtC().b.positions];
    StructuredBuffer<float3> N = ResourceDescriptorHeap[rtC().b.normals];
    float3 p0 = P[md.vertexOffset + tri.x], p1 = P[md.vertexOffset + tri.y], p2 = P[md.vertexOffset + tri.z];
    if (!world)
    {
        p0 = rtXformPointExact(in_, p0);
        p1 = rtXformPointExact(in_, p1);
        p2 = rtXformPointExact(in_, p2);
    }
    RtSurface s;
    s.p = p0 * w + p1 * u + p2 * v;
    s.ng = normalize(cross(p1 - p0, p2 - p0));
    s.extent = sqrt(max(max(dot(p0 - s.p, p0 - s.p), dot(p1 - s.p, p1 - s.p)), dot(p2 - s.p, p2 - s.p)));
    float3 nObj = N[md.vertexOffset + tri.x] * w + N[md.vertexOffset + tri.y] * u + N[md.vertexOffset + tri.z] * v;
    float3 n = normalize(world ? nObj : rtXformVectorExact(in_, nObj));
    s.material = matIndex;
    const RtMaterial mat = rtMaterial(matIndex);
    // uv from the source mesh (a deformed copy moves positions, normals and tangents only; indices are shared).
    const float2 uv = rtHitUv(rtMesh(in_.sourceMesh), tri, u, v);
    float3 base = mat.baseColor;
    if (mat.baseColorTexture != kRtNone)
    {
        const float4 t = rtSampleTexture(mat.baseColorTexture, uv);
        base = base * t.xyz;
    }
    float rough = mat.roughness, metal = mat.metallic;
    if (mat.roughMetalTexture != kRtNone)
    {
        const float4 t = rtSampleTexture(mat.roughMetalTexture, uv);
        rough *= t.x;
        metal *= t.y;
    }
    if (mat.normalTexture != kRtNone && md.hasTangents != 0)
    {
        StructuredBuffer<float4> T = ResourceDescriptorHeap[rtC().b.tangents];
        const float4 t0 = T[md.vertexOffset + tri.x], t1 = T[md.vertexOffset + tri.y], t2 = T[md.vertexOffset + tri.z];
        const float3 tl = float3(t0.x * w + t1.x * u + t2.x * v, t0.y * w + t1.y * u + t2.y * v, t0.z * w + t1.z * u + t2.z * v);
        float3 tg = world ? tl : rtXformVectorExact(in_, tl);
        tg = tg - n * dot(n, tg);
        if (dot(tg, tg) > 1e-20f)
        {
            tg = normalize(tg);
            const float3 bt = cross(n, tg) * t0.w;
            const float4 nt = rtSampleTexture(mat.normalTexture, uv);
            const float x = 2 * nt.x - 1, y = 2 * nt.y - 1, z = sqrt(max(0.0f, 1 - x * x - y * y));
            n = normalize(tg * x + bt * y + n * z);
        }
    }
    if (dot(n, s.ng) < 0) n = n - s.ng * (2 * dot(n, s.ng));
    const float3 wo = -rayDir;
    s.frontFacing = dot(s.ng, wo) >= 0;
    if (!s.frontFacing && mat.twoSided != 0)
    {
        s.ng = -s.ng;
        n = -n;
        s.frontFacing = true;
    }
    const float nv = dot(n, wo);
    if (s.frontFacing && nv < 1e-4f) n = normalize(n + wo * (1e-4f - nv));
    s.ns = n;
    s.bsdf.cls = mat.cls;
    s.bsdf.baseColor = base;
    s.bsdf.roughness = clamp(rough, 0.0f, 1.0f);
    s.bsdf.metallic = clamp(metal, 0.0f, 1.0f);
    s.bsdf.specular = mat.specular;
    s.bsdf.transmission = mat.transmission;
    s.emission = float3(0, 0, 0);
    if (mat.emissive.x > 0 || mat.emissive.y > 0 || mat.emissive.z > 0)
    {
        float3 e = mat.emissive;
        if (mat.emissiveTexture != kRtNone) e = e * rtSampleTexture(mat.emissiveTexture, uv).xyz;
        if (s.frontFacing) s.emission = e;
    }
    return s;
}

// Planet ground outside the scene geometry: Lambert with the ground albedo, normal from the planet centre.
RtSurface rtGroundSurface(RtAtmosphere a, float3 p)
{
    RtSurface s;
    s.p = p;
    s.ng = s.ns = normalize(float3(p.x, p.y + a.R, p.z));
    s.frontFacing = true;
    s.bsdf.cls = kRtClassStandard;
    s.bsdf.baseColor = a.groundAlbedo;
    s.bsdf.roughness = 0.5f;
    s.bsdf.metallic = 0;
    s.bsdf.specular = 0.5f;
    s.bsdf.transmission = 0;
    s.emission = float3(0, 0, 0);
    s.material = kRtNone;
    s.extent = 0;
    return s;
}

bool rtSmooth(RtSurface s)
{
    if (s.material == kRtNone) return false;
    return rtMaterial(s.material).smooth != 0;
}

// Robust ray origin offset (Waechter & Binder, Ray Tracing Gems ch. 6), RtScene.cpp offsetRayOrigin.
// Ray origin off a surface (Waechter and Binder, Ray Tracing Gems 6) plus a triangle term. Their offset covers the
// rounding of p at its own magnitude; a hit point interpolated from a large triangle, and the hardware's intersection of
// the next ray with that triangle, carry errors of about eps x the triangle's extent (the watertight test shears the
// vertices, relative to the origin, by the ray's dominant axis): on a 4 km ground quad seen at 30 degrees half of the
// sun's shadow rays hit their own triangle (unx_test_reference --gpu sunparts). So the origin also moves along n by
// kRtTriangleOffset x extent (16 float ulps of the extent: < 1e-5 m for triangles below 10 m, 1.9 mm for a 2 km
// half-diagonal). Condition: geometry closer to the surface than that is not seen by rays leaving it.
static const float kRtTriangleOffset = 16.0f / 16777216.0f;
float3 rtOffsetRayOrigin(float3 p, float3 n, float extent)
{
    const float origin = 1.0f / 32.0f, floatScale = 1.0f / 65536.0f, intScale = 256.0f;
    const int3 oi = int3((int)(intScale * n.x), (int)(intScale * n.y), (int)(intScale * n.z));
    const float3 pi = float3(asfloat(asint(p.x) + (p.x < 0 ? -oi.x : oi.x)), asfloat(asint(p.y) + (p.y < 0 ? -oi.y : oi.y)),
                             asfloat(asint(p.z) + (p.z < 0 ? -oi.z : oi.z)));
    return float3(abs(p.x) < origin ? p.x + floatScale * n.x : pi.x, abs(p.y) < origin ? p.y + floatScale * n.y : pi.y,
                  abs(p.z) < origin ? p.z + floatScale * n.z : pi.z) + n * (kRtTriangleOffset * extent);
}

// Wave-aggregated counters.
void rtFlushCounters(uint nans, uint truncated, uint splatNans)
{
    RWByteAddressBuffer c = ResourceDescriptorHeap[rtC().b.counters];
    const uint rays = WaveActiveSum(g_rays), n = WaveActiveSum(nans), t = WaveActiveSum(truncated), sn = WaveActiveSum(splatNans);
    const uint e = WaveActiveBitOr(g_errors), mc = WaveActiveMax(g_maxCandidates);
    if (WaveIsFirstLane())
    {
        if (rays) c.InterlockedAdd(4 * kRtCounterRays, rays);
        if (n) c.InterlockedAdd(4 * kRtCounterNans, n);
        if (t) c.InterlockedAdd(4 * kRtCounterTruncated, t);
        if (sn) c.InterlockedAdd(4 * kRtCounterSplatNans, sn);
        if (e) c.InterlockedOr(4 * kRtCounterErrors, e);
        if (mc) c.InterlockedMax(4 * kRtCounterMaxCandidates, mc);
    }
}
#endif
