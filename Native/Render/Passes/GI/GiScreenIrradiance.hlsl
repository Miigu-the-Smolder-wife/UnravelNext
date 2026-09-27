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
        uint has = 0;
        if (shareCells)
        {
            uint a0, a1;
            const uint cA = 2 * quadLane, cB = cA + 1;
            const uint e0 = giScreenCell(b, h, giKey(level, nc, c0 + int3(cA & 1, (cA >> 1) & 1, cA >> 2)), a0);
            const uint e1 = giScreenCell(b, h, giKey(level, nc, c0 + int3(cB & 1, (cB >> 1) & 1, cB >> 2)), a1);
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
        {
            [unroll] for (uint c = 0; c < 8; ++c)
            {
                uint anchor;
                const uint entry = giScreenCell(b, h, giKey(level, nc, c0 + int3(c & 1, (c >> 1) & 1, c >> 2)), anchor);
                if (c < 4) entryLo[c] = entry, anchorLo[c] = anchor;
                else entryHi[c - 4] = entry, anchorHi[c - 4] = anchor;
                if (entry != GI_ENTRY_PENDING) has |= 1u << c;
            }
        }
        float3 s = 0;
        float w = 0;
        if (shareMaps)
        {
            // corners 2q and 2q + 1 evaluated here (0 where the corner has no data), then every lane's own weights
            float3 m0 = 0, m1 = 0;
            const uint cA = 2 * quadLane, cB = cA + 1;
            if ((has & (1u << cA)) != 0) m0 = giIrrMapAt(b, h, giScreenPick(entryLo, entryHi, cA), giUnpackAnchorNormal(giScreenPick(anchorLo, anchorHi, cA)), normal);
            if ((has & (1u << cB)) != 0) m1 = giIrrMapAt(b, h, giScreenPick(entryLo, entryHi, cB), giUnpackAnchorNormal(giScreenPick(anchorLo, anchorHi, cB)), normal);
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
