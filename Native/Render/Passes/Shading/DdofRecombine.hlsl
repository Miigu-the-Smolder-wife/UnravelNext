// unx-kernel: cs_6_6 main
// Diaphragm depth of field, the recombine at full resolution (DiaphragmDof.cpp; the reference's DOFRecombine.usf): the
// half-resolution layers and the full-resolution picture into the output.
//   slight      radii under DDOF_RECOMBINE_COC would lose their detail at half resolution: they are gathered here, per
//               pixel, over the full-resolution picture - P[4].w mirrored pairs on a Hammersley disc of the group's widest
//               such radius (found over the group's pixels and four taps around each; none above 1/8: no gather), a
//               sample counted by how far its disc reaches the pixel and 1 / its disc's area, the sample behind of a
//               pair taking the nearer one's reach (the hidden side filled). The result is held to the range of the
//               half-resolution gather of the same radii (DdofGather.hlsl LAYER=3) around the pixel, by 4 radius^2: the
//               jittered full-resolution samples flicker on a highlight, the half-resolution picture is stable.
//               Its opacity over the background: the samples' saturate(DDOF_RECOMBINE_COC - radius), averaged; without
//               a gather, the pixel's own saturate(2 - 4 |radius|).
//   background  the gathered background (bilinear), normalised by its validity; under a blurred foreground edge the
//               hole-filling layer is composed over it (where nothing was gathered: it alone, or the pixel itself).
//   foreground  the gathered foreground (premultiplied) over all of it.
// The half-resolution layers are on the unjittered grid when the prefilter ran (P[4].xy the frame's jitter; 0 otherwise).
// One group per 8 x 8 pixels.
// P[0] = { colour SRV (full resolution, exposed linear), depth SRV, destination UAV, edge table SRV | none }
// P[1] = { foreground SRV, hole filling SRV, slight SRV, background SRV } (half resolution, RGBA16F)
// P[2] = { width, height, half width, half height }, P[3] = the lens (4 floats, DdofCommon.hlsli)
// P[4] = { asuint(jitter x), asuint(jitter y) (full-resolution pixels), frame index, sample pairs (16; the reference's
//          lower quality 12) }, P[5] = { asuint(the stable range's widening near focus), asuint(depth blur radius),
//          asuint(depth blur exponent x near plane) (DdofCommon.hlsli ddofCoc), asuint(the lens's squeeze) }
// An anamorphic lens (P[5].w): the disc's samples are taken x / squeeze apart (the nearest pixel) and measured on the
// lens, x * squeeze of the pixel taken.
#include "Bindless.hlsli"
#include "Passes/Shading/DdofCommon.hlsli"

#define NONE 0xFFFFFFFFu
#define OPACITY_EPSILON 0.01

groupshared uint gWidest;

uint3 pcg3d16(uint3 v)
{
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    return v >> 16u;
}
float2 hammersley16(uint index, uint count, uint2 seed)
{
    return float2(frac((float)index / (float)count + (float)seed.x * (1.0 / 65536.0)), (float)((reversebits(index) >> 16) ^ seed.y) * (1.0 / 65536.0));
}
float2 concentricDisk(float2 e)
{
    float2 sf = e * sqrt(2.0) - sqrt(0.5);
    const float2 sq = sf * sf;
    const float root = sqrt(2.0 * max(sq.x, sq.y) - min(sq.x, sq.y));
    if (sq.x > sq.y) sf.x = sf.x > 0 ? root : -root;
    else sf.y = sf.y > 0 ? root : -root;
    return sf;
}

// (a disc no smaller than a full-resolution pixel: radius 1/4 of a half-resolution pixel)
float sampleWeight(float coc) { return rcp(4.0 * DDOF_PI * max(coc * coc, 0.0625)); }
float reachOf(float coc, float away)
{
    const float reach = saturate((max(abs(coc), 0.25) - away) * 4.0 + 0.5);
    return reach * reach * (3.0 - 2.0 * reach);
}
float backgroundOpacity(float coc) { return saturate(DDOF_RECOMBINE_COC - coc); }
float foregroundConsidered(float coc) { return saturate(-1.0 - 8.0 * coc) * saturate(DDOF_RECOMBINE_COC + coc); }

struct Tap
{
    float3 colour;
    float coc, hit, weight, opacity, considered;
};
Tap tapAt(int2 pixel, float away, float2 direction)
{
    Texture2D<float4> colour = ResourceDescriptorHeap[P[0].x];
    Texture2D<float> depth = ResourceDescriptorHeap[P[0].y];
    pixel = clamp(pixel, 0, int2(P[2].xy) - 1);
    Tap t;
    t.colour = colour.Load(int3(pixel, 0)).rgb;
    t.coc = ddofCoc(depth.Load(int3(pixel, 0)), asfloat(P[3]), asfloat(P[5].yz));
    t.hit = reachOf(t.coc, away / ddofEdgeFactor(P[0].w, direction));
    t.weight = sampleWeight(t.coc);
    t.opacity = backgroundOpacity(t.coc);
    t.considered = saturate(DDOF_RECOMBINE_COC - abs(t.coc));
    return t;
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID, uint index : SV_GroupIndex)
{
    const uint2 size = P[2].xy;
    const float2 halfSize = float2(P[2].zw);
    const int2 pixel = min(int2(id), int2(size) - 1);
    const float2 layerUv = min((float2(pixel) + 0.5 - asfloat(P[4].xy)) * 0.5, halfSize - 0.5) / halfSize;

    Texture2D<float4> foregroundLayer = ResourceDescriptorHeap[P[1].x];
    const float4 foreground = foregroundLayer.SampleLevel(g_linearClamp, layerUv, 0);
    const float foregroundTranslucency = 1.0 - foreground.a;

    Tap centre = tapAt(pixel, 0.0, float2(0, 0));
    centre.hit = 1;

    // the group's widest radius the full-resolution gather takes
    if (index == 0) gWidest = 0;
    GroupMemoryBarrierWithGroupSync();
    {
        Texture2D<float> depth = ResourceDescriptorHeap[P[0].y];
        float widest = abs(centre.coc) < DDOF_RECOMBINE_COC ? abs(centre.coc) : 0.0;
        [unroll] for (uint j = 0; j < 4; ++j)
        {
            const int2 at = clamp(pixel + kDdofCross[j] * (int)(2.0 * DDOF_RECOMBINE_COC), 0, int2(size) - 1);
            const float other = abs(ddofCoc(depth.Load(int3(at, 0)), asfloat(P[3]), asfloat(P[5].yz)));
            if (other < DDOF_RECOMBINE_COC) widest = max(widest, other);
        }
        InterlockedMax(gWidest, asuint(widest));  // (not negative: floats order as their bits)
    }
    GroupMemoryBarrierWithGroupSync();
    const float widestInGroup = asfloat(gWidest);
    if (any(id >= size)) return;

    float gatherOpacity = saturate(2.0 - 4.0 * abs(centre.coc));
    float3 gatherColour = centre.colour;
    const uint pairsMax = P[4].w;
    if (pairsMax != 0 && widestInGroup > 0.125 && foregroundTranslucency >= OPACITY_EPSILON)
    {
        const float fullRadius = 2.0 * widestInGroup;
        const float kernelRadius = ceil(fullRadius) + 0.5;  // (half a pixel more: the samples are whole pixels)
        const float squeeze = asfloat(P[5].w);
        const uint pairs = min(pairsMax, (uint)(DDOF_PI * fullRadius * fullRadius * 0.5 / squeeze));  // (the pixels the bokeh covers)
        const uint2 seed = pcg3d16(uint3(id, P[4].z % 8u)).xy;
        // the pixel itself always counts
        float3 colour = centre.colour * centre.weight;
        float colourWeight = centre.weight, opacity = centre.opacity, opacityWeight = 1.0;
        for (uint i = 0; i < pairs; ++i)
        {
            float2 offset = kernelRadius * concentricDisk(hammersley16(i, pairsMax, seed));
            offset.x /= squeeze;
            const int2 pixels = int2(sign(offset) * floor(abs(offset) + 0.5));
            const float2 onLens = float2(pixels) * float2(squeeze, 1.0);
            const float away = 0.5 * length(onLens);  // (in half-resolution pixels, as the radii)
            Tap s = tapAt(pixel + pixels, away, onLens), m = tapAt(pixel - pixels, away, -onLens);
            if (m.coc > s.coc)
            {
                m.hit = s.hit;
                m.weight = s.weight;
            }
            else if (s.coc > m.coc)
            {
                s.hit = m.hit;
                s.weight = m.weight;
            }
            const float ws = s.hit * s.weight * s.considered, wm = m.hit * m.weight * m.considered;
            colour += s.colour * ws + m.colour * wm;
            colourWeight += ws + wm;
            opacity += s.hit * s.opacity + m.hit * m.opacity;
            opacityWeight += s.hit + m.hit;
        }
        if (P[1].z != NONE)
        {
            // the stable range: the half-resolution gather's four texels around the pixel
            Texture2D<float4> slightLayer = ResourceDescriptorHeap[P[1].z];
            float4 lo = 0, hi = 0;
            [unroll] for (uint c = 0; c < 4; ++c)
            {
                const float2 uv = min(layerUv + 0.5 * float2(kDdofCross[c]) / halfSize, (halfSize - 0.5) / halfSize);
                const float4 stable = slightLayer.SampleLevel(g_pointClamp, uv, 0);
                lo = c == 0 ? stable : min(lo, stable);
                hi = c == 0 ? stable : max(hi, stable);
            }
            const float widen = asfloat(P[5].x) * (1.0 - saturate(centre.coc * centre.coc / 64.0));
            hi += lo * widen;
            lo -= lo * widen;
            lo.a = saturate(lo.a);
            hi.a = saturate(hi.a);
            const float hold = saturate(centre.coc * centre.coc * 4.0);
            colour = lerp(colour, clamp(colour, lo.rgb * colourWeight, hi.rgb * colourWeight), hold);
            opacity = lerp(opacity, clamp(opacity, lo.a * opacityWeight, hi.a * opacityWeight), hold);
        }
        gatherOpacity = colourWeight > 0.0 ? saturate(opacity * ddofRcp(opacityWeight)) : 0.0;
        gatherColour = colour * ddofRcp(colourWeight);
    }

    // the background, hole-filled under a foreground edge
    Texture2D<float4> backgroundLayer = ResourceDescriptorHeap[P[1].w];
    const float4 gathered = backgroundLayer.SampleLevel(g_linearClamp, layerUv, 0);
    const float validity = gathered.a;
    float3 background = gathered.rgb * ddofRcp(validity);
    Texture2D<float4> holeLayer = ResourceDescriptorHeap[P[1].y];
    float4 hole = holeLayer.SampleLevel(g_linearClamp, layerUv, 0);  // (rgb x opacity, translucency)
    if (validity <= 0.001)
    {
        if (hole.a < 1.0)
        {
            // nothing gathered behind: the hole filling is as opaque as the pixel is not foreground
            const float translucency = min(hole.a, min(1.0 - foregroundConsidered(centre.coc), validity));
            hole.rgb *= (1.0 - translucency) / (1.0 - hole.a);
            hole.a = translucency;
        }
        else gatherOpacity = 1;
    }
    background = background * hole.a + hole.rgb;

    float3 result = background * (1.0 - gatherOpacity) + gatherOpacity * gatherColour;
    result = result * foregroundTranslucency + foreground.rgb;
    Texture2D<float4> own = ResourceDescriptorHeap[P[0].x];
    RWTexture2D<float4> destination = ResourceDescriptorHeap[P[0].z];
    destination[id] = float4(all(isfinite(result)) ? max(result, 0.0) : centre.colour, own.Load(int3(pixel, 0)).a);  // (the pixel's alpha is kept)
}
