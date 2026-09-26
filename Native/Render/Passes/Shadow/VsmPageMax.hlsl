// unx-kernel: cs_6_6 main
// Block hierarchy of each rendered page (one group per page, every texel read once) for the visibility pass's exact classification:
// blocks of 8, 16, 32, 64 and 128 texels (the whole page). Each block stores
//   - min / max of the encoded heights (VSM_EMPTY = no caster; min = VSM_EMPTY when the block has an empty texel),
//   - the least-squares plane of its casters' heights, h ~ ref + a x + b y (x, y in page texels from the page corner,
//     ref = the page's highest caster), and residual bounds lo <= h - plane <= hi over its non-empty texels.
// 8-texel blocks fit their 64 texels; coarser blocks fit the summed moments of their children and bound the residual
// by the children's residual bounds plus the child-to-parent plane difference at the child's corners (conservative).
// Receivers lying on a caster surface are then proven lit (the block plane minus the receiver plane is linear, so its
// maximum over a block is at a corner) without per-texel taps. The page maximum also goes to the page metadata.
// Entry layout: VsmBlock (VsmCommon.hlsli), VSM_BLOCK_ENTRIES per physical page, offsets VSM_BLOCK_OFFSET_*.
// Texels come from the depth atlas as keys (VsmCommon.hlsli: sun v -> vsmEncode(h), local reversed-Z depth ->
// vsmEncode(-z)); one group per page of this frame (the dirty list: every requested page, VsmScan).
// P[0].x dirty list SRV (raw), P[0].y atlas SRV (Texture2D<float>), P[0].z page metadata UAV, P[0].w VSM constants CBV,
// P[1].x local lights SRV (StructuredBuffer<VsmLocalLight>; 0xFFFFFFFF: none), P[1].y blocks UAV (raw)
#include "Passes/Shadow/VsmLocal.hlsli"

// Key decode of this page: sun (a, b) = (hMin, hMax), local (a, b) = (near, far).
struct PageDecode
{
    bool local;
    float a, b;
};
uint pageKey(Texture2D<float> atlas, PageDecode d, uint phys, uint2 local)
{
    const float v = atlas.Load(vsmAtlasTexel(phys, local));
    return d.local ? vsmLocalKeyOfDepth(v, d.a, d.b) : vsmSunKey(v, d.a, d.b);
}

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

groupshared float g_blockRef[256];  // each 8-texel block's highest caster (the reference of its moments until step 2b)

// Texel t (0..63, row-major) of 8-texel block b of the page.
uint2 blockTexel(uint b, uint t) { return uint2(b % 16, b / 16) * 8 + uint2(t % 8, t / 8); }

[numthreads(256, 1, 1)]
void main(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    ByteAddressBuffer dirty = ResourceDescriptorHeap[P[0].x];
    Texture2D<float> pool = ResourceDescriptorHeap[P[0].y];
    RWByteAddressBuffer blocks = ResourceDescriptorHeap[P[1].y];
    ConstantBuffer<VsmConstants> c = ResourceDescriptorHeap[P[0].w];
    const uint2 entry = dirty.Load2(8 + group.x * 8);
    const uint phys = entry.y;
    const uint pageBase = phys * VSM_BLOCK_ENTRIES * VSM_BLOCK_BYTES;
    PageDecode dec;
    dec.local = entry.x >= VSM_SUN_SLOTS;
    dec.a = c.hMin;
    dec.b = c.hMax;
    if (dec.local)
    {
        StructuredBuffer<VsmLocalLight> lights = ResourceDescriptorHeap[P[1].x];
        const VsmLocalLight l = lights[(entry.x - VSM_SUN_SLOTS) / VSM_LOCAL_LIGHT_SLOTS];
        dec.a = l.nearM;
        dec.b = l.farM;
    }

    // 1-2. One wave per 8 x 8 block (every texel read once): its lanes hold the block's texels (two per lane on waves of
    //      32, one on 64; smaller waves read the texels again for the second and third sums), min / max, moments and the
    //      least-squares plane relative to the block's own highest caster, residual bounds; wave reductions only.
    const uint L = WaveGetLaneCount(), inWave = WaveGetLaneIndex(), wave = lane / L, waves = 256 / L;
    [loop] for (uint b = wave; b < 256; b += waves)
    {
        uint k0 = VSM_EMPTY, k1 = VSM_EMPTY, lo = 0xFFFFFFFFu, hi = VSM_EMPTY;
        [loop] for (uint t = inWave, i = 0; t < 64; t += L, ++i)
        {
            const uint k = pageKey(pool, dec, phys, blockTexel(b, t));
            if (i == 0) k0 = k;
            else if (i == 1) k1 = k;
            lo = min(lo, k);
            hi = max(hi, k);
        }
        lo = WaveActiveMin(lo);
        hi = WaveActiveMax(hi);
        const float bref = hi == VSM_EMPTY ? 0.0 : vsmDecode(hi);
        Moments m = (Moments)0;
        [loop] for (uint t2 = inWave, i2 = 0; t2 < 64; t2 += L, ++i2)
        {
            const uint k = i2 == 0 ? k0 : i2 == 1 ? k1 : pageKey(pool, dec, phys, blockTexel(b, t2));
            if (k == VSM_EMPTY) continue;
            const float2 xy = float2(blockTexel(b, t2)) + 0.5;
            const float h = vsmDecode(k) - bref;
            m.n += 1; m.sx += xy.x; m.sy += xy.y; m.sh += h;
            m.sxx += xy.x * xy.x; m.syy += xy.y * xy.y; m.sxy += xy.x * xy.y; m.sxh += xy.x * h; m.syh += xy.y * h;
        }
        m.n = WaveActiveSum(m.n); m.sx = WaveActiveSum(m.sx); m.sy = WaveActiveSum(m.sy); m.sh = WaveActiveSum(m.sh);
        m.sxx = WaveActiveSum(m.sxx); m.syy = WaveActiveSum(m.syy); m.sxy = WaveActiveSum(m.sxy);
        m.sxh = WaveActiveSum(m.sxh); m.syh = WaveActiveSum(m.syh);
        const float3 plane = fitPlane(m);
        float rlo = 3.0e38, rhi = -3.0e38;
        [loop] for (uint t3 = inWave, i3 = 0; t3 < 64; t3 += L, ++i3)
        {
            const uint k = i3 == 0 ? k0 : i3 == 1 ? k1 : pageKey(pool, dec, phys, blockTexel(b, t3));
            if (k == VSM_EMPTY) continue;
            const float2 xy = float2(blockTexel(b, t3)) + 0.5;
            const float r = vsmDecode(k) - bref - (plane.x * xy.x + plane.y * xy.y + plane.z);
            rlo = min(rlo, r);
            rhi = max(rhi, r);
        }
        rlo = WaveActiveMin(rlo);
        rhi = WaveActiveMax(rhi);
        if (WaveIsFirstLane())
        {
            g_moments[b] = m;
            g_range[b] = uint2(lo, hi);
            g_plane[b] = float4(plane, 0);
            g_resid[b] = float2(rlo, rhi);
            g_blockRef[b] = bref;
        }
    }
    GroupMemoryBarrierWithGroupSync();

    // 2b. The page maximum (the reference of every block's plane) and the 8-texel blocks relative to it: a constant
    //     shift d = blockRef - ref moves the plane's offset and the h-moments (sh + n d, sxh + sx d, syh + sy d); the
    //     residual bounds are unchanged.
    const uint pageMax = WaveActiveMax(g_range[lane].y);
    if (lane == 0) g_pageMax = VSM_EMPTY;
    GroupMemoryBarrierWithGroupSync();
    if (WaveIsFirstLane()) InterlockedMax(g_pageMax, pageMax);
    GroupMemoryBarrierWithGroupSync();
    const float ref = g_pageMax == VSM_EMPTY ? 0.0 : vsmDecode(g_pageMax);
    {
        Moments m = g_moments[lane];
        float3 plane = g_plane[lane].xyz;
        if (m.n >= 1)
        {
            const float d = g_blockRef[lane] - ref;
            m.sh += m.n * d;
            m.sxh += m.sx * d;
            m.syh += m.sy * d;
            plane.z += d;
        }
        VsmBlock blk;
        blk.range = g_range[lane];
        blk.plane = plane;
        blk.residual = g_resid[lane];
        blk.ref = ref;
        blocks.Store<VsmBlock>(pageBase + (VSM_BLOCK_OFFSET_8 + lane) * VSM_BLOCK_BYTES, blk);
        GroupMemoryBarrierWithGroupSync();
        g_moments[lane] = m;
        g_plane[lane] = float4(plane, 0);
    }
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
