// unx-kernel: cs_6_6 main
// unx-variants: STEP=0,1
// Image-based lens flares (shading.post_lens_flare; Post.cpp; the reference's PostProcessLensFlares): light reflected
// between a lens's elements arrives again as ghosts - images of the bright parts of the picture, mirrored through the
// centre or not, each at its own scale and tint, every point spread to the aperture's shape.
//   STEP=0  the bright parts spread to the aperture's shape: per texel of the bloom chain's 1/8 level, the mean over a
//           disc (or a polygon of P[1].z blades) of the texels whose r + g + b reaches the threshold - 91 taps on 5
//           rings, each ring's count by its circumference. The reference draws a quad with a bokeh texture per bright
//           pixel; the mean over the shape is the same sum gathered. The source fades towards the picture's corners
//           (the reference's disc mask at half scale).
//   STEP=1  the ghosts: per quarter-resolution pixel p, the sum over the 8 flares of tint_i x the spread image at
//           centre + (p - centre) / scale_i (a negative scale mirrors), faded by two discs over the screen (a flare
//           does not reach the border), plus the halo (ours, off by default: the mirrored image pulled towards the
//           centre by the ring's radius, inside a ring window), x the intensity and tint.
// STEP=0: P[0] = { source SRV (RGBA16F, exposed linear), spread UAV (RGBA16F), width, height },
//         P[1] = { asuint(threshold), asuint(radius, texels), blades (0: a disc), 0 }
// STEP=1: P[0] = { spread SRV, flare UAV (RGBA16F), width, height }, P[1] = { asuint(intensity x tint r), g, b,
//         asuint(halo intensity) }, P[2..9] = the flares' { asuint(tint r), g, b, asuint(scale) } (scale 0: none)
#include "Bindless.hlsli"

float discMask(float2 ndc)
{
    const float x = saturate(1.0 - dot(ndc, ndc));
    return x * x;
}

#if STEP == 0
#define RINGS 5

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    const uint2 size = P[0].zw;
    if (any(id >= size)) return;
    Texture2D<float4> source = ResourceDescriptorHeap[P[0].x];
    RWTexture2D<float4> spread = ResourceDescriptorHeap[P[0].y];
    const float threshold = asfloat(P[1].x), radius = asfloat(P[1].y);
    const float blades = (float)P[1].z;
    const float2 texel = 1.0 / float2(size), uv = (float2(id) + 0.5) * texel;
    float3 sum = 0;
    [unroll] for (uint ring = 0; ring <= RINGS; ++ring)
    {
        const uint count = ring == 0 ? 1u : 6u * ring;
        [loop] for (uint k = 0; k < count; ++k)
        {
            const float angle = 6.2831853 * ((float)k + 0.5 * (float)(ring & 1u)) / (float)count;
            float reach = radius * (float)ring / (float)RINGS;
            if (blades >= 3)
            {
                // the polygon's edge along this direction (its corners on the circle)
                const float sector = 6.2831853 / blades;
                reach *= cos(0.5 * sector) / cos(angle - sector * floor(angle / sector) - 0.5 * sector);
            }
            const float2 at = uv + float2(cos(angle), sin(angle)) * reach * texel;
            if (any(at < 0) || any(at > 1)) continue;
            const float3 c = source.SampleLevel(g_linearClamp, at, 0).rgb;
            if (!all(isfinite(c)) || c.r + c.g + c.b < threshold) continue;
            sum += max(c, 0.0) * discMask(at - 0.5);  // (the reference's mask of the source at half scale: ndc / 2)
        }
    }
    spread[id] = float4(sum / (float)(1 + 3 * RINGS * (RINGS + 1)), 0);
}
#else
[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    const uint2 size = P[0].zw;
    if (any(id >= size)) return;
    Texture2D<float4> spread = ResourceDescriptorHeap[P[0].x];
    RWTexture2D<float4> flare = ResourceDescriptorHeap[P[0].y];
    const float2 ndc = (float2(id) + 0.5) / float2(size) * 2.0 - 1.0;
    float3 sum = 0;
    [unroll] for (uint i = 0; i < 8; ++i)
    {
        const float4 tint = asfloat(P[2 + i]);
        if (abs(tint.w) < 1e-3) continue;
        const float2 from = ndc / tint.w;
        if (any(abs(from) > 1)) continue;
        sum += tint.rgb * spread.SampleLevel(g_linearClamp, from * 0.5 + 0.5, 0).rgb;
    }
    const float halo = asfloat(P[1].w);
    if (halo > 0)
    {
        // a ring: what lies on the far side of the centre, drawn in by the ring's radius (in half heights)
        const float2 aspect = float2((float)size.x / (float)size.y, 1.0);
        const float2 mirrored = -ndc * aspect;
        const float away = length(mirrored);
        if (away > 1e-4)
        {
            const float ringRadius = 0.6, ringWidth = 0.25;
            const float2 from = (mirrored - mirrored / away * ringRadius) / aspect;
            const float window = saturate(1.0 - abs(away - ringRadius) / ringWidth);
            if (all(abs(from) <= 1)) sum += halo * window * window * spread.SampleLevel(g_linearClamp, from * 0.5 + 0.5, 0).rgb;
        }
    }
    flare[id] = float4(sum * asfloat(P[1].xyz) * discMask(ndc) * discMask(ndc * 0.8), 0);
}
#endif
