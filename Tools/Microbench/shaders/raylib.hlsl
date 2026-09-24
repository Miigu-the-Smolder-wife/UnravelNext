// Ray floor, DispatchRays pipeline (lib_6_6). Same makeRay contract as rays.hlsl.
// Extra: P[3].w = runtime ray flags, P[2].w = height for mode 2, P[4].w = hit group index,
// P[5].w = base SRV index of 8 material textures (HEAVY_HIT), mode 4 = OMM calibration grid.
// Hit groups: HG (ClosestHit + AnyHit alpha test), HGAccept (AnyHit that accepts, for calibration).
// Variants: NV_SER = NVAPI hit-object trace + reorder + invoke, HEAVY_HIT = divergent material work in closest hit.

#ifdef NV_SER
#define NV_SHADER_EXTN_SLOT u999
#define NV_SHADER_EXTN_REGISTER_SPACE space0
#include "nvHLSLExtns.h"
#endif

RaytracingAccelerationStructure Tlas : register(t0);

struct Payload { float t; };
struct Attr { float2 b; };

void makeRay(uint3 tid, out RayDesc r)
{
    uint mode = P[1].y;
    uint s = pcg(tid.x + tid.y * P[1].x + P[1].w * 7919u);
    float2 sceneMin = float2(asfloat(P[2].x), asfloat(P[2].y));
    float size = asfloat(P[2].z);
    r.TMin = 0.0;
    r.TMax = asfloat(P[1].z);
    if (mode == 4)
    {
        float grid = (float)P[1].x;
        float u = (tid.x + 0.5) / grid, v = (tid.y + 0.5) / grid;
        r.Origin = float3(4.0 * tid.z + u, v, 1.0);
        r.Direction = float3(0, 0, -1);
        r.TMax = 4.0;
        return;
    }
    if (mode == 2)
    {
        float3 cam = float3(asfloat(P[4].x), asfloat(P[4].y), asfloat(P[4].z));
        float3 fwd = float3(asfloat(P[5].x), asfloat(P[5].y), asfloat(P[5].z));
        float3 right = float3(asfloat(P[6].x), asfloat(P[6].y), asfloat(P[6].z));
        float3 up = float3(asfloat(P[7].x), asfloat(P[7].y), asfloat(P[7].z));
        float tanHalf = asfloat(P[7].w);
        float W = (float)P[1].x, H = (float)P[2].w;
        float2 ndc = ((float2(tid.xy) + 0.5) / float2(W, H)) * 2.0 - 1.0;
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

[shader("raygeneration")]
void RayGen()
{
    uint3 tid = DispatchRaysIndex();
    RWStructuredBuffer<float> outb = ResourceDescriptorHeap[P[0].y];
    RayDesc r; makeRay(tid, r);
    Payload p; p.t = -1.0;
#ifdef NV_SER
    NvHitObject hit = NvTraceRayHitObject(Tlas, P[3].w, 0xff, P[4].w, 1, 0, r, p);
    NvReorderThread(hit, hit.GetInstanceIndex() & 15u, 4);
    NvInvokeHitObject(Tlas, hit, p);
#else
    TraceRay(Tlas, P[3].w, 0xff, P[4].w, 1, 0, r, p);
#endif
    uint3 dim = DispatchRaysDimensions();
    outb[(tid.z * dim.y + tid.y) * dim.x + tid.x] = p.t;
}

[shader("miss")]
void Miss(inout Payload p) { p.t = -1.0; }

[shader("closesthit")]
void ClosestHit(inout Payload p, in Attr a)
{
#ifdef HEAVY_HIT
    // Strongly divergent "material" work: 16 code paths selected by instance, each sampling its own 2048^2
    // texture (16 x 16 MB, far beyond L2) at a footprint spread by primitive index, plus path-specific ALU.
    uint mat = InstanceIndex() & 15u;
    Texture2D<float4> tex = ResourceDescriptorHeap[P[5].w + mat];
    SamplerState lin = SamplerDescriptorHeap[0];
    float2 uv = float2(a.b.x * 37.0 + PrimitiveIndex() * 0.173, a.b.y * 41.0 + InstanceIndex() * 0.0131);
    float4 c = tex.SampleLevel(lin, uv, 0);
    float3 n = normalize(float3(c.x - 0.5, 1.0, c.y - 0.5));
    float3 v = -WorldRayDirection();
    float acc = 0;
    switch (mat)
    {
    case 0:  [loop] for (int i = 0; i < 8; ++i) acc += pow(saturate(dot(n, normalize(v + float3(sin(i + c.z), 0, 0)))), 8.0 + i); break;
    case 1:  [loop] for (int i = 0; i < 24; ++i) acc += sin(c.x * i + acc) * 0.01; break;
    case 2:  acc = length(c.xyz) * exp(-c.w * 3.0) + sqrt(abs(c.x - c.y)); break;
    case 3:  [loop] for (int i = 0; i < 16; ++i) acc += frac(c.y * 13.0 * i + acc * 0.5); break;
    case 4:  acc = dot(c, float4(0.2, 0.3, 0.4, 0.1)); [loop] for (int i = 0; i < 40; ++i) acc = mad(acc, 0.999, c.z * 0.001); break;
    case 5:  acc = tex.SampleLevel(lin, uv * 3.0, 0).x + tex.SampleLevel(lin, uv * 5.0, 0).y; break;
    case 6:  [loop] for (int i = 0; i < 12; ++i) acc += cos(c.w * i) * pow(saturate(n.y), (float)i); break;
    case 7:  acc = atan2(c.x, c.y + 0.1) + log(1.0 + c.z); break;
    case 8:  [loop] for (int i = 0; i < 32; ++i) acc += (c.x > 0.5 ? c.y : c.z) * i * 0.001; break;
    case 9:  acc = tex.SampleLevel(lin, uv.yx, 0).z * 2.0; break;
    case 10: [loop] for (int i = 0; i < 20; ++i) acc = acc * 0.9 + sqrt(abs(c.x * i - c.y)); break;
    case 11: acc = rsqrt(c.x + 0.01) + rcp(c.y + 0.01); break;
    case 12: [loop] for (int i = 0; i < 28; ++i) acc += exp2(-i * c.z); break;
    case 13: acc = tex.SampleLevel(lin, uv + float2(0.01, 0), 0).x - tex.SampleLevel(lin, uv - float2(0.01, 0), 0).x; break;
    case 14: [loop] for (int i = 0; i < 10; ++i) acc += pow(saturate(dot(n, v)), (float)(i + 1)) * c.w; break;
    default: acc = dot(n, v) * c.x; break;
    }
    p.t = RayTCurrent() + acc * 1e-6;
#else
    p.t = RayTCurrent();
#endif
}

[shader("anyhit")]
void AnyHit(inout Payload p, in Attr a)
{
    Texture2D<float> alphaTex = ResourceDescriptorHeap[P[0].z];
    StructuredBuffer<float2> uvs = ResourceDescriptorHeap[P[0].w];
    SamplerState lin = SamplerDescriptorHeap[0];
    uint prim = PrimitiveIndex();
    float2 b = a.b;
    float2 uv = uvs[prim * 3] * (1 - b.x - b.y) + uvs[prim * 3 + 1] * b.x + uvs[prim * 3 + 2] * b.y;
    if (alphaTex.SampleLevel(lin, uv, 0) <= 0.5) IgnoreHit();
}

[shader("anyhit")]
void AnyHitAccept(inout Payload p, in Attr a) { }
