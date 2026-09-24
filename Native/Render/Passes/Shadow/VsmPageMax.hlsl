// unx-kernel: cs_6_6 main
// Block hierarchy of each rendered page (one group per dirty page) for the visibility pass's exact classification:
// blocks of 8, 16, 32, 64 and 128 texels (the whole page). Each block stores
//   - min / max of the encoded heights (VSM_EMPTY = no caster; min = VSM_EMPTY when the block has an empty texel),
//   - the least-squares plane of its casters' heights, h ~ ref + a x + b y (x, y in page texels from the page corner,
//     ref = the page's highest caster), and residual bounds lo <= h - plane <= hi over its non-empty texels.
// 8-texel blocks fit their 64 texels; coarser blocks fit the summed moments of their children and bound the residual
// by the children's residual bounds plus the child-to-parent plane difference at the child's corners (conservative).
// Receivers lying on a caster surface are then proven lit (the block plane minus the receiver plane is linear, so its
// maximum over a block is at a corner) without per-texel taps. The page maximum also goes to the page metadata.
// Entry layout: VsmBlock (VsmCommon.hlsli), VSM_BLOCK_ENTRIES per physical page, offsets VSM_BLOCK_OFFSET_*.
// P[0].x dirty list SRV (raw), P[0].y pool SRV (Texture2D<uint>), P[0].z page metadata UAV, P[0].w VSM constants CBV,
// P[1].x unused, P[1].y blocks UAV (raw)
#include "Passes/Shadow/VsmCommon.hlsli"

struct Moments
{
    float n, sx, sy, sh, sxx, syy, sxy, sxh, syh;
};
groupshared Moments g_moments[256];
groupshared uint2 g_range[256];
groupshared float4 g_plane[256];  // a, b, c (c = offset of h - ref at x = y = 0), unused
groupshared float2 g_resid[256];
groupshared uint g_pageMax;

Moments addMoments(Moments p, Moments q)
{
    Moments r;
    r.n = p.n + q.n; r.sx = p.sx + q.sx; r.sy = p.sy + q.sy; r.sh = p.sh + q.sh;
    r.sxx = p.sxx + q.sxx; r.syy = p.syy + q.syy; r.sxy = p.sxy + q.sxy; r.sxh = p.sxh + q.sxh; r.syh = p.syh + q.syh;
    return r;
}

// Least-squares plane h = a x + b y + c; degenerate sets (fewer than three non-collinear texels) get the constant fit.
float3 fitPlane(Moments m)
{
    if (m.n < 1) return 0;
    const float mx = m.sx / m.n, my = m.sy / m.n, mh = m.sh / m.n;
    const float cxx = m.sxx / m.n - mx * mx, cyy = m.syy / m.n - my * my, cxy = m.sxy / m.n - mx * my;
    const float cxh = m.sxh / m.n - mx * mh, cyh = m.syh / m.n - my * mh;
    const float det = cxx * cyy - cxy * cxy;
    float a = 0, b = 0;
    if (m.n >= 3 && det > 1e-6 * max(cxx * cyy, 1e-12))
    {
        a = (cxh * cyy - cyh * cxy) / det;
        b = (cyh * cxx - cxh * cxy) / det;
    }
    return float3(a, b, mh - a * mx - b * my);
}

[numthreads(256, 1, 1)]
void main(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    ByteAddressBuffer dirty = ResourceDescriptorHeap[P[0].x];
    Texture2D<uint> pool = ResourceDescriptorHeap[P[0].y];
    RWByteAddressBuffer blocks = ResourceDescriptorHeap[P[1].y];
    ConstantBuffer<VsmConstants> c = ResourceDescriptorHeap[P[0].w];
    const uint phys = dirty.Load(8 + group.x * 8 + 4);
    const uint2 base = vsmPhysBase(c, phys);
    const uint pageBase = phys * VSM_BLOCK_ENTRIES * VSM_BLOCK_BYTES;
    const uint2 b = uint2(lane % 16, lane / 16);

    // 1. Min / max per 8 x 8 block, page maximum (the plane reference).
    uint lo = 0xFFFFFFFFu, hi = VSM_EMPTY;
    [loop] for (uint y = 0; y < 8; ++y)
        [unroll] for (uint x = 0; x < 8; ++x)
        {
            const uint h = pool.Load(int3(base + b * 8 + uint2(x, y), 0));
            lo = min(lo, h);
            hi = max(hi, h);
        }
    if (lane == 0) g_pageMax = VSM_EMPTY;
    GroupMemoryBarrierWithGroupSync();
    const uint waveMax = WaveActiveMax(hi);
    if (WaveIsFirstLane()) InterlockedMax(g_pageMax, waveMax);
    GroupMemoryBarrierWithGroupSync();
    const float ref = g_pageMax == VSM_EMPTY ? 0.0 : vsmDecode(g_pageMax);

    // 2. Plane and residual bounds of the 8 x 8 block.
    Moments m = (Moments)0;
    [loop] for (uint y2 = 0; y2 < 8; ++y2)
        [unroll] for (uint x2 = 0; x2 < 8; ++x2)
        {
            const uint e = pool.Load(int3(base + b * 8 + uint2(x2, y2), 0));
            if (e == VSM_EMPTY) continue;
            const float px = b.x * 8 + x2 + 0.5, py = b.y * 8 + y2 + 0.5, h = vsmDecode(e) - ref;
            m.n += 1; m.sx += px; m.sy += py; m.sh += h;
            m.sxx += px * px; m.syy += py * py; m.sxy += px * py; m.sxh += px * h; m.syh += py * h;
        }
    const float3 plane = fitPlane(m);
    float rlo = 3.0e38, rhi = -3.0e38;
    [loop] for (uint y3 = 0; y3 < 8; ++y3)
        [unroll] for (uint x3 = 0; x3 < 8; ++x3)
        {
            const uint e = pool.Load(int3(base + b * 8 + uint2(x3, y3), 0));
            if (e == VSM_EMPTY) continue;
            const float r = vsmDecode(e) - ref - (plane.x * (b.x * 8 + x3 + 0.5) + plane.y * (b.y * 8 + y3 + 0.5) + plane.z);
            rlo = min(rlo, r);
            rhi = max(rhi, r);
        }
    VsmBlock blk;
    blk.range = uint2(lo, hi);
    blk.plane = plane;
    blk.residual = float2(rlo, rhi);
    blk.ref = ref;
    blocks.Store<VsmBlock>(pageBase + (VSM_BLOCK_OFFSET_8 + lane) * VSM_BLOCK_BYTES, blk);
    g_moments[lane] = m;
    g_range[lane] = uint2(lo, hi);
    g_plane[lane] = float4(plane, 0);
    g_resid[lane] = float2(rlo, rhi);
    GroupMemoryBarrierWithGroupSync();

    // 3. Coarser blocks from their four children.
    uint n = 16;
    uint offset = VSM_BLOCK_OFFSET_16;
    uint size = 8;  // child block size in texels
    [loop] for (uint level = 1; level <= 4; ++level)
    {
        const uint half = n / 2;
        VsmBlock pb = (VsmBlock)0;
        Moments pm = (Moments)0;
        if (lane < half * half)
        {
            const uint2 q = uint2(lane % half, lane / half) * 2;
            uint2 range = uint2(0xFFFFFFFFu, VSM_EMPTY);
            [unroll] for (uint j = 0; j < 4; ++j)
            {
                const uint ci = (q.y + (j >> 1)) * n + q.x + (j & 1);
                range = uint2(min(range.x, g_range[ci].x), max(range.y, g_range[ci].y));
                pm = addMoments(pm, g_moments[ci]);
            }
            const float3 pp = fitPlane(pm);
            float plo = 3.0e38, phi = -3.0e38;
            [unroll] for (uint j2 = 0; j2 < 4; ++j2)
            {
                const uint2 cq = q + uint2(j2 & 1, j2 >> 1);
                const uint ci = cq.y * n + cq.x;
                if (g_moments[ci].n < 1) continue;
                // Child plane minus parent plane is linear: its extremes over the child block are at the corners.
                const float3 d = g_plane[ci].xyz - pp;
                const float2 o = float2(cq) * size;
                float dlo = 3.0e38, dhi = -3.0e38;
                [unroll] for (uint k = 0; k < 4; ++k)
                {
                    const float2 xy = o + float2(k & 1, k >> 1) * size;
                    const float v = d.x * xy.x + d.y * xy.y + d.z;
                    dlo = min(dlo, v);
                    dhi = max(dhi, v);
                }
                plo = min(plo, g_resid[ci].x + dlo);
                phi = max(phi, g_resid[ci].y + dhi);
            }
            pb.range = range;
            pb.plane = pp;
            pb.residual = float2(plo, phi);
            pb.ref = ref;
        }
        GroupMemoryBarrierWithGroupSync();
        if (lane < half * half)
        {
            g_moments[lane] = pm;
            g_range[lane] = pb.range;
            g_plane[lane] = float4(pb.plane, 0);
            g_resid[lane] = pb.residual;
            blocks.Store<VsmBlock>(pageBase + (offset + lane) * VSM_BLOCK_BYTES, pb);
        }
        GroupMemoryBarrierWithGroupSync();
        offset += half * half;
        n = half;
        size *= 2;
    }
    if (lane == 0)
    {
        RWStructuredBuffer<VsmPageMeta> meta = ResourceDescriptorHeap[P[0].z];
        meta[phys].maxHeight = g_pageMax;
    }
}
