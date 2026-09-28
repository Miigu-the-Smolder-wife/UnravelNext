// unx-kernel: cs_6_6 main
// r.gi.screen: M's per-pixel GI cache irradiance (front side) as a pass of its own, written to view.giIrradiance for M's
// shading kernel to read once (R_STATUS_KO.md 0, GI tile path verdict: the lookup costs 2.44 ms at 4K in a kernel of its
// own occupancy [measured], against ~3.4 ms inside M's shading kernel [expected]). The same function on the same inputs
// as M (GiScreenInputs.hlsli), so the only difference is the storage: RGBA16F, rgb = irradiance x the view's exposure
// (relative rounding <= 2^-11; pre-exposed so no value leaves the half range), a = 1 where the cache had data (weight
// > 0), 0 where M keeps the screen probes' irradiance. Pixels without a surface get 0.
// Quad sharing (bit-identical to giCacheIrradianceScreen per pixel): the four lanes of a quad (neighbouring pixels) mostly
// look up the same 8 cells of a level (the cells span 9-36 px). Where all four have the same level, cell origin and
// normal class, each lane resolves two of the 8 corners (hash probe, update count, anchor: three dependent loads each)
// and the quad exchanges them (QuadReadLaneAt); where they also have the same normal bits (surfaces without normal-map
// detail), each lane evaluates two of the corner maps too (four Load4 and 16 unpacks each) and the values are exchanged.
// Every lane then sums its own trilinear weights over the same values in the same order as giScreenLevel. Which pixels
// form a quad does not matter: sharing happens only where the keys are equal. Lanes outside the view take part in the
// exchange with an invalid key (no early return before the quad operations).
// P[0] = { cache SRV, depth SRV, gbuffer SRV, output UAV }, P[1] = { width, height, 0, 0 }; b1 = the main view.
#define GI_CORNER_BATCH 4u  // the cache's corner lookups batched (GiCache.hlsli)
#include "Passes/GI/GiCache.hlsli"
#include "Passes/GI/GiScreenInputs.hlsli"

uint giQuadAll(bool v)
{
    const uint u = v ? 1u : 0u;
    return QuadReadLaneAt(u, 0) & QuadReadLaneAt(u, 1) & QuadReadLaneAt(u, 2) & QuadReadLaneAt(u, 3);
}

float3 giCacheIrradianceScreenQuad(ByteAddressBuffer b, GiHeader h, bool valid, float3 worldPos, float3 normal, out float weight)
{
    uint level = 0;
    const float beta = valid ? giLevelBand(h, worldPos, level) : 0.0;
    const uint nc = giNormalClass(normal);
    const uint quadLane = WaveGetLaneIndex() & 3u;
    float3 result = 0;
    float remain = 1;
    [loop] for (uint k = 0; k < GI_FILL_LEVELS; ++k)
    {
        // (the loop runs its full count on every lane: the quad operations need the whole quad)
        const bool active = valid && remain > 1e-3 && level <= h.maxLevel;
        const float3 f = worldPos / giCellSize(h, min(level, h.maxLevel)) - 0.5;
        const int3 c0 = int3(floor(f));
        const bool sameCells = active && level == QuadReadLaneAt(level, 0) && nc == QuadReadLaneAt(nc, 0) && all(c0 == QuadReadLaneAt(c0, 0));
        const bool shareCells = giQuadAll(sameCells) != 0;
        const bool sameNormal = all(asuint(normal) == asuint(QuadReadLaneAt(normal, 0)));
        const bool shareMaps = shareCells && giQuadAll(sameNormal) != 0;
        uint4 entryLo = GI_ENTRY_PENDING, entryHi = GI_ENTRY_PENDING, anchorLo = 0, anchorHi = 0;
        uint has = 0, judged = GI_VIS_UNKNOWN;
        if (shareCells)
        {
            // this lane's two cells (giScreenCell's result), their loads issued together
            const uint cA = 2 * quadLane, cB = cA + 1;
            const uint64_t kA = giKey(level, nc, c0 + int3(cA & 1, (cA >> 1) & 1, cA >> 2)), kB = giKey(level, nc, c0 + int3(cB & 1, (cB >> 1) & 1, cB >> 2));
            const uint sA = giHomeSlot(h, kA), sB = giHomeSlot(h, kB);
            const uint4 fA = b.Load4(h.offTable + sA * 16), fB = b.Load4(h.offTable + sB * 16);
            uint e0 = giFindFrom(b, h, kA, sA, fA), e1 = giFindFrom(b, h, kB, sB, fB);
            const uint i0 = e0 != GI_ENTRY_PENDING ? e0 : 0u, i1 = e1 != GI_ENTRY_PENDING ? e1 : 0u;
            const uint u0 = b.Load(h.offSh + i0 * GI_SH_STRIDE + GI_SH_UPDATES), u1 = b.Load(h.offSh + i1 * GI_SH_STRIDE + GI_SH_UPDATES);
            const uint n0 = b.Load(h.offAnchor + i0 * 16 + 12), n1 = b.Load(h.offAnchor + i1 * 16 + 12);
            if (e0 != GI_ENTRY_PENDING && u0 == 0) e0 = GI_ENTRY_PENDING;
            if (e1 != GI_ENTRY_PENDING && u1 == 0) e1 = GI_ENTRY_PENDING;
            const uint a0 = e0 != GI_ENTRY_PENDING ? n0 : 0u, a1 = e1 != GI_ENTRY_PENDING ? n1 : 0u;
            // the level is judged when the quad's 8 corners with data are all converged (giScreenSeen)
            const bool pairJudged = (e0 == GI_ENTRY_PENDING || giVisJudged(h, u0)) && (e1 == GI_ENTRY_PENDING || giVisJudged(h, u1));
            judged = giQuadAll(pairJudged) != 0 ? GI_VIS_JUDGED : GI_VIS_YOUNG;
            [unroll] for (uint c = 0; c < 8; ++c)
            {
                const uint entry = QuadReadLaneAt((c & 1) ? e1 : e0, c >> 1);
                const uint anchor = QuadReadLaneAt((c & 1) ? a1 : a0, c >> 1);
                if (c < 4) entryLo[c] = entry, anchorLo[c] = anchor;
                else entryHi[c - 4] = entry, anchorHi[c - 4] = anchor;
                if (entry != GI_ENTRY_PENDING) has |= 1u << c;
            }
        }
        else if (active)
            giScreenCells(b, h, level, nc, c0, entryLo, entryHi, anchorLo, anchorHi, has, judged);
        // this lane's corners: those whose anchor sees this lane's point (gi.anchor_visibility; the shared maps are
        // evaluated for the quad's cells, the weights take this lane's)
        const uint hasShared = has;
        if (active) has = giScreenSeen(b, h, entryLo, entryHi, anchorLo, anchorHi, has, worldPos, giCellSize(h, min(level, h.maxLevel)), judged);
        float3 s = 0;
        float w = 0;
        if (shareMaps)
        {
            // corners 2q and 2q + 1 evaluated here (0 where the corner has no data), then every lane's own weights
            // (both maps' loads unconditional: a corner without data reads entry 0, and its value is not used)
            const uint cA = 2 * quadLane, cB = cA + 1;
            const bool onA = (hasShared & (1u << cA)) != 0, onB = (hasShared & (1u << cB)) != 0;
            const float3 mA = giIrrMapAt(b, h, onA ? giScreenPick(entryLo, entryHi, cA) : 0u, giUnpackAnchorNormal(onA ? giScreenPick(anchorLo, anchorHi, cA) : 0u), normal);
            const float3 mB = giIrrMapAt(b, h, onB ? giScreenPick(entryLo, entryHi, cB) : 0u, giUnpackAnchorNormal(onB ? giScreenPick(anchorLo, anchorHi, cB) : 0u), normal);
            const float3 m0 = onA ? mA : 0, m1 = onB ? mB : 0;
            const float3 share = normal * normal;
            const float3 t = f - floor(f);
            [unroll] for (uint c = 0; c < 8; ++c)
            {
                const float3 m = QuadReadLaneAt((c & 1) ? m1 : m0, c >> 1);
                const float wc = giScreenCornerWeight(has, c, t, share);
                if (wc <= 0) continue;
                s += wc * m;
                w += wc;
            }
        }
        else if (active)
            giScreenLevel(b, h, entryLo, entryHi, anchorLo, anchorHi, has, f - floor(f), normal, s, w);
        if (active)
        {
            const float take = k == 0 ? 1 - beta : 1.0;  // the band passes beta of the point on to the next level
            result += (remain * take) * s;
            remain *= 1 - take * w;
            ++level;
        }
    }
    weight = valid ? 1 - remain : 0;
    return weight > 0 ? result / weight : 0;
}

[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    const bool inView = all(pixel < P[1].xy);
    const uint2 readPixel = min(pixel, P[1].xy - 1);
    Texture2D<float> depth = ResourceDescriptorHeap[P[0].y];
    Texture2D<uint2> gbuffer = ResourceDescriptorHeap[P[0].z];
    RWTexture2D<float4> output = ResourceDescriptorHeap[P[0].w];
    float3 worldPos, nv;
    const bool surface = giScreenInputs(readPixel, depth.Load(int3(readPixel, 0)), gbuffer.Load(int3(readPixel, 0)), worldPos, nv) && inView;
    ByteAddressBuffer cache = ResourceDescriptorHeap[P[0].x];
    float weight;
    const float3 e = giCacheIrradianceScreenQuad(cache, giHeader(cache), surface, worldPos, nv, weight);
    if (inView) output[pixel] = surface && weight > 0 ? float4(e * g_exposure, 1) : float4(0, 0, 0, 0);
}
