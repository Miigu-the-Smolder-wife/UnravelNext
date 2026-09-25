// Bench 1: band B fragment pipeline (COVERAGE_REDESIGN_KO.md 4.1, 4.5, 7.3).
// Slivers (width w px x length L px) or cards (w x w) are generated in the mesh shader (32 per group), rasterised
// conservatively; the pixel shader computes the exact triangle-pixel area (Sutherland-Hodgman clip, shoelace), the
// 32-subsample mask, and appends a 24 B record:
//   { uint pixel (y << 16 | x); float depth; uint mask; uint area16_flags16; uint normalOct; uint attr (uv 2 x 16) }
// APPEND_MODE 0: one global list, one atomic per wave (the P0a microbench floor, 24 B instead of 16).
// APPEND_MODE 1: per 8x8 tile segments (offset/capacity from a previous count pass), one atomic per fragment.
// APPEND_MODE 2: per tile segments, one atomic per (wave, distinct tile): lanes are grouped by tile with a bounded loop.
// COUNT=1: the count pass (tile counters only, no record).
// The composite kernel (CompositeCS) sorts a tile's records in groupshared by (pixel, depth) and composites front to
// back with the mask union, sampling two bilinear texture taps per fragment (material colour, K radiance stand-ins).
// P[0] = { width (float), height (float), sliver width px (float), sliver length px (float) }
// P[1] = { records UAV, counters UAV (0 total, 1 overflow), tile counts UAV, tile offsets SRV/UAV }
// P[2] = { tile capacities, seed, groups x, tiles x }
// P[3] = { record capacity, tile cap for composite (<= 1024), texture SRV, output UAV }
#include "common.hlsli"

#ifndef APPEND_MODE
#define APPEND_MODE 0
#endif
#ifndef COUNT
#define COUNT 0
#endif
#ifndef CARDS
#define CARDS 0
#endif

struct V { float4 pos : SV_Position; };
struct Prim { nointerpolation float2 a : PA; nointerpolation float2 b : PB; nointerpolation float2 c : PC; nointerpolation uint id : ID; nointerpolation uint normal : NRM; };
struct Frag { uint pixel; float depth; uint mask; uint areaFlags; uint normal; uint attr; };

[numthreads(128, 1, 1)]
[outputtopology("triangle")]
void SliverMS(uint gtid : SV_GroupThreadID, uint3 gid3 : SV_GroupID,
              out vertices V verts[128], out indices uint3 tris[64], out primitives Prim prims[64])
{
    SetMeshOutputCounts(128, 64);
    const uint gid = gid3.x + gid3.y * P[2].z;
    const float W = asfloat(P[0].x), H = asfloat(P[0].y), w = asfloat(P[0].z), L = asfloat(P[0].w);
    // 32 slivers per meshlet, 4 vertices + 2 triangles each; sliver s = gtid / 4 for vertices.
    const uint s = gtid >> 2;
    uint seed = pcg(gid * 32u + s + P[2].y);
    float2 o = float2(u01(seed) * W, u01(seed) * H);
    const float ang = u01(seed) * 6.2831853;
    const float2 d = float2(cos(ang), sin(ang)), n = float2(-d.y, d.x);
    const float z = 0.1 + 0.8 * u01(seed);
#if CARDS
    const float2 q[4] = { o, o + float2(w, 0), o + float2(w, w), o + float2(0, w) };
#else
    const float2 q[4] = { o - n * (w * 0.5), o + n * (w * 0.5), o + d * L + n * (w * 0.5), o + d * L - n * (w * 0.5) };
#endif
    {
        const float2 p = q[gtid & 3];
        verts[gtid].pos = float4(p.x / W * 2 - 1, 1 - p.y / H * 2, z, 1);
    }
    if (gtid < 64)
    {
        const uint sp = gtid >> 1;
        uint seed2 = pcg(gid * 32u + sp + P[2].y);
        float2 o2 = float2(u01(seed2) * W, u01(seed2) * H);
        const float ang2 = u01(seed2) * 6.2831853;
        const float2 d2 = float2(cos(ang2), sin(ang2)), n2 = float2(-d2.y, d2.x);
#if CARDS
        const float2 r[4] = { o2, o2 + float2(w, 0), o2 + float2(w, w), o2 + float2(0, w) };
#else
        const float2 r[4] = { o2 - n2 * (w * 0.5), o2 + n2 * (w * 0.5), o2 + d2 * L + n2 * (w * 0.5), o2 + d2 * L - n2 * (w * 0.5) };
#endif
        const uint v0 = sp * 4;
        const uint3 t = (gtid & 1) ? uint3(v0, v0 + 2, v0 + 3) : uint3(v0, v0 + 1, v0 + 2);
        tris[gtid] = t;
        prims[gtid].a = r[t.x & 3]; prims[gtid].b = r[t.y & 3]; prims[gtid].c = r[t.z & 3];
        prims[gtid].id = (gid << 6) | gtid;
        prims[gtid].normal = octEncode(normalize(float3(n2.x, 0.6, n2.y)));
    }
}

#define CLIP_MAX 7u
void clipPoly(inout float2 p[CLIP_MAX], inout uint n, uint axis, float limit, float sign)
{
    float2 o[CLIP_MAX];
    uint m = 0;
    [unroll] for (uint i = 0; i < CLIP_MAX; ++i)
    {
        if (i >= n) break;
        const float2 a = p[i], b = p[(i + 1) % n];
        const float da = (a[axis] - limit) * sign, db = (b[axis] - limit) * sign;
        if (da <= 0 && m < CLIP_MAX) o[m++] = a;
        if ((da < 0 && db > 0) || (da > 0 && db < 0))
        {
            const float t = da / (da - db);
            if (m < CLIP_MAX) o[m++] = lerp(a, b, t);
        }
    }
    n = m;
    [unroll] for (uint k = 0; k < CLIP_MAX; ++k) p[k] = o[k];
}
float triangleArea(float2 a, float2 b, float2 c, float2 pixel)
{
    float2 p[CLIP_MAX];
    p[0] = a - pixel; p[1] = b - pixel; p[2] = c - pixel;
    [unroll] for (uint k = 3; k < CLIP_MAX; ++k) p[k] = 0;
    uint n = 3;
    clipPoly(p, n, 0, 0.0, -1.0);
    clipPoly(p, n, 0, 1.0, 1.0);
    clipPoly(p, n, 1, 0.0, -1.0);
    clipPoly(p, n, 1, 1.0, 1.0);
    float twice = 0;
    [unroll] for (uint i = 0; i < CLIP_MAX; ++i)
    {
        if (i >= n) break;
        const float2 u = p[i], v = p[(i + 1) % n];
        twice += u.x * v.y - v.x * u.y;
    }
    return 0.5 * abs(twice);
}
#ifndef UNION
#define UNION 0  // revision 1 4.6 opaqueCovered union rule: 1 = per-pixel U |= mask, D = min depth with the early skip when U is full; 2 = no skip. P[5] = { U UAV (raw), D UAV (raw) }
#endif
#ifndef AREA
#define AREA 0
#endif
#ifndef MASK
#define MASK 1
#endif
// AREA=1: signed area of the polygon clipped to the unit square as the sum of per-edge contributions, each edge clipped
// against the four half-planes independently (Green's theorem on the clipped edge): no vertex arrays, register resident.
float clipSegmentArea(float2 p, float2 q)
{
    const float dy = q.y - p.y;
    if (abs(dy) < 1e-7) { const float y = clamp(p.y, 0.0, 1.0); return 0.5 * (p.x * y - q.x * y); }
    const float t0 = (0.0 - p.y) / dy, t1 = (1.0 - p.y) / dy;
    const float ta = clamp(min(t0, t1), 0.0, 1.0), tb = clamp(max(t0, t1), 0.0, 1.0);
    float2 a = lerp(p, q, ta), b = lerp(p, q, tb);
    a.y = clamp(a.y, 0.0, 1.0); b.y = clamp(b.y, 0.0, 1.0);
    const float2 pa = float2(p.x, clamp(p.y, 0.0, 1.0)), qb = float2(q.x, clamp(q.y, 0.0, 1.0));
    return 0.5 * (pa.x * a.y - a.x * pa.y) + 0.5 * (a.x * b.y - b.x * a.y) + 0.5 * (b.x * qb.y - qb.x * b.y);
}
float clipXThenY(float2 p, float2 q)
{
    const float dx = q.x - p.x;
    if (abs(dx) < 1e-7) { const float x = clamp(p.x, 0.0, 1.0); return clipSegmentArea(float2(x, p.y), float2(x, q.y)); }
    const float t0 = (0.0 - p.x) / dx, t1 = (1.0 - p.x) / dx;
    const float ta = clamp(min(t0, t1), 0.0, 1.0), tb = clamp(max(t0, t1), 0.0, 1.0);
    float2 a = lerp(p, q, ta), b = lerp(p, q, tb);
    a.x = clamp(a.x, 0.0, 1.0); b.x = clamp(b.x, 0.0, 1.0);
    const float2 pa = float2(clamp(p.x, 0.0, 1.0), p.y), qb = float2(clamp(q.x, 0.0, 1.0), q.y);
    return clipSegmentArea(pa, a) + clipSegmentArea(a, b) + clipSegmentArea(b, qb);
}
float triangleAreaGreen(float2 a, float2 b, float2 c, float2 pixel)
{
    a -= pixel; b -= pixel; c -= pixel;
    return abs(clipXThenY(a, b) + clipXThenY(b, c) + clipXThenY(c, a));
}
float2 coverageSample(uint i) { return float2((i + 0.5) / 32.0, (reversebits(i) >> 27) / 32.0 + 1.0 / 64.0); }
uint triangleMask(float2 a, float2 b, float2 c, float2 pixel)
{
    const float orient = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
    const float s = orient >= 0 ? 1.0 : -1.0;
    uint mask = 0;
    [unroll] for (uint i = 0; i < 32; ++i)
    {
        const float2 q = pixel + coverageSample(i);
        const float e0 = ((b.x - a.x) * (q.y - a.y) - (b.y - a.y) * (q.x - a.x)) * s;
        const float e1 = ((c.x - b.x) * (q.y - b.y) - (c.y - b.y) * (q.x - b.x)) * s;
        const float e2 = ((a.x - c.x) * (q.y - c.y) - (a.y - c.y) * (q.x - c.x)) * s;
        if (e0 >= 0 && e1 >= 0 && e2 >= 0) mask |= 1u << i;
    }
    return mask;
}

void FragPS(V v, Prim p)
{
    const float2 p0 = floor(v.pos.xy);
#if AREA
    const float area = triangleAreaGreen(p.a, p.b, p.c, p0);
#else
    const float area = triangleArea(p.a, p.b, p.c, p0);
#endif
#ifdef NO_HELPER
    const bool live = area > 0;
#else
    const bool live = area > 0 && !IsHelperLane();
#endif
    const uint tilesX = P[2].w;
    const uint tile = (uint)(p0.y * 0.125) * tilesX + (uint)(p0.x * 0.125);
#if COUNT
    RWStructuredBuffer<uint> tileCount = ResourceDescriptorHeap[P[1].z];
    if (live) InterlockedAdd(tileCount[tile], 1u);
    return;
#else
    RWStructuredBuffer<Frag> frags = ResourceDescriptorHeap[P[1].x];
    RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[P[1].y];
    uint slot = 0xFFFFFFFFu;
#if APPEND_MODE == 0
    {
        const uint lane = WavePrefixCountBits(live), waveCount = WaveActiveCountBits(live);
        uint base = 0;
        if (WaveIsFirstLane() && waveCount) InterlockedAdd(counters[2], waveCount, base);  // allocation counter (counters[0] counts records written)
        base = WaveReadLaneFirst(base);
        if (live) slot = base + lane;
        if (slot >= P[3].x) { slot = 0xFFFFFFFFu; if (live) InterlockedAdd(counters[1], 1u); }
    }
#else
    RWStructuredBuffer<uint> tileCount = ResourceDescriptorHeap[P[1].z];
    StructuredBuffer<uint> tileOffset = ResourceDescriptorHeap[P[1].w];
    StructuredBuffer<uint> tileCap = ResourceDescriptorHeap[P[2].x];
    uint local = 0;
#if APPEND_MODE == 1
    if (live) InterlockedAdd(tileCount[tile], 1u, local);
#else
    // One atomic per (wave, distinct tile): the first not-done lane names a tile, every lane of that tile takes a rank.
    bool done = !live;
    [loop] for (uint it = 0; it < 64 && WaveActiveAnyTrue(!done); ++it)
    {
        const uint t = WaveActiveMin(done ? 0xFFFFFFFFu : tile);
        const bool me = !done && tile == t;
        const uint cnt = WaveActiveCountBits(me), rank = WavePrefixCountBits(me);
        uint base = 0;
        if (me && rank == 0) InterlockedAdd(tileCount[t], cnt, base);
        base = WaveActiveMax(me && rank == 0 ? base : 0u);
        if (me) { local = base + rank; done = true; }
    }
#endif
    if (live)
    {
        if (local < tileCap[tile]) slot = tileOffset[tile] + local;
        else InterlockedAdd(counters[1], 1u);
    }
#endif
    if (slot != 0xFFFFFFFFu)
    {
        Frag f;
        f.pixel = ((uint)p0.y << 16) | (uint)p0.x;
        f.depth = v.pos.z;
#if MASK
        f.mask = triangleMask(p.a, p.b, p.c, p0);
#else
        f.mask = 0xFFFFFFFFu;
#endif
        f.areaFlags = (uint)round(saturate(area) * 65535.0) | (1u << 16);
        f.normal = p.normal;
        const float2 uv = frac(p0 * float2(0.013, 0.017) + p.id * 1e-4);
        f.attr = (uint)(uv.x * 65535.0) | ((uint)(uv.y * 65535.0) << 16);
        frags[slot] = f;
        InterlockedAdd(counters[0], 1u);
#if UNION
        RWByteAddressBuffer uU = ResourceDescriptorHeap[P[5].x];
        RWByteAddressBuffer uD = ResourceDescriptorHeap[P[5].y];
        const uint pix = ((uint)p0.y * (uint)asfloat(P[0].x) + (uint)p0.x) * 4u;
#if UNION == 1
        if (uU.Load(pix) != 0xFFFFFFFFu)
#endif
        {
            uD.InterlockedMin(pix, asuint(v.pos.z));
            uU.InterlockedOr(pix, f.mask);
        }
#endif
    }
#endif
}

// Tile offsets from the counts: offset = exclusive prefix of capacity, capacity = count * 5 / 4 + 8 (design: previous
// frame x 1.5; the bench uses 1.25 to fit 20 M records in 640 MB). Single group, tiles <= 1024 * 128. P[0].x = tiles.
// P[1] = { tile counts SRV, tile offsets UAV, tile capacities UAV, total UAV }
groupshared uint gsSum[1024];
[numthreads(1024, 1, 1)]
void PrefixCS(uint tid : SV_GroupThreadID)
{
    StructuredBuffer<uint> counts = ResourceDescriptorHeap[P[1].x];
    RWStructuredBuffer<uint> offsets = ResourceDescriptorHeap[P[1].y];
    RWStructuredBuffer<uint> caps = ResourceDescriptorHeap[P[1].z];
    RWStructuredBuffer<uint> total = ResourceDescriptorHeap[P[1].w];
    const uint tiles = P[0].x, per = (tiles + 1023) / 1024;
    uint sum = 0;
    [loop] for (uint i = 0; i < 128 && i < per; ++i)
    {
        const uint t = tid * per + i;
        if (t < tiles) sum += counts[t] * 5 / 4 + 8;
    }
    gsSum[tid] = sum;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint s = 1; s < 1024; s <<= 1)
    {
        const uint v = tid >= s ? gsSum[tid - s] : 0;
        GroupMemoryBarrierWithGroupSync();
        gsSum[tid] += v;
        GroupMemoryBarrierWithGroupSync();
    }
    uint run = tid > 0 ? gsSum[tid - 1] : 0;
    [loop] for (uint i2 = 0; i2 < 128 && i2 < per; ++i2)
    {
        const uint t = tid * per + i2;
        if (t < tiles)
        {
            const uint c = counts[t] * 5 / 4 + 8;
            offsets[t] = run;
            caps[t] = c;
            run += c;
        }
    }
    if (tid == 1023) total[0] = gsSum[1023];
}

// Composite: one group per 8x8 tile. Loads min(count, cap) records into groupshared, sorts by (pixel, depth) with a
// bitonic network, then each lane (pixel) walks its records front to back: coverage by mask union (as EdgeComposite),
// two bilinear taps per fragment (material colour, K radiance), constant lighting, RGBA8 out.
// P[1] = { records SRV, tile counts SRV, tile offsets SRV, unused }, P[2] = { tile capacities SRV, -, -, tiles x }
// P[3] = { -, tile cap (<= 1024), texture SRV, output UAV }, P[4] = { mode: 0 = tile segments, 1 = global list (records of
// tile found by full scan of a bounded window: not used), -, -, - }
#define TILE_CAP 1024u
groupshared uint2 gsKey[TILE_CAP];  // x = sort key (pixel << 26 | depth bits >> 6), y = record index
groupshared uint gsCount;
[numthreads(64, 1, 1)]
void CompositeCS(uint3 gid : SV_GroupID, uint tid : SV_GroupThreadID)
{
    StructuredBuffer<Frag> frags = ResourceDescriptorHeap[P[1].x];
    StructuredBuffer<uint> tileCount = ResourceDescriptorHeap[P[1].y];
    StructuredBuffer<uint> tileOffset = ResourceDescriptorHeap[P[1].z];
    StructuredBuffer<uint> tileCap = ResourceDescriptorHeap[P[2].x];
    Texture2D<float4> tex = ResourceDescriptorHeap[P[3].z];
    RWTexture2D<float4> output = ResourceDescriptorHeap[P[3].w];
    const uint tile = gid.y * P[2].w + gid.x;
    const uint cap = min(P[3].y, TILE_CAP);
    const uint n = min(min(tileCount[tile], tileCap[tile]), cap);
    const uint base = tileOffset[tile];
    // Padded to the next power of two for the bitonic network (keys of empty slots sort last).
    uint size = 1;
    [unroll] for (uint b = 0; b < 10; ++b) if (size < n) size <<= 1;
    for (uint i = tid; i < size; i += 64)
    {
        if (i < n)
        {
            const Frag f = frags[base + i];
            const uint local = ((f.pixel >> 16) & 7u) * 8 + (f.pixel & 7u);
            gsKey[i] = uint2((local << 26) | (asuint(f.depth) >> 6), i);
        }
        else gsKey[i] = uint2(0xFFFFFFFFu, 0xFFFFFFFFu);
    }
    GroupMemoryBarrierWithGroupSync();
    // Bitonic sort (size <= 1024): 55 compare-exchange passes at most, 8 pairs per lane per pass.
    [loop] for (uint k = 2; k <= size; k <<= 1)
        [loop] for (uint j = k >> 1; j > 0; j >>= 1)
        {
            [loop] for (uint pIdx = tid; pIdx < size / 2; pIdx += 64)
            {
                const uint i0 = 2 * pIdx - (pIdx & (j - 1));
                const uint i1 = i0 + j;
                const bool up = (i0 & k) == 0;
                const uint2 a = gsKey[i0], c = gsKey[i1];
                if ((a.x > c.x) == up) { gsKey[i0] = c; gsKey[i1] = a; }
            }
            GroupMemoryBarrierWithGroupSync();
        }
    // Lane = pixel of the tile: lower bound of its key range by binary search.
    const uint local = tid;
    uint lo = 0, hi = n;
    [unroll] for (uint step = 0; step < 11; ++step)
    {
        if (lo >= hi) break;
        const uint mid = (lo + hi) >> 1;
        if ((gsKey[mid].x >> 26) < local) lo = mid + 1; else hi = mid;
    }
    const uint2 pixel = uint2(gid.x * 8 + (local & 7u), gid.y * 8 + (local >> 3));
    uint covered = 0;
    float used = 0;
    float3 sum = 0;
    [loop] for (uint f = lo; f < n && f < lo + 256; ++f)
    {
        const uint2 key = gsKey[f];
        if ((key.x >> 26) != local) break;
        const Frag r = frags[base + key.y];
        const float area = (r.areaFlags & 0xFFFFu) / 65535.0;
        const uint m = r.mask;
        const float seen = countbits(m) > 0 ? countbits(m & ~covered) / (float)countbits(m) : 1.0;
        const float w = min(area * seen, max(1 - used, 0.0));
        covered |= m;
        used += w;
        const float3 nrm = octDecode(r.normal);
        const float2 uv = float2(r.attr & 0xFFFFu, r.attr >> 16) / 65535.0;
        const float3 albedo = tex.SampleLevel(g_linear, uv, 0).rgb;
        const float3 radianceK = tex.SampleLevel(g_linear, uv * 0.37 + nrm.xy * 0.1, 2).rgb;
        const float ndl = saturate(dot(nrm, normalize(float3(0.3, 0.8, 0.5)))) + 0.25 * saturate(dot(nrm, normalize(float3(-0.5, 0.2, 0.7))));
        sum += w * (albedo * ndl + radianceK * 0.04);
        if (used >= 1.0) break;
    }
    if (all(pixel < uint2(asfloat(P[0].x), asfloat(P[0].y)))) output[pixel] = float4(sum + (1 - used) * float3(0.1, 0.12, 0.15), 1);
}
