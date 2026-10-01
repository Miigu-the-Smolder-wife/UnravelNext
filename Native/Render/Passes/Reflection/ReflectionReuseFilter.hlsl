// unx-kernel: cs_6_6 main
// The last pass of the ray-reuse reflection pipeline (ReflectionReuse.hlsli), one thread per pixel of the view: a
// bilateral filter over the accumulated value, steered by its temporal variance, and the pixel's entry in
// view.reflection (rgb = lobe radiance, a = the traced reflection's share of the specular light; ShadeOpaque mixes the K
// path's lobe radiance in for the rest).
// The filter runs where the accumulated luminance still varies in time (standard deviation over the mean above 0.5) or
// the history is young (fewer than lumen_bilateral_disocclusion_frames frames: 'young' = 1 - frames / that): 'samples'
// neighbours (twice as many when young) on a disk of radius lumen_bilateral_radius x min(8 x roughness, 1) px, weighted by
//   the pixel's tangent plane   exp2(-lumen_bilateral_depth_weight x (distance / depth)^2),
//   the normals' agreement      1 - angle / lobe half-angle (the half-angle x 4 when young),
//   the luminance difference    exp2(-difference / temporal standard deviation) (dropped when young).
// A young pixel's samples are compressed by their luminance (lumen_disocclusion_tonemap: v / (1 + young x luminance))
// before the mean, as the reference does to keep single bright samples out of newly seen areas.
// P[0] = { accumulated SRV (rgb nits, a = second moment), frames SRV, depth SRV, gbuffer SRV }
// P[1] = { reflection UAV, modes SRV, rows H, frame }
// P[2] = { width, height, samples, flags (bit 0: no filter, bit 1: no tone map on young pixels) }
// P[3] = { asuint(radius px), asuint(depth weight), asuint(young frames), asuint(max frames) }
// P[4] = { asuint(max roughness to trace), asuint(roughness fade length), asuint(tone-map range), 0 }
// Frame constants b1 = main view.
#include "Passes/Reflection/ReflectionReuse.hlsli"

float3 youngCompress(float3 v, float young) { return v / (1 + young * reuseLuminance(v)); }
float3 youngExpand(float3 v, float young) { return v / max(1 - young * reuseLuminance(v), 1e-4); }

[numthreads(8, 8, 1)]
void main(uint2 tile : SV_GroupID, uint2 local : SV_GroupThreadID)
{
    const uint2 size = P[2].xy;
    const uint2 pixel = tile * 8 + local;
    if (any(pixel >= size)) return;
    RWTexture2D<float4> reflection = ResourceDescriptorHeap[P[1].x];
    if (reflection[uint2(tile.x, P[1].z + tile.y)].a < 0.5) return;  // a tile without traced or planar pixels
    Texture2D<uint> modes = ResourceDescriptorHeap[P[1].y];
    if (reflMode(modes.Load(int3(pixel, 0))) != REFL_M) return;  // K and planar pixels: as ReflectionResolve left them
    Texture2D<float4> accumulated = ResourceDescriptorHeap[P[0].x];
    Texture2D<float4> frameCounts = ResourceDescriptorHeap[P[0].y];
    Texture2D<float> depth = ResourceDescriptorHeap[P[0].z];
    Texture2D<uint2> gbuffer = ResourceDescriptorHeap[P[0].w];
    const float range = asfloat(P[4].z);
    const ReflSurface s = reflSurface(depth, gbuffer, pixel);
    const float4 own = accumulated.Load(int3(pixel, 0));
    const float3 centre = reuseToFilter(own.rgb, range);
    const float centreLuminance = reuseLuminance(centre);
    const float deviation = sqrt(max(own.a - centreLuminance * centreLuminance, 0.0));
    const float frames = frameCounts.Load(int3(pixel, 0)).r;
    const float maxFrames = reuseMaxFrames(asfloat(P[3].w), s.roughness);
    const float youngFrames = asfloat(P[3].z);
    float young = maxFrames > 1 && youngFrames > 0 ? 1 - saturate(frames / youngFrames) : 0;
    const float compress = (P[2].w & 2u) ? 0.0 : young;
    float3 sum = youngCompress(centre, compress);
    float weight = 1;
    const float radius = asfloat(P[3].x) * saturate(s.roughness * 8.0);
    const bool wanted = deviation / max(centreLuminance, 0.1) > 0.5 || young > 0.01;
    if ((P[2].w & 1u) == 0 && radius > 1.0 && wanted)
    {
        const uint samples = (uint)(lerp((float)P[2].z, 2.0 * P[2].z, young) + 0.5);
        const float lobe = clamp((reuseLobeHalfAngle(s.roughness) + 0.01) * lerp(1.0, 4.0, young), 0.01, 1.5707963);
        const uint key = reuseHash(pixel.x + pixel.y * 65536u + (P[1].w & 7u) * 0x9E3779B9u + 0x51ED270Bu);
        const float2 shift = float2(reuseUnit(key), reuseUnit(key + 1));
        [loop] for (uint i = 0; i < samples; ++i)
        {
            const int2 q = int2(floor(float2(pixel) + 0.5 + reuseDisk(i, samples, shift) * radius));
            if (any(q < 0) || any(q >= int2(size)) || all(q == int2(pixel))) continue;
            if (reflMode(modes.Load(int3(q, 0))) != REFL_M) continue;
            const ReflSurface t = reflSurface(depth, gbuffer, uint2(q));
            if (!t.valid) continue;
            const float off = abs(dot(t.position - s.position, s.normal)) / max(s.linearDepth, 1e-4);
            const float planeWeight = exp2(-asfloat(P[3].y) * off * off);
            const float normalWeight = 1 - saturate(acos(saturate(dot(s.normal, t.normal))) / lobe);
            const float3 v = reuseToFilter(accumulated.Load(int3(q, 0)).rgb, range);
            const float luminanceWeight = lerp(exp2(-abs(centreLuminance - reuseLuminance(v)) / max(deviation, 1e-3)), 1.0, young);
            const float w = planeWeight * normalWeight * luminanceWeight;
            sum += youngCompress(v, compress) * w;
            weight += w;
        }
    }
    const float3 value = reuseFromFilter(youngExpand(sum / weight, compress), range);
    reflection[pixel] = float4(reflStorable(max(value, 0.0)), reuseTraceShare(s.roughness, asfloat(P[4].x), asfloat(P[4].y)));
}
