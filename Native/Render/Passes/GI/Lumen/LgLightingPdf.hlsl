// unx-kernel: cs_6_6 main
// gi.lumen, r.gi.lg.lightingpdf: per probe, where the light came from last frame - the second factor of the rays'
// importance (LgGenerateRays.hlsl). One group per probe, thread = direction texel (8 x 8). The texel's value is the
// luminance of the previous frame's probe radiance in that texel, from the up to 4 history tile probes around the
// probe's own previous position that lie on its plane (relative plane distance: exp2(-10000 x^2) > 0.1), equal weights.
// With fewer than 4 history probes the rest comes from the radiance cache in that direction, and without the cache
// the texel is 1 (uniform). Output: the texel's value over the probe's sum (a density over the 64 texels).
// P[0] = LgSurface inputs, P[1] = { output UAV (R16F, atlas x 8), previous probe depth SRV, previous probe position SRV,
// previous probe radiance SRV (atlas x 8) }, P[2] = { previous temporal index, previous exposure / this exposure (float),
// history distance threshold (unused: the plane test decides), 0 }, P[3] = { radiance cache params (raw SRV; 0xFFFFFFFF:
// none), indirection SRV, atlas SRV, 0 }, P[10].z = adaptive SRV, P[10].w / P[11].x / P[11].y =
// probe depth / normal / position SRVs. b1 = the view.
#include "Passes/GI/Lumen/LgInterpolate.hlsli"
#include "Passes/GI/Lumen/LgRadianceCache.hlsli"

groupshared float gs_value[64];

[numthreads(8, 8, 1)]
void main(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
    const uint probe = group.x;
    const uint2 atlas = lgAtlasCoord(probe);
    ByteAddressBuffer adaptive = ResourceDescriptorHeap[P[10].z];
    Texture2D<float> probeDepth = ResourceDescriptorHeap[P[10].w];
    Texture2D<float2> probeNormal = ResourceDescriptorHeap[P[11].x];
    Texture2D<float4> probePosition = ResourceDescriptorHeap[P[11].y];
    const uint index = thread.y * 8 + thread.x;
    const bool live = atlas.x < lgProbeViewSize().x && probe < lgProbeCount(adaptive) && probeDepth[atlas] > 0;
    float value = 0;
    if (live)
    {
        float3 lighting = 0;
        float transparency = 1;
        if (lgHistoryValid())
        {
            const float depth = probeDepth[atlas];
            const float3 position = probePosition[atlas].xyz;
            const float3 normal = lgDecodeNormal(probeNormal[atlas]);
            const uint2 pixel = lgProbePixel(adaptive, probe);
            LgSurface s = (LgSurface)0;
            s.valid = true;
            s.position = position;
            s.normal = normal;
            float2 prevPixel;
            float prevDepth;
            if (giPreviousPixel(lgPreviousPosition(pixel, s), (float2)lgViewSize(), prevPixel, prevDepth) && all(prevPixel >= -0.5) &&
                all(prevPixel <= (float2)lgViewSize() + 0.5))
            {
                Texture2D<float> prevProbeDepth = ResourceDescriptorHeap[P[1].y];
                Texture2D<float4> prevProbePosition = ResourceDescriptorHeap[P[1].z];
                Texture2D<float4> prevRadiance = ResourceDescriptorHeap[P[1].w];
                const float2 tileCoord = (prevPixel - (float2)lgTileJitter(P[2].x) - 0.5) / (float)lgTile();
                const float4 plane = float4(normal, dot(position, normal));
                float3 sum = 0;
                float weight = 0;
                [unroll] for (uint c = 0; c < 4; ++c)
                {
                    const uint2 t = (uint2)clamp(tileCoord + float2(c & 1u, c >> 1), 0.0, (float2)lgProbeViewSize() - 1.0);
                    if (!(prevProbeDepth[t] > 0)) continue;
                    if (!(lgPlaneWeight(plane, depth, prevProbePosition[t].xyz, LG_DEPTH_WEIGHT) > 0.1)) continue;
                    sum += prevRadiance[t * LG_GATHER_RES + thread.xy].rgb;
                    weight += 1;
                }
                if (weight > 0) lighting = sum / weight / max(asfloat(P[2].y), 1e-6);
                transparency = 1 - saturate(weight / 4.0);
            }
        }
        // What the history does not cover: the radiance cache in the texel's direction (x exposure, as the history), where
        // its 8 probes around the probe exist; else the texel is uniform (Unreal: lighting = 1 without coverage).
        if (transparency > 0)
        {
            LrcCoverage coverage = (LrcCoverage)0;
            if (P[3].x != 0xFFFFFFFFu) coverage = lgRcCoverageAt(lrcParams(P[3].x), P[3].y, probe, probePosition[atlas].xyz, lgRcDither(atlas), lgRcPrepared());
            if (coverage.valid)
                lighting += lgRcSample(lrcParams(P[3].x), P[3].y, P[3].z, 0xFFFFFFFFu, coverage, probe, probePosition[atlas].xyz,
                                      lgSphere((float2(thread.xy) + lgTexelCentre(lgTileOfPixel(lgProbePixel(adaptive, probe)))) / (float)LG_TRACE_RES),
                                      probePosition[atlas].xyz, lgRcPrepared()).rgb * (g_exposure * transparency);
            else lighting = 1;
        }
        value = dot(lighting, float3(0.2126, 0.7152, 0.0722));
    }
    gs_value[index] = value;
    GroupMemoryBarrierWithGroupSync();
    if (!live) return;
    float sum = 0;
    [loop] for (uint i = 0; i < 64; ++i) sum += gs_value[i];
    RWTexture2D<float> output = ResourceDescriptorHeap[P[1].x];
    output[atlas * LG_TRACE_RES + thread.xy] = value / max(sum, 1e-4);
}
