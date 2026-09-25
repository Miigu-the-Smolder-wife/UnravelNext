// unx-kernel: cs_6_6 main
// unx-variants: STEP=0,1,2,3,4,5,6,7
// Ribbon strips in parallel (replaces the per-emitter walk of the old VfxRibbonShader, same geometry rules). The
// points of all ribbon emitters are one array (ranges [base, base + count) in birth order, written by the integrate
// kernel). A point is "used" when the next point of its range has another position (a run of coincident points is
// represented by its last point); pv = the previous used point = run start - 1, its position is the previous run's.
// For a used point c: before = pv exists and |c - pv| <= break, after = a next point exists and |next - c| <= break.
// Strips: a start is (after and not before); a continuing point has before (its pv then has after, so it is in the same
// strip). The side frame is parallel-transported: side_c = R(t_pv -> t_c) side_pv, so along a strip it is the product
// of the minimal rotations since the start applied to initial(t_start, normal) — an associative quaternion product,
// computed with arc length by a segmented scan. The per-point projection and normalisation of the old transport run
// at the end. Links: a continuing point links its pv; others are strip starts or isolated (NONE).
//   STEP=0 thread per point: run-start keys (k at a range start or a position change, else 0) -> scanA
//   STEP=1/2/3 max-scan of scanA (block, block totals, apply) -> run start of every point
//   STEP=4 thread per point: tangent, before/after, element (quaternion, arc length, strip start index) -> scanB
//   STEP=5/6/7 segmented scan of scanB (block, block totals, apply); STEP=7 also writes vertices and links
// P[0] = (points, links, vertices, ranges SRV), P[1] = (point count, range count, scanA, scanB),
// P[2] = (runStart, tangents, block totals A, block totals B)
#include "Passes/FX/Particles.hlsli"

struct RibbonVertex { float3 position; float3 normal; float2 uv; };
struct RibbonRange { uint base, count, program, pad; };
struct FrameElement { float4 q; float distance; uint start; uint flag; uint pad; };  // 32 B

#define N P[1].x
float3 pointAt(uint k) { FX_RWBUFFER(RibbonPoint, points, P[0].x); return points[k].position; }
void rangeOf(uint k, out uint first, out uint end, out uint program)
{
    FX_BUFFER(RibbonRange, ranges, P[0].w);
    uint lo = 0u, hi = P[1].y;
    while (hi - lo > 1u) { const uint mid = (lo + hi) >> 1; if (ranges[mid].base <= k) lo = mid; else hi = mid; }
    const RibbonRange r = ranges[lo];
    first = r.base; end = r.base + r.count; program = r.program;
}
float4 qmul(float4 a, float4 b) { return float4(a.w * b.xyz + b.w * a.xyz + cross(a.xyz, b.xyz), a.w * b.w - dot(a.xyz, b.xyz)); }
float3 qrotate(float4 q, float3 v) { const float3 t = 2.0f * cross(q.xyz, v); return v + q.w * t + cross(q.xyz, t); }
float3 unitOr(float3 v, float3 fallback) { const float l = length(v); return l > 1e-12f ? v / l : fallback; }
float3 initialSide(float3 tangent, float3 normal)
{
    float3 side = cross(tangent, normal);
    if (length(side) < 1e-6f)
    {
        const float3 a = abs(tangent);
        const float3 alt = a.x <= a.y && a.x <= a.z ? float3(1, 0, 0) : (a.y <= a.z ? float3(0, 1, 0) : float3(0, 0, 1));
        side = cross(tangent, alt);
    }
    return unitOr(side, float3(1, 0, 0));
}
FrameElement combine(FrameElement a, FrameElement b)  // a earlier, b later
{
    if (b.flag != 0u) return b;
    FrameElement r;
    r.q = normalize(qmul(b.q, a.q));
    r.distance = a.distance + b.distance;
    r.start = a.start;
    r.flag = a.flag;
    r.pad = 0u;
    return r;
}
FrameElement identityElement() { FrameElement e; e.q = float4(0, 0, 0, 1); e.distance = 0; e.start = 0u; e.flag = 0u; e.pad = 0u; return e; }

// Geometry of a used point c (its run is complete): pv, tangent, before/after, segment length.
struct PointGeometry { bool used, before, after; uint pv; float3 tangent; float segment; };
PointGeometry geometry(uint c, uint first, uint end, float limit)
{
    FX_RWBUFFER(uint, runStart, P[2].x);
    PointGeometry g;
    const float3 p = pointAt(c);
    g.used = c + 1u >= end || any(pointAt(c + 1u) != p);
    const uint rs = runStart[c];
    g.pv = rs > first ? rs - 1u : FX_NONE;
    g.before = g.pv != FX_NONE;
    g.after = c + 1u < end;
    float3 incoming = float3(0, 0, 0), outgoing = float3(0, 0, 0);
    g.segment = 0;
    if (g.before) { incoming = p - pointAt(g.pv); g.segment = length(incoming); if (g.segment > limit) g.before = false; else incoming /= g.segment; }
    if (g.after) { outgoing = pointAt(c + 1u) - p; const float span = length(outgoing); if (span > limit) g.after = false; else outgoing /= span; }
    g.tangent = g.before ? (g.after ? unitOr(incoming + outgoing, outgoing) : incoming) : outgoing;
    return g;
}

groupshared uint gs_max[1024];
groupshared FrameElement gs_elem[1024];

[numthreads(1024, 1, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 gtid : SV_GroupThreadID, uint3 gid : SV_GroupID)
{
    const uint k = id.x, t = gtid.x;
    FX_RWBUFFER(uint, scanA, P[1].z);
    FX_RWBUFFER(FrameElement, scanB, P[1].w);
    FX_RWBUFFER(uint, runStart, P[2].x);
    FX_RWBUFFER(float4, tangents, P[2].y);
    FX_RWBUFFER(uint, totalsA, P[2].z);
    FX_RWBUFFER(FrameElement, totalsB, P[2].w);
    FX_BUFFER(StreamProgram, programs, g_programs);
#if STEP == 0
    if (k >= N) return;
    uint first, end, program;
    rangeOf(k, first, end, program);
    scanA[k] = (k == first || any(pointAt(k) != pointAt(k - 1u))) ? k : 0u;
#elif STEP == 1 || STEP == 3
    // block max-scan (STEP 1 writes block totals; STEP 3 adds the carry of earlier blocks and writes run starts)
    uint v = k < N ? scanA[k] : 0u;
#if STEP == 3
    if (gid.x > 0u) v = max(v, totalsA[gid.x - 1u]);
    if (k < N) runStart[k] = v;
#else
    gs_max[t] = v;
    GroupMemoryBarrierWithGroupSync();
    for (uint s = 1u; s < 1024u; s <<= 1)
    {
        const uint o = t >= s ? gs_max[t - s] : 0u;
        GroupMemoryBarrierWithGroupSync();
        gs_max[t] = max(gs_max[t], o);
        GroupMemoryBarrierWithGroupSync();
    }
    if (k < N) scanA[k] = gs_max[t];
    if (t == 1023u) totalsA[gid.x] = gs_max[t];
#endif
#elif STEP == 2
    // inclusive max over the block totals (one group; <= 1024 blocks = 1M points)
    const uint blocks = (N + 1023u) / 1024u;
    gs_max[t] = t < blocks ? totalsA[t] : 0u;
    GroupMemoryBarrierWithGroupSync();
    for (uint s = 1u; s < 1024u; s <<= 1)
    {
        const uint o = t >= s ? gs_max[t - s] : 0u;
        GroupMemoryBarrierWithGroupSync();
        gs_max[t] = max(gs_max[t], o);
        GroupMemoryBarrierWithGroupSync();
    }
    if (t < blocks) totalsA[t] = gs_max[t];
#elif STEP == 4
    if (k >= N) return;
    uint first, end, program;
    rangeOf(k, first, end, program);
    const StreamProgram pr = programs[program];
    const PointGeometry g = geometry(k, first, end, pr.ribbonBreak);
    FrameElement e = identityElement();
    const bool strip = g.used && (g.before || g.after);
    if (strip && !g.before) { e.flag = 1u; e.start = k; }
    else if (strip)
    {
        const PointGeometry pg = geometry(g.pv, first, end, pr.ribbonBreak);
        const float c = dot(pg.tangent, g.tangent);
        if (c > -1.0f + 1e-6f) e.q = normalize(float4(cross(pg.tangent, g.tangent), 1.0f + c));
        e.distance = g.segment;
    }
    tangents[k] = float4(g.tangent, strip ? 1.0f : 0.0f);
    scanB[k] = e;
#elif STEP == 5 || STEP == 7
    FrameElement e = identityElement();
    if (k < N) e = scanB[k];
#if STEP == 5
    gs_elem[t] = e;
    GroupMemoryBarrierWithGroupSync();
    for (uint s = 1u; s < 1024u; s <<= 1)
    {
        FrameElement o = identityElement();
        if (t >= s) o = gs_elem[t - s];
        GroupMemoryBarrierWithGroupSync();
        if (t >= s) gs_elem[t] = combine(o, gs_elem[t]);
        GroupMemoryBarrierWithGroupSync();
    }
    if (k < N) scanB[k] = gs_elem[t];
    if (t == 1023u) totalsB[gid.x] = gs_elem[t];
#else
    if (k >= N) return;
    if (gid.x > 0u) e = combine(totalsB[gid.x - 1u], e);
    FX_RWBUFFER(RibbonVertex, vertices, P[0].z);
    FX_RWBUFFER(uint, links, P[0].y);
    FX_RWBUFFER(RibbonPoint, points, P[0].x);
    uint first, end, program;
    rangeOf(k, first, end, program);
    const StreamProgram pr = programs[program];
    const RibbonPoint pt = points[k];
    const float3 normal = pr.ribbonNormal;
    RibbonVertex v;
    v.position = pt.position; v.normal = normal; v.uv = float2(0, 0);
    vertices[k * 2u] = v;
    v.uv.y = 1;
    vertices[k * 2u + 1u] = v;
    links[k] = FX_NONE;
    const float4 tg = tangents[k];
    if (tg.w == 0) return;  // unused (coincident) or isolated point: default vertices, no link
    const float3 tangent = tg.xyz;
    const float3 startTangent = tangents[e.start].xyz;
    float3 side = qrotate(e.q, initialSide(startTangent, normal));
    side -= tangent * dot(side, tangent);
    const PointGeometry g = geometry(k, first, end, pr.ribbonBreak);
    side = length(side) > 1e-6f ? normalize(side) : initialSide(tangent, g.before ? tangents[g.pv].xyz : normal);
    if (g.before) links[k] = g.pv;
    const float3 n = unitOr(cross(side, tangent), normal);
    const float halfWidth = 0.5f * pt.width, uvScale = pr.ribbonUv > 0 ? pr.ribbonUv : 1.0f;
    v.position = pt.position - side * halfWidth; v.normal = n; v.uv = float2(e.distance / uvScale, 0);
    vertices[k * 2u] = v;
    v.position = pt.position + side * halfWidth; v.uv.y = 1;
    vertices[k * 2u + 1u] = v;
#endif
#elif STEP == 6
    // inclusive segmented scan over the block totals (one group; <= 1024 blocks)
    const uint blocks = (N + 1023u) / 1024u;
    FrameElement b0 = identityElement();
    if (t < blocks) b0 = totalsB[t];
    gs_elem[t] = b0;
    GroupMemoryBarrierWithGroupSync();
    for (uint s = 1u; s < 1024u; s <<= 1)
    {
        FrameElement o = identityElement();
        if (t >= s) o = gs_elem[t - s];
        GroupMemoryBarrierWithGroupSync();
        if (t >= s) gs_elem[t] = combine(o, gs_elem[t]);
        GroupMemoryBarrierWithGroupSync();
    }
    if (t < blocks) totalsB[t] = gs_elem[t];
#endif
}
