// Sub-pixel coverage layer floor: thin slivers rasterized conservatively; the pixel shader computes the exact
// triangle-pixel overlap area (Sutherland-Hodgman clip) and appends a fragment record. No render target.
// P[0].x sliver width px (float bits), P[0].y sliver length px, P[0].z W, P[0].w H
// P[1].x fragment UAV, P[1].y counter UAV, P[1].z seed, P[1].w dispatch x count, P[2].x capacity mask (pow2-1)

struct V { float4 pos : SV_Position; };
struct Prim { nointerpolation float2 a : PA; nointerpolation float2 b : PB; nointerpolation float2 c : PC; nointerpolation uint id : ID; };
struct Frag { uint pixel; uint id; float depth; float area; };

[numthreads(128, 1, 1)]
[outputtopology("triangle")]
void CovMS(uint gtid : SV_GroupThreadID, uint3 gid3 : SV_GroupID,
           out vertices V verts[128], out indices uint3 tris[64], out primitives Prim prims[64])
{
    SetMeshOutputCounts(128, 64);
    uint gid = gid3.x + gid3.y * P[1].w;
    float w = asfloat(P[0].x), L = asfloat(P[0].y), W = asfloat(P[0].z), H = asfloat(P[0].w);
    // 32 slivers per meshlet, 4 vertices + 2 triangles each
    uint s = gtid >> 2;             // sliver for vertices (gtid < 128)
    uint seed = pcg(gid * 32u + s + P[1].z);
    float2 o = float2(u01(seed) * W, u01(seed) * H);
    float ang = u01(seed) * 6.2831853;
    float2 d = float2(cos(ang), sin(ang)), n = float2(-d.y, d.x);
    float z = 0.1 + 0.8 * u01(seed);
    float2 q[4] = { o - n * (w * 0.5), o + n * (w * 0.5), o + d * L + n * (w * 0.5), o + d * L - n * (w * 0.5) };
    {
        float2 p = q[gtid & 3];
        verts[gtid].pos = float4(p.x / W * 2 - 1, 1 - p.y / H * 2, z, 1);
    }
    if (gtid < 64)
    {
        uint sp = gtid >> 1;
        uint seed2 = pcg(gid * 32u + sp + P[1].z);
        float2 o2 = float2(u01(seed2) * W, u01(seed2) * H);
        float ang2 = u01(seed2) * 6.2831853;
        float2 d2 = float2(cos(ang2), sin(ang2)), n2 = float2(-d2.y, d2.x);
        float2 r[4] = { o2 - n2 * (w * 0.5), o2 + n2 * (w * 0.5), o2 + d2 * L + n2 * (w * 0.5), o2 + d2 * L - n2 * (w * 0.5) };
        uint v0 = sp * 4;
        uint3 t = (gtid & 1) ? uint3(v0, v0 + 2, v0 + 3) : uint3(v0, v0 + 1, v0 + 2);
        tris[gtid] = t;
        prims[gtid].a = r[t.x & 3]; prims[gtid].b = r[t.y & 3]; prims[gtid].c = r[t.z & 3];
        prims[gtid].id = (gid << 6) | gtid;
    }
}

// exact area of triangle (a,b,c) clipped to the unit pixel square with corner p0.
// Register-resident formulation: the signed area of a polygon clipped by a half-plane equals the area of the
// polygon with every edge clipped independently (Green's theorem on the clipped edge), so each of the 4 pixel
// edges is applied to every polygon edge without building an explicit vertex list.
float clipSegmentArea(float2 p, float2 q)
{
    // contribution of segment p->q (already clipped to x in [0,1]) after clamping y to [0,1] and integrating x dy
    // shoelace term for the segment clipped against the y band: split at y=0 and y=1 crossings
    float2 lo = p, hi = q;
    // clamp y to [0,1]: the part outside contributes as if projected onto the band edge (x stays, y clamped)
    float dy = q.y - p.y;
    if (abs(dy) < 1e-7) { float y = clamp(p.y, 0.0, 1.0); return 0.5 * (p.x * y - q.x * y); }
    float t0 = (0.0 - p.y) / dy, t1 = (1.0 - p.y) / dy;
    float ta = clamp(min(t0, t1), 0.0, 1.0), tb = clamp(max(t0, t1), 0.0, 1.0);
    float2 a = lerp(p, q, ta), b = lerp(p, q, tb);
    a.y = clamp(a.y, 0.0, 1.0); b.y = clamp(b.y, 0.0, 1.0);
    float2 pa = float2(p.x, clamp(p.y, 0.0, 1.0)), qb = float2(q.x, clamp(q.y, 0.0, 1.0));
    // three pieces: p->a (y clamped, zero contribution), a->b (inside band), b->q (y clamped)
    float area = 0.5 * (pa.x * a.y - a.x * pa.y) + 0.5 * (a.x * b.y - b.x * a.y) + 0.5 * (b.x * qb.y - qb.x * b.y);
    return area;
}
float clipXThenY(float2 p, float2 q)
{
    // clip the segment to x in [0,1]; parts outside are projected onto x=0 or x=1 (they contribute zero width)
    float dx = q.x - p.x;
    if (abs(dx) < 1e-7) { float x = clamp(p.x, 0.0, 1.0); return clipSegmentArea(float2(x, p.y), float2(x, q.y)); }
    float t0 = (0.0 - p.x) / dx, t1 = (1.0 - p.x) / dx;
    float ta = clamp(min(t0, t1), 0.0, 1.0), tb = clamp(max(t0, t1), 0.0, 1.0);
    float2 a = lerp(p, q, ta), b = lerp(p, q, tb);
    a.x = clamp(a.x, 0.0, 1.0); b.x = clamp(b.x, 0.0, 1.0);
    float2 pa = float2(clamp(p.x, 0.0, 1.0), p.y), qb = float2(clamp(q.x, 0.0, 1.0), q.y);
    return clipSegmentArea(pa, a) + clipSegmentArea(a, b) + clipSegmentArea(b, qb);
}
float clippedArea(float2 a, float2 b, float2 c, float2 p0)
{
    a -= p0; b -= p0; c -= p0;
    return abs(clipXThenY(a, b) + clipXThenY(b, c) + clipXThenY(c, a));
}

void CovPS(V v, Prim p)
{
    RWStructuredBuffer<Frag> frags = ResourceDescriptorHeap[P[1].x];
    RWStructuredBuffer<uint> counter = ResourceDescriptorHeap[P[1].y];
    float2 p0 = floor(v.pos.xy);
    float area = clippedArea(p.a, p.b, p.c, p0);
    if (area <= 0) return;
    if (P[2].y != 0) InterlockedAdd(counter[2], (uint)(area * 16.0 + 0.5)); // verification run only: total covered area (x16 fixed point)
    // wave-aggregated append: one global atomic per wave instead of one per fragment
    uint lane = WavePrefixCountBits(true);
    uint waveCount = WaveActiveCountBits(true);
    uint base = 0;
    if (WaveIsFirstLane()) InterlockedAdd(counter[0], waveCount, base);
    base = WaveReadLaneFirst(base);
    uint slot = base + lane;
    Frag f; f.pixel = (uint)p0.y * (uint)asfloat(P[0].z) + (uint)p0.x; f.id = p.id; f.depth = v.pos.z; f.area = area;
    frags[slot & P[2].x] = f;
}

// reference: same slivers, plain depth-only raster (how many pixels the hardware would have produced)
void CovCountPS(V v, Prim p)
{
    RWStructuredBuffer<uint> counter = ResourceDescriptorHeap[P[1].y];
    InterlockedAdd(counter[1], 1u);
}
