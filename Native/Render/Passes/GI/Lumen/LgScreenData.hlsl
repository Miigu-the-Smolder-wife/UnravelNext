// unx-kernel: cs_6_6 main
// gi.lumen, r.gi.lg.screendata: per probe, the directions its pixels need (the BRDF's probability density as SH) and
// whether its surroundings were just disoccluded. One group per probe, 64 threads: thread (x, y) reads the pixel at the
// probe's pixel + ((x, y) + 0.5) / 8 x 2 - 1) x tile (the centre thread the probe's own pixel); a pixel counts when the
// probe lies on its plane (plane weight > 0.01; the centre always). Its diffuse transfer function (the clamped cosine
// lobe of its normal, SH) is averaged over the counted pixels: 9 floats. Disocclusion: a counted pixel is new when its
// history in the pixel filter holds fewer than P[2].y frames (or there is no history); the probe is flagged when at
// least P[2].z of its counted pixels are new (the probe filter then drops its angle test: LgFilter.hlsl).
// P[0] = LgSurface inputs, P[1] = { output UAV (raw, 40 B per probe: 9 SH floats, the flag as float), 0, 0, 0 },
// P[2] = { pixel history SRV (RGBA16F, a = frames; 0xFFFFFFFF: none), max frames (float), fraction (float), 0 },
// P[10].z = adaptive buffer SRV, P[10].w / P[11].y = probe depth / position SRVs. b1 = the view.
#include "Passes/GI/Lumen/LgInterpolate.hlsli"

groupshared float gs_sh[64][9];
groupshared uint gs_counted[64];
groupshared uint gs_new[64];

[numthreads(8, 8, 1)]
void main(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
    const uint2 atlas = group.xy;
    const uint probe = lgProbeIndex(atlas);
    ByteAddressBuffer adaptive = ResourceDescriptorHeap[P[10].z];
    Texture2D<float> probeDepth = ResourceDescriptorHeap[P[10].w];
    Texture2D<float4> probePosition = ResourceDescriptorHeap[P[11].y];
    const uint index = thread.y * 8 + thread.x;
    gs_counted[index] = 0;
    gs_new[index] = 0;
    [unroll] for (uint z = 0; z < 9; ++z) gs_sh[index][z] = 0;
    const bool live = atlas.x < lgProbeViewSize().x && probe < lgProbeCount(adaptive) && probeDepth[atlas] > 0;
    if (live)
    {
        const float depth = probeDepth[atlas];
        const float3 position = probePosition[atlas].xyz;
        const uint2 probePixel = lgProbePixel(adaptive, probe);
        const bool centre = all(thread.xy == 4);
        const float2 offset = centre ? float2(0, 0) : ((float2(thread.xy) + 0.5) / 8.0 * 2.0 - 1.0) * (float)lgTile();
        const uint2 pixel = (uint2)clamp((float2)probePixel + offset, 0.0, (float2)lgViewSize() - 1.0);
        const LgSurface s = lgSurface(pixel);
        if (s.valid)
        {
            const float4 plane = float4(s.normal, dot(s.position, s.normal));
            if (centre || lgPlaneWeight(plane, depth, position, LG_DEPTH_WEIGHT) > 0.01)
            {
                const LgSh lobe = lgShCosineLobe(s.normal);
                gs_sh[index][0] = lobe.a.x;
                gs_sh[index][1] = lobe.a.y;
                gs_sh[index][2] = lobe.a.z;
                gs_sh[index][3] = lobe.a.w;
                gs_sh[index][4] = lobe.b.x;
                gs_sh[index][5] = lobe.b.y;
                gs_sh[index][6] = lobe.b.z;
                gs_sh[index][7] = lobe.b.w;
                gs_sh[index][8] = lobe.c;
                gs_counted[index] = 1;
                bool isNew = true;
                if (P[2].x != 0xFFFFFFFFu && lgHistoryValid())
                {
                    // the pixel's own point one frame ago, in the previous view (the nearest history pixel)
                    float2 prevPixel;
                    float prevDepth;
                    if (giPreviousPixel(lgPreviousPosition(pixel, s), (float2)lgViewSize(), prevPixel, prevDepth))
                    {
                        Texture2D<float4> history = ResourceDescriptorHeap[P[2].x];
                        const int2 q = (int2)floor(prevPixel);
                        if (all(q >= 0) && all(q < (int2)lgViewSize())) isNew = history[q].a < asfloat(P[2].y);  // a = frames + 1 (LgTemporal.hlsl)
                    }
                }
                gs_new[index] = isNew ? 1u : 0u;
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (index != 0) return;
    RWByteAddressBuffer output = ResourceDescriptorHeap[P[1].x];
    float sum[9] = { 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    uint counted = 0, fresh = 0;
    [loop] for (uint i = 0; i < 64; ++i)
    {
        counted += gs_counted[i];
        fresh += gs_new[i];
        [unroll] for (uint k = 0; k < 9; ++k) sum[k] += gs_sh[i][k];
    }
    const float scale = counted > 0 ? 1.0 / counted : 0.0;
    const uint address = probe * 40;
    [unroll] for (uint k2 = 0; k2 < 9; ++k2) output.Store(address + k2 * 4, asuint(sum[k2] * scale));
    output.Store(address + 36, asuint(counted > 0 && (float)fresh >= (float)counted * asfloat(P[2].z) ? 1.0 : 0.0));
}
