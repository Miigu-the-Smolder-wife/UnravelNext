// unx-kernel: cs_6_6 main
// Ribbon strips (replaces the per-emitter walk of the old VfxRibbonShader, same geometry rules): one group per ribbon
// range, one dispatch. The points of all ribbon emitters are one array (ranges [base, base + count) in birth order,
// written by the integrate kernel). A point is "used" when the next point of its range has another position (a run of
// coincident points is represented by its last point); pv = the previous used point = run start - 1, its position is the
// previous run's. For a used point c: before = pv exists and |c - pv| <= break, after = a next point exists and
// |next - c| <= break. Strips: a start is (after and not before); a continuing point has before (its pv then has after,
// so it is in the same strip). The side frame is parallel-transported: side_c = R(t_pv -> t_c) side_pv, so along a strip
// it is the product of the minimal rotations since the start applied to initial(t_start, normal) - an associative
// quaternion product, computed with arc length by a segmented scan. The per-point projection and normalisation of the
// old transport run at the end. Links: a continuing point links its pv; others are strip starts or isolated (NONE).
// The group walks its range in chunks of 256 points; per chunk: run starts (max-scan, carried across chunks), geometry
// and scan elements, the segmented scan (carried), vertices. The combination order is fixed (in-chunk Hillis-Steele,
// then the carry of the earlier chunks), so the result is deterministic. runStart and tangents are written to memory,
// since a point's pv or strip start may lie in an earlier chunk (visible after the device barriers of the same group).
// P[0] = (points, links, vertices, ranges SRV), P[1] = (runStart, tangents, range count, 0)
#include "Passes/FX/Particles.hlsli"

struct RibbonVertex { float3 position; float3 normal; float2 uv; };
struct RibbonRange { uint base, count, program, pad; };
struct FrameElement { float4 q; float distance; uint start; uint flag; uint pad; };  // 32 B

#define CHUNK 256u
float3 pointAt(uint k) { FX_RWBUFFER(RibbonPoint, points, P[0].x); return points[k].position; }
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
    FX_RWBUFFER(uint, runStart, P[1].x);
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

groupshared uint gs_run[CHUNK];
groupshared FrameElement gs_elem[CHUNK];

[numthreads(256, 1, 1)]
void main(uint3 gtid : SV_GroupThreadID, uint3 gid : SV_GroupID)
{
    if (gid.x >= P[1].z) return;
    const uint t = gtid.x;
    FX_BUFFER(RibbonRange, ranges, P[0].w);
    FX_BUFFER(StreamProgram, programs, g_programs);
    FX_RWBUFFER(uint, runStart, P[1].x);
    FX_RWBUFFER(float4, tangents, P[1].y);
    FX_RWBUFFER(RibbonVertex, vertices, P[0].z);
    FX_RWBUFFER(uint, links, P[0].y);
    FX_RWBUFFER(RibbonPoint, points, P[0].x);
    const RibbonRange range = ranges[gid.x];
    const uint first = range.base, end = range.base + range.count;
    const StreamProgram pr = programs[range.program];
    const float3 normal = pr.ribbonNormal;
    const float uvScale = pr.ribbonUv > 0 ? pr.ribbonUv : 1.0f;
    uint runCarry = 0u;
    FrameElement carry = identityElement();
    for (uint c0 = first; c0 < end; c0 += CHUNK)
    {
        const uint k = c0 + t;
        const bool valid = k < end;
        // run starts: key k at the range start or a position change, inclusive max-scan (carried)
        gs_run[t] = valid && (k == first || any(pointAt(k) != pointAt(k - 1u))) ? k : 0u;
        GroupMemoryBarrierWithGroupSync();
        for (uint s = 1u; s < CHUNK; s <<= 1)
        {
            const uint o = t >= s ? gs_run[t - s] : 0u;
            GroupMemoryBarrierWithGroupSync();
            gs_run[t] = max(gs_run[t], o);
            GroupMemoryBarrierWithGroupSync();
        }
        if (valid) runStart[k] = max(gs_run[t], runCarry);
        runCarry = max(runCarry, gs_run[CHUNK - 1u]);
        DeviceMemoryBarrierWithGroupSync();
        // geometry and scan element
        FrameElement e = identityElement();
        bool strip = false;
        PointGeometry g;
        g.used = false; g.before = false; g.after = false; g.pv = FX_NONE; g.tangent = float3(0, 0, 0); g.segment = 0;
        if (valid)
        {
            g = geometry(k, first, end, pr.ribbonBreak);
            strip = g.used && (g.before || g.after);
            if (strip && !g.before) { e.flag = 1u; e.start = k; }
            else if (strip)
            {
                const PointGeometry pg = geometry(g.pv, first, end, pr.ribbonBreak);
                const float c = dot(pg.tangent, g.tangent);
                if (c > -1.0f + 1e-6f) e.q = normalize(float4(cross(pg.tangent, g.tangent), 1.0f + c));
                e.distance = g.segment;
            }
            tangents[k] = float4(g.tangent, strip ? 1.0f : 0.0f);
        }
        gs_elem[t] = e;
        GroupMemoryBarrierWithGroupSync();
        for (uint s2 = 1u; s2 < CHUNK; s2 <<= 1)
        {
            FrameElement o = identityElement();
            if (t >= s2) o = gs_elem[t - s2];
            GroupMemoryBarrierWithGroupSync();
            if (t >= s2) gs_elem[t] = combine(o, gs_elem[t]);
            GroupMemoryBarrierWithGroupSync();
        }
        e = combine(carry, gs_elem[t]);
        const FrameElement chunkTotal = gs_elem[CHUNK - 1u];
        DeviceMemoryBarrierWithGroupSync();
        // vertices and links
        if (valid)
        {
            const RibbonPoint pt = points[k];
            RibbonVertex v;
            v.position = pt.position; v.normal = normal; v.uv = float2(0, 0);
            links[k] = FX_NONE;
            if (!strip)
            {
                // unused (coincident) or isolated point: default vertices, no link
                vertices[k * 2u] = v;
                v.uv.y = 1;
                vertices[k * 2u + 1u] = v;
            }
            else
            {
                const float3 startTangent = tangents[e.start].xyz;
                float3 side = qrotate(e.q, initialSide(startTangent, normal));
                side -= g.tangent * dot(side, g.tangent);
                side = length(side) > 1e-6f ? normalize(side) : initialSide(g.tangent, g.before ? tangents[g.pv].xyz : normal);
                if (g.before) links[k] = g.pv;
                const float3 n = unitOr(cross(side, g.tangent), normal);
                const float halfWidth = 0.5f * pt.width;
                v.position = pt.position - side * halfWidth; v.normal = n; v.uv = float2(e.distance / uvScale, 0);
                vertices[k * 2u] = v;
                v.position = pt.position + side * halfWidth; v.uv.y = 1;
                vertices[k * 2u + 1u] = v;
            }
        }
        carry = combine(carry, chunkTotal);
        GroupMemoryBarrierWithGroupSync();
    }
}
