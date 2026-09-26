// unx-kernel: cs_6_6 main
// Fluid surface: one group per active block (indirect, argument 1): each cell writes its triangles at its block's
// first triangle + its prefix. Vertices sit on the cube edges where the density crosses 0.5 (linear), normals are the
// negated density gradient (central differences at the corners, blended along the edge). Vertex = (world xyz, 1),
// (normal xyz, 0); velocities (world m/s, 0) in their own buffer. The grid lives in the particles' space; positions,
// normals and velocities are written in the renderer's (fsAxes: the host's World is the renderer's mirrored in z), a
// mirror swapping each triangle's last two vertices so its outside stays counter-clockwise.
#include "FluidSurface.hlsli"
#include "WaterLinear.hlsli"

groupshared float g_density[FS_WINDOW * FS_WINDOW * FS_WINDOW];
struct FsVertex { float3 p, n, v; };
// Stores one output triangle (vertices in the case table's CCW order; a mirror swaps the last two).
void fsStore(RWByteAddressBuffer vertices, RWByteAddressBuffer velocities, uint tri, FsVertex a, FsVertex b, FsVertex c, bool mirrored)
{
    FsVertex v3[3] = { a, b, c };
    [unroll] for (uint v = 0; v < 3; ++v)
    {
        const uint slot = mirrored && v != 0 ? 3 - v : v;
        vertices.Store4((tri * 3 + slot) * 32, asuint(float4(v3[v].p, 1)));
        vertices.Store4((tri * 3 + slot) * 32 + 16, asuint(float4(v3[v].n, 0)));
        velocities.Store4((tri * 3 + slot) * 16, asuint(float4(v3[v].v, 0)));
    }
}
// The waterline point on edge a -> b (fa >= 0 > fb): regula falsi (Illinois) on the vertical distance to the bath
// surface, 4 steps (eta is smooth over a cell: the linear start is within a fraction of a millimetre).
FsVertex fsWaterline(FsVertex a, FsVertex b, float fa, float fb)
{
    float lo = 0, hi = 1, flo = fa, fhi = fb, t = fa / (fa - fb);
    [unroll] for (uint k = 0; k < 4; ++k)
    {
        const float f = fsAboveWater(lerp(a.p, b.p, t));
        if (f >= 0) { lo = t; flo = f; fhi *= 0.5f; } else { hi = t; fhi = f; flo *= 0.5f; }
        t = flo - fhi != 0 ? lo + (hi - lo) * flo / (flo - fhi) : 0.5f * (lo + hi);
    }
    FsVertex r;
    r.p = lerp(a.p, b.p, t);
    const float3 n = lerp(a.n, b.n, t);
    r.n = length(n) > 0 ? normalize(n) : float3(0, 1, 0);
    r.v = lerp(a.v, b.v, t);
    return r;
}
groupshared float3 g_momentum[FS_WINDOW * FS_WINDOW * FS_WINDOW];
float fsAt(int3 n) { return g_density[(n.z * FS_WINDOW + n.y) * FS_WINDOW + n.x]; }
float3 fsGradient(int3 n)
{
    return float3(fsAt(n + int3(1, 0, 0)) - fsAt(n - int3(1, 0, 0)), fsAt(n + int3(0, 1, 0)) - fsAt(n - int3(0, 1, 0)), fsAt(n + int3(0, 0, 1)) - fsAt(n - int3(0, 0, 1)));
}
[numthreads(512, 1, 1)]
void main(uint t : SV_GroupThreadID, uint3 group : SV_GroupID)
{
    const uint g = group.y * WATER_LINEAR_ROW + group.x;  // the active block (rows of WATER_LINEAR_ROW groups)
    RWByteAddressBuffer blockCounters = ResourceDescriptorHeap[P[4].w];
    if (g >= min(blockCounters.Load(4 * FS_COUNTER_ACTIVE), fsMaxBlocks())) return;  // the last row's extra groups
    RWByteAddressBuffer scan = ResourceDescriptorHeap[P[4].y];
    RWByteAddressBuffer info = ResourceDescriptorHeap[P[5].x];
    RWByteAddressBuffer blockTris = ResourceDescriptorHeap[P[5].y];
    RWByteAddressBuffer vertices = ResourceDescriptorHeap[P[5].z];
    RWByteAddressBuffer velocities = ResourceDescriptorHeap[P[6].z];
    ByteAddressBuffer cases = ResourceDescriptorHeap[P[5].w];
    int3 block = fsBlockCoord(scan.Load(4 * (FS_SLOTS(fsTableSize()) + g)));
    int3 origin = block * 8 - 1;
    for (uint k = t; k < FS_WINDOW * FS_WINDOW * FS_WINDOW; k += FS_BLOCK_NODES)
    {
        uint4 node = fsNode(origin + int3(k % FS_WINDOW, (k / FS_WINDOW) % FS_WINDOW, k / (FS_WINDOW * FS_WINDOW)));
        g_density[k] = node.x / FS_SCALE; g_momentum[k] = (float3)(int3)node.yzw / FS_MOMENTUM;
    }
    GroupMemoryBarrierWithGroupSync();
    uint cell = info.Load(4 * (g * FS_BLOCK_NODES + t)), mask = cell & 255u;
    uint count = cases.Load(4 * mask * FS_CASE_STRIDE);
    if (count == 0) return;
    uint first = blockTris.Load(4 * (fsMaxBlocks() + g)) + (cell >> 8);
    int3 c = int3(t % 8, (t / 8) % 8, t / 64);
    float h = fsH(); float3 world0 = fsOrigin() + (float3)(block * 8 + c) * h;
    const float3 axes = fsAxes();
    const bool mirrored = fsMirrored();
    if (fsUnderBand(fsCellCentre(block * 8 + c)))
    {
        // W3 seam: wholly under a basin's water - no boundary there (one medium)
        FsVertex none; none.p = axes * world0; none.n = float3(0, 1, 0); none.v = float3(0, 0, 0);
        for (uint k4 = 0; k4 < count; ++k4)
            if (first + k4 < fsMaxTriangles()) fsStore(vertices, velocities, first + k4, none, none, none, mirrored);
        return;
    }
    if (fsInBand(fsCellCentre(block * 8 + c)))
    {
        // W3 seam: the waterline band - each case triangle takes two slots, cut to the part above the bath surface
        for (uint k3 = 0; k3 < count; ++k3)
        {
            const uint tri = first + 2 * k3;
            if (tri + 1 >= fsMaxTriangles()) break;
            FsVertex q[3];
            float f[3];
            [unroll] for (uint v = 0; v < 3; ++v)
            {
                uint2 e = kFsEdges[cases.Load(4 * (mask * FS_CASE_STRIDE + 1 + 3 * k3 + v))];
                int3 ca = c + 1 + fsCorner(e.x), cb = c + 1 + fsCorner(e.y);
                float da = fsAt(ca), db = fsAt(cb), s = saturate((0.5 - da) / (db - da));
                float3 grad = lerp(fsGradient(ca), fsGradient(cb), s);
                float len = length(grad);
                uint ia = (ca.z * FS_WINDOW + ca.y) * FS_WINDOW + ca.x, ib = (cb.z * FS_WINDOW + cb.y) * FS_WINDOW + cb.x;
                float mass = lerp(da, db, s); float3 velocity = mass > 0 ? lerp(g_momentum[ia], g_momentum[ib], s) / mass : float3(0, 0, 0);
                q[v].p = axes * (world0 + lerp((float3)fsCorner(e.x), (float3)fsCorner(e.y), s) * h);
                q[v].n = axes * (len > 0 ? -grad / len : float3(0, 1, 0));
                q[v].v = axes * velocity;
                f[v] = fsAboveWater(q[v].p);
            }
            FsVertex none; none.p = q[0].p; none.n = float3(0, 1, 0); none.v = float3(0, 0, 0);  // zero area: drawn nowhere
            const uint above = (f[0] >= 0 ? 1u : 0u) + (f[1] >= 0 ? 1u : 0u) + (f[2] >= 0 ? 1u : 0u);
            if (above == 3u) { fsStore(vertices, velocities, tri, q[0], q[1], q[2], mirrored); fsStore(vertices, velocities, tri + 1, none, none, none, mirrored); }
            else if (above == 0u) { fsStore(vertices, velocities, tri, none, none, none, mirrored); fsStore(vertices, velocities, tri + 1, none, none, none, mirrored); }
            else
            {
                // rotate so that q[r] is the odd vertex out (the one alone on its side), keeping the cyclic order
                const bool lone = above == 1u;  // one above: the lone vertex is above; two above: it is below
                const uint r = (f[0] >= 0) == lone ? 0u : ((f[1] >= 0) == lone ? 1u : 2u);
                const FsVertex a = q[r], b = q[(r + 1) % 3], d = q[(r + 2) % 3];
                const float fa = f[r], fb = f[(r + 1) % 3], fd = f[(r + 2) % 3];
                if (lone)
                {
                    // a above, b and d below: the triangle a, (a b), (a d)
                    fsStore(vertices, velocities, tri, a, fsWaterline(a, b, fa, fb), fsWaterline(a, d, fa, fd), mirrored);
                    fsStore(vertices, velocities, tri + 1, none, none, none, mirrored);
                }
                else
                {
                    // a below, b and d above: the quad (a b), b, d, (d a) as two triangles
                    const FsVertex ab = fsWaterline(b, a, fb, fa), da = fsWaterline(d, a, fd, fa);
                    fsStore(vertices, velocities, tri, ab, b, d, mirrored);
                    fsStore(vertices, velocities, tri + 1, ab, d, da, mirrored);
                }
            }
        }
        return;
    }
    for (uint k2 = 0; k2 < count; ++k2)
    {
        uint tri = first + k2; if (tri >= fsMaxTriangles()) break;
        [unroll] for (uint v = 0; v < 3; ++v)
        {
            uint2 e = kFsEdges[cases.Load(4 * (mask * FS_CASE_STRIDE + 1 + 3 * k2 + v))];
            int3 ca = c + 1 + fsCorner(e.x), cb = c + 1 + fsCorner(e.y);
            float da = fsAt(ca), db = fsAt(cb), s = saturate((0.5 - da) / (db - da));
            float3 p = lerp((float3)fsCorner(e.x), (float3)fsCorner(e.y), s);
            float3 grad = lerp(fsGradient(ca), fsGradient(cb), s);
            float len = length(grad);
            float3 n = len > 0 ? -grad / len : float3(0, 1, 0);
            const uint slot = mirrored && v != 0 ? 3 - v : v;
            uint at = (tri * 3 + slot) * 32;
            vertices.Store4(at, asuint(float4(axes * (world0 + p * h), 1)));
            vertices.Store4(at + 16, asuint(float4(axes * n, 0)));
            // Velocity: the density-weighted mean along the edge (the vertex density is 0.5 of the rest density).
            uint ia = (ca.z * FS_WINDOW + ca.y) * FS_WINDOW + ca.x, ib = (cb.z * FS_WINDOW + cb.y) * FS_WINDOW + cb.x;
            float mass = lerp(da, db, s); float3 velocity = mass > 0 ? lerp(g_momentum[ia], g_momentum[ib], s) / mass : float3(0, 0, 0);
            velocities.Store4((tri * 3 + slot) * 16, asuint(float4(axes * velocity, 0)));
        }
    }
}
