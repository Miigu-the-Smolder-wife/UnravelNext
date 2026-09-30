// unx-kernel: cs_6_6 main
// Radiance-map path guiding (gi.path_guiding; RENDERER_REDESIGN_V2 11.7 D-12, a decision item: off by default). Per update
// slot, the texel choice of its 64 rays: a mixture of the uniform texel choice (share gi.path_guiding_uniform_share) and
// one proportional to each texel's share of the entry's irradiance in the history (stored texel radiance x cos x solid
// angle at the texel centre), as 64 cumulative probabilities (the last is 1). GiTrace draws each ray's texel from them
// (jittered uniformly inside it); GiIntegrate weights each ray by 1 / (its mixture pdf) for the map and SH (one-sample
// balance heuristic of the two strategies: unbiased while the uniform share is > 0) and averages the rays that landed in
// a texel for that texel. The ray count is unchanged; a lamp's hotspot texel gets several rays instead of one.
// Entries without history (no texels yet) and slots without an entry get the uniform choice.
// P[0] = { cache UAV, updates per frame, guide UAV (64 floats per slot), uniform share (float) }
#include "Passes/GI/GiInternal.hlsli"

groupshared float gs_w[GI_TEXEL_COUNT];

[numthreads(64, 1, 1)]
void main(uint lane : SV_GroupIndex, uint slot : SV_GroupID)
{
    if (slot >= P[0].y) return;
    RWByteAddressBuffer b = ResourceDescriptorHeap[P[0].x];
    RWStructuredBuffer<float> guide = ResourceDescriptorHeap[P[0].z];
    const float uniformShare = asfloat(P[0].w);
    const GiHeader h = giHeader(b);
    uint entry;
    bool background;
    float w = 0;
    if (giUpdateSlot(b, h, slot, entry, background) && giHistory(b, h, entry) != 0)
    {
        const uint2 t = b.Load2(h.offTexels + (entry * GI_TEXEL_COUNT + lane) * 8);
        const float3 L = float3(f16tof32(t.x), f16tof32(t.x >> 16), f16tof32(t.y));
        // the texel centre's direction density (GiIntegrate: dw / (du dv) = 2 / |p|^3) and cosine
        const float2 uv = (float2(lane % GI_TEXELS, lane / GI_TEXELS) + 0.5) / GI_TEXELS;
        const float2 xy = uv * 2 - 1;
        const float2 ab = float2((xy.x + xy.y) * 0.5, (xy.x - xy.y) * 0.5);
        const float3 q = float3(ab, 1 - abs(ab.x) - abs(ab.y));
        const float len = length(q);
        const float lum = dot(max(L, 0), float3(0.2126, 0.7152, 0.0722));
        w = lum == lum && lum < 3.0e38 ? lum * (q.z / len) * 2 / (len * len * len) : 0;
    }
    gs_w[lane] = w;
    GroupMemoryBarrierWithGroupSync();
    float total = 0, below = 0;
    [loop] for (uint k = 0; k < GI_TEXEL_COUNT; ++k)
    {
        total += gs_w[k];
        if (k <= lane) below += gs_w[k];
    }
    const float guided = total > 0 ? 1 - uniformShare : 0;
    const float cumulative = (1 - guided) * (lane + 1) / (float)GI_TEXEL_COUNT + (total > 0 ? guided * below / total : 0);
    guide[slot * GI_TEXEL_COUNT + lane] = lane == GI_TEXEL_COUNT - 1 ? 1.0 : cumulative;
}
