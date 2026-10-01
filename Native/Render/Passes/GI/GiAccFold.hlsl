// unx-kernel: cs_6_6 main
// gi.hit_accumulator_pool (GiAccPool.hlsli, RENDERER_REDESIGN_V2 12.8): the accumulator's per-frame passes.
// P[0] = { pool UAV, mode, list (fold), levels (gi.hit_accumulator_levels) }, P[1] = { slots, frame, lighting epoch, exposure scale (float) },
// P[2] = { keep = 1 - alpha (float), min samples (float), fine scale (float), GI cellSize0 (float) } (modes 0, 2: the header)
//
// mode 0, r.gi.acc.begin (before the GI rays; one thread per slot): thread 0 writes the frame's header and resets the list
// counts; every thread empties its slot when no hit has touched it for GI_ACCP_KEEP_FRAMES frames (key, stamp, payload:
// its window is as old). (Eviction only here: during the hits and the folds a slot never becomes empty.)
// mode 2, r.gi.acc.clear (the first frame, and after an origin shift: the keys are world cells): every slot emptied.
//
// mode 1, r.gi.acc.fold<k> (after the GI rays; one thread per entry of list k, k = 0..3): the cell's frame sums
//   - are added to its parent cell's frame sums (k < 3: the next level, cells x 2; the parent is listed in list k + 1 by
//     its first child) - exact sums, so every level holds the hits of the cells below it;
//   - are folded into its window: N <- keep^age N + N_frame for every sum and the hit count (age = frames since its
//     last fold: the same weights for numerators and denominators), after the restart test - the frame's ratio of term
//     A's luminance against the window's, 3 standard errors of the frame (GiAccPool.hlsli) - and a restart on a new
//     lighting epoch;
//   - are cleared.
// Each thread writes its own cell's window; frame sums are taken with atomic exchanges, because a cell of list k can be
// the parent of other cells of list k (rays of different footprints write different levels): what its children add after
// its exchange waits for its turn in list k + 1, where it is listed by them. List k + 1 is complete before fold k + 1
// runs (the passes are ordered), so every contribution is folded once and passed up once.
#include "Passes/GI/GiAccPool.hlsli"

#define GI_ACCP_KEEP_FRAMES 240u

double giAccpSum(RWByteAddressBuffer pool, uint address)
{
    const uint2 w = pool.Load2(address);
    return (double)w.y * 4294967296.0 + (double)w.x;
}

[numthreads(64, 1, 1)]
void main(uint id : SV_DispatchThreadID)
{
    RWByteAddressBuffer pool = ResourceDescriptorHeap[P[0].x];
    if (P[0].y != 1)
    {
        // (the header from the constants: thread 0 writes it in this pass)
        GiAccPoolHeader n;
        n.slots = P[1].x;
        n.frame = P[1].y;
        n.epoch = P[1].z;
        n.exposure = asfloat(P[1].w);
        n.keep = asfloat(P[2].x);
        n.minSamples = asfloat(P[2].y);
        n.fineScale = asfloat(P[2].z);
        n.cellSize0 = asfloat(P[2].w);
        n.levels = clamp(P[0].w, 1u, GI_ACCP_LEVELS);
        if (id == 0)
        {
            pool.Store4(0, P[1]);
            pool.Store4(16, P[2]);
            pool.Store(40, P[0].w);
            pool.Store4(48, uint4(0, 0, 0, 0));
        }
        if (id >= n.slots) return;
        const uint stamp = pool.Load(giAccpStampAddress(n, id));
        const bool stale = stamp == 0 || n.frame - (stamp - 1) / GI_ACCP_LEVELS >= GI_ACCP_KEEP_FRAMES;
        const uint64_t key = giAccpLoadKey(pool, giAccpKeyAddress(n, id));
        if (P[0].y == 0 && (key == 0 || key == GI_ACCP_EVICTED || !stale)) return;
        const GiAccPoolHeader h = n;
        const uint base = giAccpPayload(h, id);
        [unroll] for (uint z = 0; z < GI_ACCP_PAYLOAD / 16; ++z) pool.Store4(base + z * 16, uint4(0, 0, 0, 0));
        pool.Store(giAccpStampAddress(h, id), 0u);
        // (evicted, not never-used: keys placed past this slot stay reachable; the clear of the whole pool leaves 0)
        pool.Store2(giAccpKeyAddress(h, id), uint2(P[0].y == 0 ? 1u : 0u, 0));
        return;
    }
    const GiAccPoolHeader h = giAccPoolHeader(pool);
    const uint list = P[0].z;
    if (id >= min(pool.Load(48 + list * 4), h.slots)) return;
    const uint slot = pool.Load(giAccpListAddress(h, list, id));
    const uint base = giAccpPayload(h, slot);
    // The frame's sums so far (fixed point), taken and left at 0.
    uint2 raw[GI_ACCP_SUMS];
    [unroll] for (uint k = 0; k < GI_ACCP_SUMS; ++k)
    {
        uint64_t taken;
        pool.InterlockedExchange64(base + k * 8, 0ull, taken);
        raw[k] = uint2((uint)taken, (uint)(taken >> 32));
    }
    uint count;
    pool.InterlockedExchange(base + GI_ACCP_COUNT, 0u, count);
    // (a child adds its sums, then its count: a parent taking them in between holds sums without a count - they still fold)
    uint any = count;
    [unroll] for (uint q = 0; q < GI_ACCP_SUMS; ++q) any |= raw[q].x | raw[q].y;
    if (any == 0) return;
    // Up one level.
    if (list + 1 < h.levels)
    {
        const uint parent = giAccpFindOrCreate(pool, h, giAccpParentKey(giAccpLoadKey(pool, giAccpKeyAddress(h, slot))));
        if (parent != GI_ACCP_NONE)
        {
            const uint pb = giAccpPayload(h, parent);
            uint64_t previous;
            [unroll] for (uint k = 0; k < GI_ACCP_SUMS; ++k)
            {
                const uint64_t v = ((uint64_t)raw[k].y << 32) | raw[k].x;
                if (v != 0) pool.InterlockedAdd64(pb + k * 8, v, previous);
            }
            uint previousCount;
            if (count != 0) pool.InterlockedAdd(pb + GI_ACCP_COUNT, count, previousCount);
            giAccpTouch(pool, h, parent, list + 1);
        }
    }
    // Into the window.
    float f[16];
    [unroll] for (uint j = 0; j < 16; ++j) f[j] = (float)(((double)raw[j].y * 4294967296.0 + (double)raw[j].x) / GI_ACCP_SCALE);
    const uint4 w = pool.Load4(base + GI_ACCP_WINDOW);  // n, frame of the last fold, epoch
    float held[16];
    {
        const float4 s0 = asfloat(pool.Load4(base + GI_ACCP_WINDOW_SUMS)), s1 = asfloat(pool.Load4(base + GI_ACCP_WINDOW_SUMS + 16)),
                     s2 = asfloat(pool.Load4(base + GI_ACCP_WINDOW_SUMS + 32)), s3 = asfloat(pool.Load4(base + GI_ACCP_WINDOW_SUMS + 48));
        const float all[16] = { s0.x, s0.y, s0.z, s0.w, s1.x, s1.y, s1.z, s1.w, s2.x, s2.y, s2.z, s2.w, s3.x, s3.y, s3.z, s3.w };
        held = all;
    }
    float keep = w.z == h.epoch && asfloat(w.x) > 0 ? pow(h.keep, (float)min(h.frame - w.y, 4096u)) : 0.0;
    if (keep > 0)
    {
        // Restart test (luminance of term A): R_f = N_f / D_f against the window's R, 3 sigma_f with
        // sigma_f^2 = sum k^2 (T - R)^2 / D_f^2 = (Q - 2 R P + R^2 K) / D_f^2 from the frame's moments (exposure-scaled).
        const float3 Y = float3(0.2126, 0.7152, 0.0722);
        const float nf = dot(float3(f[0], f[1], f[2]), Y) * h.exposure, df = dot(float3(f[3], f[4], f[5]), Y);
        const float nw = dot(float3(held[0], held[1], held[2]), Y) * h.exposure, dw = dot(float3(held[3], held[4], held[5]), Y);
        if (df > 0 && dw > 0)
        {
            const float r = nw / dw, rf = nf / df;
            const float q = (float)(((double)raw[16].y * 4294967296.0 + (double)raw[16].x) / GI_ACCP_MOMENT_SCALE);
            const float p = (float)(((double)raw[17].y * 4294967296.0 + (double)raw[17].x) / GI_ACCP_MOMENT_SCALE);
            const float kk = (float)(((double)raw[18].y * 4294967296.0 + (double)raw[18].x) / GI_ACCP_MOMENT_SCALE);
            const float variance = max(q - 2 * r * p + r * r * kk, 0.0) / (df * df);
            if (abs(rf - r) > 3 * sqrt(variance)) keep = 0;
        }
    }
    float o[16];
    [unroll] for (uint c = 0; c < 16; ++c) o[c] = held[c] * keep + f[c];
    pool.Store4(base + GI_ACCP_WINDOW, uint4(asuint(asfloat(w.x) * keep + (float)count), h.frame, h.epoch, 0));
    pool.Store4(base + GI_ACCP_WINDOW_SUMS, asuint(float4(o[0], o[1], o[2], o[3])));
    pool.Store4(base + GI_ACCP_WINDOW_SUMS + 16, asuint(float4(o[4], o[5], o[6], o[7])));
    pool.Store4(base + GI_ACCP_WINDOW_SUMS + 32, asuint(float4(o[8], o[9], o[10], o[11])));
    pool.Store4(base + GI_ACCP_WINDOW_SUMS + 48, asuint(float4(o[12], o[13], o[14], o[15])));
}
