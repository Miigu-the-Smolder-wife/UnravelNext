// Extra floors for the design review: virtual-shadow-map page lookups, a synthetic fused shading kernel,
// and a material-point-method (MPM) step (sort, P2G, grid, G2P).

// =========================================================== VSM lookup (page-table indirection + N taps)
// Clipmap: 12 levels, level k extent 16*2^k m, 16384 virtual texels, 128-texel pages, page table 128x128 per level.
// Atlas: 1024 physical pages of 128x128 float depth (64 MB). Physical page id = pageTable[level*16384 + py*128 + px].
float vsmVisibility(StructuredBuffer<uint> pageTable, StructuredBuffer<float> atlas, float2 lightUV, float receiverDepth,
                    uint level, uint taps, float2 tapDir)
{
    float extent = 16.0 * exp2((float)level);
    float texel = extent / 16384.0;
    float2 t = lightUV / texel;
    int2 ti = (int2)floor(t);
    int2 page = (ti >> 7) & 127;
    uint entry = pageTable[level * 16384u + page.y * 128u + page.x];
    float vis = 0;
    [loop] for (uint k = 0; k < taps; ++k)
    {
        float o = (float)k - (taps - 1) * 0.5;
        int2 tt = ti + (int2)round(tapDir * o);
        int2 pg = (tt >> 7) & 127;
        int2 loc = tt & 127;
        uint e = all(pg == page) ? entry : pageTable[level * 16384u + pg.y * 128u + pg.x];
        float d = atlas[e * 16384u + loc.y * 128u + loc.x];
        vis += (receiverDepth <= d + 0.002) ? 1.0 : 0.0;
    }
    return vis / taps;
}

// Synthetic 4K view: camera 1.7 m high, 10 deg pitch down, 60 deg vertical FOV. Ground plane + far buildings + clutter.
void syntheticPixel(uint2 tid, float W, float H, uint seed, uint scattered, out float3 wp, out float dist)
{
    float2 ndc = ((float2(tid) + 0.5) / float2(W, H)) * 2.0 - 1.0; ndc.y = -ndc.y;
    float tanHalf = 0.57735, aspect = W / H;
    float3 dir = normalize(float3(ndc.x * tanHalf * aspect, ndc.y * tanHalf, 1.0));
    float cp = cos(-0.1745), sp = sin(-0.1745);
    dir = float3(dir.x, dir.y * cp - dir.z * sp, dir.y * sp + dir.z * cp);
    float3 cam = float3(0, 1.7, 0);
    dist = (dir.y < -0.001) ? (-cam.y / dir.y) : 200.0;
    uint h = pcg(tid.x + tid.y * (uint)W + seed);
    if (scattered != 0) dist = 1.0 + (h & 0xffff) / 65535.0 * 49.0;
    else if ((h & 7) == 0) dist = 1.0 + ((h >> 3) & 0xffff) / 65535.0 * 49.0;
    wp = cam + dir * dist;
}

uint clipLevel(float dist)
{
    float footprint = dist / 1871.0;               // world size of one pixel
    float level0Texel = 16.0 / 16384.0;
    return clamp((uint)ceil(log2(max(footprint / level0Texel, 1.0))), 0u, 11u);
}

// P[0].x sun page table SRV, P[0].y sun atlas SRV, P[0].z out UAV, P[0].w scattered (0/1)
// P[1].x W, P[1].y H, P[1].z taps, P[1].w seed, P[2].x local page table SRV, P[2].y local atlas SRV, P[2].z locals per 2 pixels (3 = 1.5 avg)
[numthreads(8, 8, 1)]
void VsmLookupCS(uint2 tid : SV_DispatchThreadID)
{
    StructuredBuffer<uint> sunTable = ResourceDescriptorHeap[P[0].x];
    StructuredBuffer<float> sunAtlas = ResourceDescriptorHeap[P[0].y];
    StructuredBuffer<uint> locTable = ResourceDescriptorHeap[P[2].x];
    StructuredBuffer<float> locAtlas = ResourceDescriptorHeap[P[2].y];
    RWStructuredBuffer<float> outb = ResourceDescriptorHeap[P[0].z];
    float W = (float)P[1].x, H = (float)P[1].y; uint taps = P[1].z;
    float3 wp; float dist; syntheticPixel(tid, W, H, P[1].w, P[0].w, wp, dist);
    uint h = pcg(tid.x * 7u + tid.y * 131u + P[1].w);
    float ang = (h & 255) * (3.14159265 / 256.0);
    float2 tapDir = float2(cos(ang), sin(ang));
    float3 sun = normalize(float3(0.5, 0.64, 0.58));
    float3 su = normalize(cross(sun, float3(0, 1, 0))), sv = cross(su, sun);
    uint level = clipLevel(dist);
    float vis = vsmVisibility(sunTable, sunAtlas, float2(dot(wp, su), dot(wp, sv)), dot(wp, sun), level, taps, tapDir);
    // local lights: 1 or 2 per pixel (average 1.5), each with its own light space and level
    uint nLocal = ((tid.x + tid.y) & 1) ? 2u : 1u;
    if (P[2].z == 0) nLocal = 0;
    [loop] for (uint l = 0; l < nLocal; ++l)
    {
        float3 lp = float3(-20 + 40 * u01(h), 3 + 5 * u01(h), 5 + 30 * u01(h));
        float3 ld = normalize(lp - wp);
        float3 lu = normalize(cross(ld, float3(0, 1, 0))), lv = cross(lu, ld);
        vis += vsmVisibility(locTable, locAtlas, float2(dot(wp, lu), dot(wp, lv)) * 0.25, dot(wp, ld), (level + l) % 12u, taps, tapDir.yx);
    }
    outb[tid.y * P[1].x + tid.x] = vis;
}

// =========================================================== synthetic fused shading kernel (4K)
// P[0].x gbuffer SRV (uint4 per pixel), P[0].y out UAV (uint2 per pixel, packed half4), P[0].z probe SRV (float4), P[0].w froxel SRV (float4)
// P[1].x W, P[1].y H, P[1].z taps, P[1].w seed, P[2].x sun table, P[2].y sun atlas, P[2].z local table, P[2].w local atlas, P[3].x lights per pixel
float3 octDecode(float2 f)
{
    float3 n = float3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    float t = saturate(-n.z); n.xy += float2(n.x >= 0.0 ? -t : t, n.y >= 0.0 ? -t : t);
    return normalize(n);
}
float ggxD(float NoH, float a2) { float d = NoH * NoH * (a2 - 1.0) + 1.0; return a2 / (3.14159265 * d * d); }
float smithV(float NoV, float NoL, float a2)
{
    float gv = NoL * sqrt(NoV * NoV * (1.0 - a2) + a2), gl = NoV * sqrt(NoL * NoL * (1.0 - a2) + a2);
    return 0.5 / max(gv + gl, 1e-4);
}
// SHADE_MASK bits: 1 = VSM taps (else visibility read from a 4 B/pixel buffer at P[3].y), 2 = light loop,
// 4 = probes, 8 = froxel, 16 = prefetch all page-table entries before any atlas tap
#ifndef SHADE_MASK
#define SHADE_MASK 15
#endif
float vsmVisibilityPrefetched(StructuredBuffer<uint> pageTable, StructuredBuffer<float> atlas, float2 lightUV, float receiverDepth,
                              uint level, uint taps, float2 tapDir)
{
    // page ids for all taps first (independent loads), then all atlas taps (independent loads)
    float extent = 16.0 * exp2((float)level);
    float texel = extent / 16384.0;
    int2 ti = (int2)floor(lightUV / texel);
    uint entries[9]; int2 locs[9];
    [unroll] for (uint k = 0; k < 9; ++k)
    {
        float o = (float)k - (taps - 1) * 0.5;
        int2 tt = ti + (int2)round(tapDir * o);
        int2 pg = (tt >> 7) & 127;
        locs[k] = tt & 127;
        entries[k] = (k < taps) ? pageTable[level * 16384u + pg.y * 128u + pg.x] : 0;
    }
    float vis = 0;
    [unroll] for (uint k2 = 0; k2 < 9; ++k2)
    {
        if (k2 < taps)
        {
            float d = atlas[entries[k2] * 16384u + locs[k2].y * 128u + locs[k2].x];
            vis += (receiverDepth <= d + 0.002) ? 1.0 : 0.0;
        }
    }
    return vis / taps;
}
[numthreads(8, 8, 1)]
void ShadeCS(uint2 tid : SV_DispatchThreadID)
{
    StructuredBuffer<uint4> gbuf = ResourceDescriptorHeap[P[0].x];
    RWStructuredBuffer<uint2> outb = ResourceDescriptorHeap[P[0].y];
    StructuredBuffer<float4> probes = ResourceDescriptorHeap[P[0].z];
    StructuredBuffer<float4> froxels = ResourceDescriptorHeap[P[0].w];
    StructuredBuffer<uint> sunTable = ResourceDescriptorHeap[P[2].x];
    StructuredBuffer<float> sunAtlas = ResourceDescriptorHeap[P[2].y];
    StructuredBuffer<uint> locTable = ResourceDescriptorHeap[P[2].z];
    StructuredBuffer<float> locAtlas = ResourceDescriptorHeap[P[2].w];
    uint W = P[1].x, H = P[1].y, taps = P[1].z;
    uint idx = tid.y * W + tid.x;
    uint4 g = gbuf[idx];
    float3 n = octDecode(float2((g.x & 0xffff) / 32767.5 - 1.0, (g.x >> 16) / 32767.5 - 1.0));
    float3 albedo = float3(g.y & 255, (g.y >> 8) & 255, (g.y >> 16) & 255) / 255.0;
    float rough = max((g.y >> 24) / 255.0, 0.05), a2 = rough * rough * rough * rough;
    float3 f0 = lerp(0.04, albedo, (g.z & 255) / 255.0);
    float ao = ((g.z >> 8) & 255) / 255.0;
    float dist = asfloat(g.w);
    float3 wp; float d2; syntheticPixel(tid, (float)W, (float)H, 0, 0, wp, d2);
    wp = wp * (dist / max(d2, 1e-3));
    float3 V = normalize(float3(0, 1.7, 0) - wp);
    float NoV = saturate(dot(n, V)) + 1e-4;
    uint h = pcg(idx + P[1].w);
    float ang = (h & 255) * (3.14159265 / 256.0); float2 tapDir = float2(cos(ang), sin(ang));
    float3 sun = normalize(float3(0.5, 0.64, 0.58));
    float3 su = normalize(cross(sun, float3(0, 1, 0))), sv = cross(su, sun);
    uint level = clipLevel(dist);
    float3 color = 0;
    float visSun = 1.0, visLoc0 = 1.0, visLoc1 = 1.0;
#if SHADE_MASK & 1
    #if SHADE_MASK & 16
    visSun = vsmVisibilityPrefetched(sunTable, sunAtlas, float2(dot(wp, su), dot(wp, sv)), dot(wp, sun), level, taps, tapDir);
    #else
    visSun = vsmVisibility(sunTable, sunAtlas, float2(dot(wp, su), dot(wp, sv)), dot(wp, sun), level, taps, tapDir);
    #endif
#else
    {
        StructuredBuffer<uint> visBuf = ResourceDescriptorHeap[P[3].y];
        uint vb = visBuf[idx];
        visSun = (vb & 255) / 255.0; visLoc0 = ((vb >> 8) & 255) / 255.0; visLoc1 = ((vb >> 16) & 255) / 255.0;
    }
#endif
    // sun
    {
        float3 Hh = normalize(sun + V); float NoL = saturate(dot(n, sun)), NoH = saturate(dot(n, Hh)), VoH = saturate(dot(V, Hh));
        float3 F = f0 + (1 - f0) * pow(1 - VoH, 5.0);
        float3 spec = ggxD(NoH, a2) * smithV(NoV, NoL, a2) * F;
        color += (albedo / 3.14159265 * (1 - F) + spec) * NoL * visSun * float3(8.0, 7.6, 7.0);
    }
#if SHADE_MASK & 2
    // local lights (P[3].x per pixel; the first two shadowed with 5 taps)
    uint nl = P[3].x;
    [loop] for (uint l = 0; l < nl; ++l)
    {
        float3 lp = float3(-20 + 40 * u01(h), 3 + 5 * u01(h), 5 + 30 * u01(h));
        float3 L = lp - wp; float dd = dot(L, L); L *= rsqrt(dd);
        float vis = (l == 0) ? visLoc0 : (l == 1) ? visLoc1 : 1.0;
    #if SHADE_MASK & 1
        if (l < 2)
        {
            float3 lu = normalize(cross(L, float3(0, 1, 0))), lv = cross(lu, L);
        #if SHADE_MASK & 16
            vis = vsmVisibilityPrefetched(locTable, locAtlas, float2(dot(wp, lu), dot(wp, lv)) * 0.25, dot(wp, L), (level + l) % 12u, taps, tapDir.yx);
        #else
            vis = vsmVisibility(locTable, locAtlas, float2(dot(wp, lu), dot(wp, lv)) * 0.25, dot(wp, L), (level + l) % 12u, taps, tapDir.yx);
        #endif
        }
    #endif
        float3 Hh = normalize(L + V); float NoL = saturate(dot(n, L)), NoH = saturate(dot(n, Hh)), VoH = saturate(dot(V, Hh));
        float3 F = f0 + (1 - f0) * pow(1 - VoH, 5.0);
        float3 spec = ggxD(NoH, a2) * smithV(NoV, NoL, a2) * F;
        float att = 1.0 / (1.0 + dd) * saturate(1 - dd / 900.0);
        color += (albedo / 3.14159265 * (1 - F) + spec) * NoL * vis * att * float3(30, 25, 20);
    }
#endif
#if SHADE_MASK & 4
    // GI: 4 screen probes (bilinear over 8x8 grid), each 2 float4 (SH-ish)
    {
        uint pw = W / 8 + 1; float2 pc = (float2(tid) + 0.5) / 8.0 - 0.5; uint2 p0 = (uint2)max(pc, 0); float2 f = frac(pc);
        float4 i00 = probes[(p0.y * pw + p0.x) * 2], i10 = probes[(p0.y * pw + p0.x + 1) * 2], i01 = probes[((p0.y + 1) * pw + p0.x) * 2], i11 = probes[((p0.y + 1) * pw + p0.x + 1) * 2];
        float4 irr = lerp(lerp(i00, i10, f.x), lerp(i01, i11, f.x), f.y);
        color += albedo * (irr.rgb + irr.w * n.y * 0.5) * ao;
    }
#endif
#if SHADE_MASK & 8
    // atmosphere froxel (160x90x64 for 4K)
    {
        uint fz = (uint)clamp(log2(max(dist, 1.0)) * 6.0, 0.0, 63.0);
        float4 fx = froxels[(fz * 90 + tid.y / 24) * 160 + tid.x / 24];
        color = color * fx.w + fx.rgb;
    }
#endif
    outb[idx] = uint2(f32tof16(color.r) | (f32tof16(color.g) << 16), f32tof16(color.b) | (f32tof16(1.0) << 16));
}

// =========================================================== MPM step
// Particle 64 B: pm (pos xyz in cell units, mass), v (vel xyz, volume), c0, c1 (APIC C rows 0/1; row 2 recomputed).
struct Particle { float4 pm; float4 v; float4 c0; float4 c1; };
// P[0].x particle buffer, P[0].y sorted buffer, P[0].z block count UAV, P[0].w block offsets/cursor UAV
// P[1].x grid int UAV (4 int per node, fixed point 1/65536), P[1].y grid vel UAV (float4 per node), P[1].z N, P[1].w grid dim (128)
// P[2].x block id per particle UAV, P[2].y mode, P[2].z dt bits
#define GDIM 128u
#define BDIM 16u
uint nodeIndex(uint3 c) { return (c.z * GDIM + c.y) * GDIM + c.x; }
uint blockOf(uint3 c) { return ((c.z >> 3) * BDIM + (c.y >> 3)) * BDIM + (c.x >> 3); }
void weights(float f, out float3 w) { w = float3(0.5 * (1.5 - f) * (1.5 - f), 0.75 - (f - 1.0) * (f - 1.0), 0.5 * (f - 0.5) * (f - 0.5)); }

[numthreads(256, 1, 1)]
void MpmHistogramCS(uint i : SV_DispatchThreadID)
{
    if (i >= P[1].z) return;
    StructuredBuffer<Particle> parts = ResourceDescriptorHeap[P[0].x];
    RWStructuredBuffer<uint> count = ResourceDescriptorHeap[P[0].z];
    RWStructuredBuffer<uint> bid = ResourceDescriptorHeap[P[2].x];
    uint3 c = (uint3)clamp(parts[i].pm.xyz, 1.0, (float)(GDIM - 3));
    uint b = blockOf(c);
    InterlockedAdd(count[b], 1u);
    bid[i] = b;
}

groupshared uint g_scan[4096];
[numthreads(1024, 1, 1)]
void MpmPrefixCS(uint t : SV_GroupThreadID)
{
    RWStructuredBuffer<uint> count = ResourceDescriptorHeap[P[0].z];
    RWStructuredBuffer<uint> offsets = ResourceDescriptorHeap[P[0].w];
    [unroll] for (uint k = 0; k < 4; ++k) g_scan[t + k * 1024] = count[t + k * 1024];
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint s = 1; s < 4096; s <<= 1)
    {
        uint v[4];
        [unroll] for (uint k = 0; k < 4; ++k) { uint i = t + k * 1024; v[k] = (i >= s) ? g_scan[i - s] : 0; }
        GroupMemoryBarrierWithGroupSync();
        [unroll] for (uint k2 = 0; k2 < 4; ++k2) g_scan[t + k2 * 1024] += v[k2];
        GroupMemoryBarrierWithGroupSync();
    }
    [unroll] for (uint k3 = 0; k3 < 4; ++k3) { uint i = t + k3 * 1024; uint excl = g_scan[i] - count[i]; offsets[i] = excl; offsets[4096 + i] = excl; count[i] = 0; }
}

[numthreads(256, 1, 1)]
void MpmScatterCS(uint i : SV_DispatchThreadID)
{
    if (i >= P[1].z) return;
    StructuredBuffer<Particle> parts = ResourceDescriptorHeap[P[0].x];
    RWStructuredBuffer<Particle> sorted = ResourceDescriptorHeap[P[0].y];
    RWStructuredBuffer<uint> cursor = ResourceDescriptorHeap[P[0].w];
    StructuredBuffer<uint> bid = ResourceDescriptorHeap[P[2].x];
    uint slot; InterlockedAdd(cursor[4096 + bid[i]], 1u, slot);
    sorted[slot] = parts[i];
}

[numthreads(256, 1, 1)]
void MpmClearCS(uint i : SV_DispatchThreadID)
{
    RWStructuredBuffer<int4> grid = ResourceDescriptorHeap[P[1].x];
    if (i < GDIM * GDIM * GDIM) grid[i] = int4(0, 0, 0, 0);
}

// P2G through global atomics (fixed point), one thread per particle
[numthreads(256, 1, 1)]
void MpmP2GAtomicCS(uint i : SV_DispatchThreadID)
{
    if (i >= P[1].z) return;
    StructuredBuffer<Particle> parts = ResourceDescriptorHeap[P[0].y];
    RWStructuredBuffer<int> grid = ResourceDescriptorHeap[P[1].x];
    Particle p = parts[i];
    float3 pos = p.pm.xyz; float m = p.pm.w;
    int3 base = (int3)floor(pos - 0.5); float3 f = pos - (float3)base;
    float3 wx, wy, wz; weights(f.x, wx); weights(f.y, wy); weights(f.z, wz);
    float3 c2 = float3(p.c0.w, p.c1.w, 0.0);
    [unroll] for (int z = 0; z < 3; ++z) [unroll] for (int y = 0; y < 3; ++y) [unroll] for (int x = 0; x < 3; ++x)
    {
        float w = wx[x] * wy[y] * wz[z];
        float3 dpos = float3(x, y, z) - f;
        float3 mom = m * w * (p.v.xyz + float3(dot(p.c0.xyz, dpos), dot(p.c1.xyz, dpos), dot(c2, dpos)));
        uint n = nodeIndex((uint3)(base + int3(x, y, z))) * 4u;
        InterlockedAdd(grid[n], (int)(m * w * 65536.0));
        InterlockedAdd(grid[n + 1], (int)(mom.x * 65536.0));
        InterlockedAdd(grid[n + 2], (int)(mom.y * 65536.0));
        InterlockedAdd(grid[n + 3], (int)(mom.z * 65536.0));
    }
}

// P2G through groupshared accumulation per 8^3 block (10^3 nodes with halo), then one global atomic per touched node
groupshared int g_lds[1000 * 4];
[numthreads(256, 1, 1)]
void MpmP2GBlockCS(uint t : SV_GroupThreadID, uint3 gid : SV_GroupID)
{
    StructuredBuffer<Particle> parts = ResourceDescriptorHeap[P[0].y];
    RWStructuredBuffer<int> grid = ResourceDescriptorHeap[P[1].x];
    StructuredBuffer<uint> offsets = ResourceDescriptorHeap[P[0].w];
    uint b = gid.x;
    uint begin = offsets[b], end = (b + 1 < 4096) ? offsets[b + 1] : P[1].z;
    if (begin == end) return;
    for (uint k = t; k < 4000; k += 256) g_lds[k] = 0;
    GroupMemoryBarrierWithGroupSync();
    uint3 bo = uint3(b % BDIM, (b / BDIM) % BDIM, b / (BDIM * BDIM)) * 8u;
    for (uint i = begin + t; i < end; i += 256)
    {
        Particle p = parts[i];
        float3 pos = p.pm.xyz; float m = p.pm.w;
        int3 base = (int3)floor(pos - 0.5); float3 f = pos - (float3)base;
        float3 wx, wy, wz; weights(f.x, wx); weights(f.y, wy); weights(f.z, wz);
        float3 c2 = float3(p.c0.w, p.c1.w, 0.0);
        int3 lb = base - (int3)bo + 1; // local with halo of 1
        [unroll] for (int z = 0; z < 3; ++z) [unroll] for (int y = 0; y < 3; ++y) [unroll] for (int x = 0; x < 3; ++x)
        {
            float w = wx[x] * wy[y] * wz[z];
            float3 dpos = float3(x, y, z) - f;
            float3 mom = m * w * (p.v.xyz + float3(dot(p.c0.xyz, dpos), dot(p.c1.xyz, dpos), dot(c2, dpos)));
            int3 l = lb + int3(x, y, z);
            uint n = ((uint)l.z * 10u + (uint)l.y) * 10u + (uint)l.x;
            n = min(n, 999u) * 4u;
            InterlockedAdd(g_lds[n], (int)(m * w * 65536.0));
            InterlockedAdd(g_lds[n + 1], (int)(mom.x * 65536.0));
            InterlockedAdd(g_lds[n + 2], (int)(mom.y * 65536.0));
            InterlockedAdd(g_lds[n + 3], (int)(mom.z * 65536.0));
        }
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint k2 = t; k2 < 1000; k2 += 256)
    {
        int4 v = int4(g_lds[k2 * 4], g_lds[k2 * 4 + 1], g_lds[k2 * 4 + 2], g_lds[k2 * 4 + 3]);
        if (v.x == 0) continue;
        uint3 l = uint3(k2 % 10, (k2 / 10) % 10, k2 / 100);
        uint3 c = bo + l - 1u;
        uint n = nodeIndex(min(c, GDIM - 1)) * 4u;
        InterlockedAdd(grid[n], v.x); InterlockedAdd(grid[n + 1], v.y); InterlockedAdd(grid[n + 2], v.z); InterlockedAdd(grid[n + 3], v.w);
    }
}

[numthreads(256, 1, 1)]
void MpmGridCS(uint i : SV_DispatchThreadID)
{
    if (i >= GDIM * GDIM * GDIM) return;
    StructuredBuffer<int4> grid = ResourceDescriptorHeap[P[1].x];
    RWStructuredBuffer<float4> gv = ResourceDescriptorHeap[P[1].y];
    int4 g = grid[i];
    if (g.x <= 0) { gv[i] = 0; return; }
    float m = g.x / 65536.0;
    float3 v = float3(g.y, g.z, g.w) / (65536.0 * m);
    v.y -= 9.8 * asfloat(P[2].z);
    uint3 c = uint3(i % GDIM, (i / GDIM) % GDIM, i / (GDIM * GDIM));
    if (c.x < 2 || c.x > GDIM - 3) v.x = 0; if (c.y < 2 || c.y > GDIM - 3) v.y = 0; if (c.z < 2 || c.z > GDIM - 3) v.z = 0;
    gv[i] = float4(v, m);
}

[numthreads(256, 1, 1)]
void MpmG2PCS(uint i : SV_DispatchThreadID)
{
    if (i >= P[1].z) return;
    StructuredBuffer<Particle> parts = ResourceDescriptorHeap[P[0].y];
    RWStructuredBuffer<Particle> outp = ResourceDescriptorHeap[P[0].x];
    StructuredBuffer<float4> gv = ResourceDescriptorHeap[P[1].y];
    Particle p = parts[i];
    float3 pos = p.pm.xyz;
    int3 base = (int3)floor(pos - 0.5); float3 f = pos - (float3)base;
    float3 wx, wy, wz; weights(f.x, wx); weights(f.y, wy); weights(f.z, wz);
    float3 v = 0; float3x3 C = 0;
    [unroll] for (int z = 0; z < 3; ++z) [unroll] for (int y = 0; y < 3; ++y) [unroll] for (int x = 0; x < 3; ++x)
    {
        float w = wx[x] * wy[y] * wz[z];
        float3 dpos = float3(x, y, z) - f;
        float3 gvel = gv[nodeIndex((uint3)(base + int3(x, y, z)))].xyz;
        v += w * gvel;
        C += 4.0 * w * float3x3(gvel * dpos.x, gvel * dpos.y, gvel * dpos.z);
    }
    float dt = asfloat(P[2].z);
    p.v.xyz = v;
    p.pm.xyz = clamp(pos + v * dt, 1.5, (float)(GDIM - 3));
    p.c0 = float4(C[0], C[2].x); p.c1 = float4(C[1], C[2].y);
    outp[i] = p;
}
