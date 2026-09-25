// Bench 4: the shading kernel with tile-shared probe SH, in-kernel air fetches and a K atlas tap (COVERAGE_REDESIGN_KO.md 4.4).
// Per 8x8 tile group:
//   TILE_SH 1: the 3 x 3 probe records around the tile (80 B each: position, normal, SH 27 fp16) go to groupshared once;
//              per pixel plane-distance/normal weights over the 4 nearest and an SH evaluation (ALU only).
//   TILE_SH 0: per pixel, the 4 nearest probe records are loaded directly (R's gather pattern).
//   AIR 1: three trilinear fetches from the air volume (RGBA16F 3D, 160 x 90 x 195 at 4K), K 1: one bilinear tap
//   from the K atlas (RGBA8, (probesX * 14) x (probesY * 8)); LIGHTS: local light loop over 8 records (80 B).
//   EXTRA (0/32/64): extra live values held across the fetches (register pressure), folded into the output.
// P[0] = { width, height, probesX, probesY }, P[1] = { gbuffer, depth, vis, probes SRV }
// P[2] = { air volume SRV, atlas SRV, lights SRV, output UAV }
#include "common.hlsli"
#ifndef TILE_SH
#define TILE_SH 1
#endif
#ifndef AIR
#define AIR 1
#endif
#ifndef KTAP
#define KTAP 1
#endif
#ifndef LIGHTS
#define LIGHTS 8
#endif
#ifndef EXTRA
#define EXTRA 0
#endif

struct Probe { float4 posNormal; float4 sh[4]; };  // position xyz + oct normal (asuint); 27 fp16 SH in 4 x uint4 (13.5 dwords) + occlusion
struct Light { float4 posRange; float4 colorIntensity; float4 dirSpot; float4 pad0; float4 pad1; };

groupshared float4 gsProbe[9][5];

float3 shIrradiance(float4 sh[4], float3 n)
{
    // 9 x RGB fp16 coefficients packed two per dword; L2 irradiance convolution constants.
    float3 c[9];
    [unroll] for (uint i = 0; i < 9; ++i)
    {
        const uint dwordR = (i * 3) / 2, dwordG = (i * 3 + 1) / 2, dwordB = (i * 3 + 2) / 2;
        const uint r = asuint(sh[dwordR / 4][dwordR % 4]) >> (((i * 3) & 1) * 16);
        const uint g = asuint(sh[dwordG / 4][dwordG % 4]) >> (((i * 3 + 1) & 1) * 16);
        const uint b = asuint(sh[dwordB / 4][dwordB % 4]) >> (((i * 3 + 2) & 1) * 16);
        c[i] = float3(f16tof32(r), f16tof32(g), f16tof32(b));
    }
    const float x = n.x, y = n.y, z = n.z;
    return max(0.0, 0.886227 * c[0] + 1.023328 * (y * c[1] + z * c[2] + x * c[3]) + 0.858086 * (x * y * c[4] + y * z * c[5] + x * z * c[7]) + 0.247708 * (3 * z * z - 1) * c[6] + 0.429043 * (x * x - y * y) * c[8]);
}

float probeWeight(float3 p, float3 n, float3 probePos, float3 probeNormal)
{
    const float3 dp = p - probePos;
    const float plane = abs(dot(dp, probeNormal)), dist = length(dp);
    return saturate(dot(n, probeNormal)) / (1e-3 + dist * (1 + 8 * plane));
}

[numthreads(8, 8, 1)]
void ShadeCS(uint3 gid : SV_GroupID, uint2 tid : SV_GroupThreadID, uint gindex : SV_GroupIndex)
{
    const uint W = P[0].x, H = P[0].y, probesX = P[0].z, probesY = P[0].w;
    const uint2 pixel = gid.xy * 8 + tid;
    Texture2D<uint2> gbuffer = ResourceDescriptorHeap[P[1].x];
    Texture2D<float> depthTex = ResourceDescriptorHeap[P[1].y];
    Texture2D<uint> vis = ResourceDescriptorHeap[P[1].z];
    StructuredBuffer<Probe> probes = ResourceDescriptorHeap[P[1].w];
#if TILE_SH
    // 3 x 3 probes around the tile: probe (px, py) covers pixels [8 px, 8 px + 8); neighbours -1..+1.
    if (gindex < 9)
    {
        const int2 pc = clamp(int2(gid.xy) + int2(gindex % 3, gindex / 3) - 1, 0, int2(probesX - 1, probesY - 1));
        const Probe pr = probes[pc.y * probesX + pc.x];
        gsProbe[gindex][0] = pr.posNormal;
        gsProbe[gindex][1] = pr.sh[0]; gsProbe[gindex][2] = pr.sh[1]; gsProbe[gindex][3] = pr.sh[2]; gsProbe[gindex][4] = pr.sh[3];
    }
    GroupMemoryBarrierWithGroupSync();
#endif
    if (any(pixel >= uint2(W, H))) return;
    const uint2 g = gbuffer[pixel];
    const float z = depthTex[pixel];
    const uint id = vis[pixel];
    const float3 n = octDecode(g.x);
    const float3 albedo = float3(g.y & 255u, (g.y >> 8) & 255u, (g.y >> 16) & 255u) / 255.0;
    const float rough = (g.y >> 24) / 255.0;
    const float2 uv = (float2(pixel) + 0.5) / float2(W, H);
    const float3 pos = float3((uv.x - 0.5) * 2 * z, (0.5 - uv.y) * 2 * z * 0.5625, z);  // camera-relative
    const float3 v = -normalize(pos);
    float4 air0 = float4(0, 0, 0, 1), air1 = 0, air2 = 0;
#if AIR
    // Issued first, consumed last (the latency overlaps the rest).
    Texture3D<float4> volume = ResourceDescriptorHeap[P[2].x];
    const float slice = saturate(log2(max(z, 0.5) / 0.5) / log2(65536.0 / 0.5));
    air0 = volume.SampleLevel(g_linear, float3(uv, slice / 3.0), 0);
    air1 = volume.SampleLevel(g_linear, float3(uv, slice / 3.0 + 1.0 / 3.0), 0);
    air2 = volume.SampleLevel(g_linear, float3(uv, slice / 3.0 + 2.0 / 3.0), 0);
#endif
#if EXTRA > 0
    float extra[EXTRA];
    [unroll] for (uint e = 0; e < EXTRA; ++e) extra[e] = sin(z * (e + 1) * 0.37 + id * 1e-3) * albedo[e % 3];
#endif
    // Indirect irradiance from the 4 nearest probes.
    float3 E = 0;
    {
        const float2 pf = (float2(pixel) + 0.5) / 8.0 - 0.5;
        const int2 p0 = int2(floor(pf));
        float wsum = 0;
        [unroll] for (uint q = 0; q < 4; ++q)
        {
            const int2 pc = clamp(p0 + int2(q & 1, q >> 1), 0, int2(probesX - 1, probesY - 1));
#if TILE_SH
            const int2 rel = pc - int2(gid.xy) + 1;
            const uint slot = clamp(rel.y, 0, 2) * 3 + clamp(rel.x, 0, 2);
            const float4 pn = gsProbe[slot][0];
            float4 sh[4] = { gsProbe[slot][1], gsProbe[slot][2], gsProbe[slot][3], gsProbe[slot][4] };
#else
            const Probe pr = probes[pc.y * probesX + pc.x];
            const float4 pn = pr.posNormal;
            float4 sh[4] = { pr.sh[0], pr.sh[1], pr.sh[2], pr.sh[3] };
#endif
            const float w = probeWeight(pos, n, pn.xyz, octDecode(asuint(pn.w)));
            E += w * shIrradiance(sh, n);
            wsum += w;
        }
        E /= max(wsum, 1e-6);
    }
    // Sun and local lights (GGX-ish).
    const float a2 = max(rough * rough, 1e-3);
    float3 radiance = 0;
    {
        const float3 l = normalize(float3(0.3, 0.8, 0.5)), h = normalize(l + v);
        const float ndl = saturate(dot(n, l)), ndh = saturate(dot(n, h));
        const float d = a2 / (3.14159 * pow(ndh * ndh * (a2 - 1) + 1, 2));
        radiance += (albedo / 3.14159 * ndl + 0.04 * d * 0.25) * 3.0;
    }
#if LIGHTS > 0
    StructuredBuffer<Light> lights = ResourceDescriptorHeap[P[2].z];
    [unroll] for (uint li = 0; li < LIGHTS; ++li)
    {
        const Light L = lights[li];
        const float3 dl = L.posRange.xyz - pos;
        const float dist2 = max(dot(dl, dl), 1e-2);
        const float3 l = dl * rsqrt(dist2), h = normalize(l + v);
        const float ndl = saturate(dot(n, l)), ndh = saturate(dot(n, h));
        const float win = saturate(1 - pow(dist2 / (L.posRange.w * L.posRange.w), 2));
        const float d = a2 / (3.14159 * pow(ndh * ndh * (a2 - 1) + 1, 2));
        radiance += L.colorIntensity.rgb * (L.colorIntensity.w * win / dist2) * (albedo / 3.14159 * ndl + 0.04 * d * 0.25) * ndl;
    }
#endif
    float3 K = 0;
#if KTAP
    Texture2D<float4> atlas = ResourceDescriptorHeap[P[2].y];
    const float3 r = reflect(-v, n);
    const float2 probeUv = (float2(gid.xy) * float2(14.0, 8.0) + float2(7.0, 4.0) + r.xy * 2.0) / float2(probesX * 14.0, probesY * 8.0);
    K = atlas.SampleLevel(g_linear, probeUv, 0).rgb;
#endif
    radiance += albedo * E + K * (0.04 + 0.96 * pow(1 - saturate(dot(n, v)), 5));
#if EXTRA > 0
    [unroll] for (uint e2 = 0; e2 < EXTRA; ++e2) radiance += extra[e2] * 1e-3;
#endif
    radiance = radiance * air0.a + air0.rgb * 0.5 + air1.rgb * 0.25 + air2.rgb * 0.25;
    RWTexture2D<float4> output = ResourceDescriptorHeap[P[2].w];
    output[pixel] = float4(radiance / (1 + radiance), 1);
}
