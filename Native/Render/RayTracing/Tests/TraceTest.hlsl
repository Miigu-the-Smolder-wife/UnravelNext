// unx-kernel: lib_6_6 main
// RayScene correctness test (Tests/RayScene.cpp): closest hit over both TLASes, hit identity, surface reconstruction and
// visibility rays, compared with a CPU brute-force intersector.
// P[0] = { rays SRV (TestRay), results UAV (TestResult), ray count, 0 }; P[6], P[7] = RtSceneSrvs.
#include "RayTracing/RayShaders.hlsli"

struct TestRay
{
    float3 origin;
    float tMax;
    float3 direction;
    float visibleTMax;  // visibility ray length (0 = skip)
};

struct TestResult
{
    float t;
    uint sceneInstance;
    uint meshTriangle;
    uint flags;          // bit 0 front face, bit 1 visibility ray unoccluded, bit 2 proxy geometry (no mesh triangle)
    float3 normal;       // RtSurface shading normal (world)
    float pad;
};

[shader("raygeneration")]
void TraceTestGen()
{
    const uint i = DispatchRaysIndex().x;
    if (i >= P[0].z) return;
    StructuredBuffer<TestRay> rays = ResourceDescriptorHeap[P[0].x];
    RWStructuredBuffer<TestResult> results = ResourceDescriptorHeap[P[0].y];
    const RtSceneSrvs s = rtScene();
    const TestRay r = rays[i];
    RayDesc d;
    d.Origin = r.origin;
    d.Direction = r.direction;
    d.TMin = 0;
    d.TMax = r.tMax;
    const RtHit h = rtTraceClosest(s, d, RAY_FLAG_NONE, RT_MASK_ALL);
    TestResult o;
    o.t = h.t;
    o.sceneInstance = UNX_NONE;
    o.meshTriangle = UNX_NONE;
    o.flags = 0;
    o.normal = 0;
    o.pad = 0;
    if (h.t >= 0)
    {
        RtGeometry g;
        const RtInstance ri = rtResolve(s, h, g);
        const GpuMesh mesh = loadMesh(loadInstance(ri.sceneInstance).mesh);
        o.sceneInstance = ri.sceneInstance;
        // Proxy geometry (R's index pool): its primitive index is the cut's, not the mesh's (flag bit 2).
        o.meshTriangle = (g.flags & RT_GEOMETRY_PROXY_INDICES) ? UNX_NONE : (g.indexOffset - mesh.indexOffset) / 3 + h.primitive;
        if (g.flags & RT_GEOMETRY_PROXY_INDICES) o.flags |= 4u;
        o.flags |= h.frontFace;
        o.normal = rtSurface(s, h, r.origin, r.direction).normal;
    }
    if (r.visibleTMax > 0)
    {
        d.TMax = r.visibleTMax;
        if (rtVisible(s, d, RT_MASK_ALL)) o.flags |= 2u;
    }
    results[i] = o;
}
