// unx-kernel: cs_6_6 main
// unx-variants: LAYER=0,1,2,3
// Diaphragm depth of field, a layer's gather at half resolution (DiaphragmDof.cpp; the reference's DOFGatherPass.usf
// with DOFGatherKernel.ush and DOFGatherAccumulator.ush). One group per radius tile (DdofTiles.hlsl): the tile says
// whether the layer has anything here (DdofCommon.hlsli ddofSuggest), and the widest radius in reach is the kernel's.
// The kernel: the centre and P[3].x rings; ring i (from 1) has 8 i samples at i x radius / (rings + 0.5), as mirrored
// pairs (a sample and its opposite through the centre) - on circles, or along the diaphragm's outline (the gather
// table of DdofBokehLut.hlsl, turned half a turn for the background); the centre is shifted by an interleaved gradient
// noise of the pixel within one ring spacing, so neighbouring pixels sample between each other's rings (the post
// filter and the upscaler's accumulation take the noise). A sample counts by how far its own disc reaches the centre:
// saturate((|radius| - its distance) x sharpness + 0.5), and by 1 / (its disc's area) (its energy spread over it).
//   LAYER=0 foreground    two layers by the sample's radius against the tile's nearest one (Jimenez 2014): the nearer
//                         layer's opacity is its samples' share of the kernel's area. Of a mirrored pair, the sample
//                         behind takes the nearer one's radius and reach: what a foreground disc covers on one side it
//                         covers on the other (the hidden side is filled from the visible one). Output: colour x
//                         alpha, alpha = the share of the kernel's samples that are foreground.
//   LAYER=1 hole filling  what is behind a blurred foreground edge: the samples that are not foreground, the output's
//                         translucency the nearest foreground sample's distance over the kernel's radius. Only in
//                         tiles where the foreground's radii differ (an edge). Output: colour x opacity, translucency.
//   LAYER=2 background    rings from the outside in, each ring a bucket (ours as the reference's ring binning): a
//                         sample belongs to the ring's bucket when its radius is the ring's, to the buckets gathered so
//                         far when it is wider; a finished bucket is laid over the earlier ones by how opaque it is (its
//                         samples that are not wider) where it is nearer (its mean radius smaller) - a near, less
//                         blurred surface occludes the wide blur behind it instead of adding to it. Where the tile
//                         holds radii much smaller than the kernel, the two outer rings are gathered again at 7/11 of
//                         the radius (up to three times), the earlier weight rescaled: the inner part of the kernel is
//                         sampled as densely as the narrow radii need. Output: colour, 1 where anything was gathered;
//                         and the mean and variance of the gathered radii (the sprites' occlusion, DdofScatter.ps.hlsl).
//   LAYER=3 slight        the radii under DDOF_RECOMBINE_COC on the pixel grid (square rings, as many as the tile's
//                         widest radius, at most 3): every sample of the disc, no noise - the stable picture the
//                         full-resolution gather is held to (DdofRecombine.hlsl). Output: colour, opacity.
// Tiles where every radius in reach is the kernel's (within 5 %) and only one side of the focus is present take the
// plain mean of the kernel's samples, bilinear, at the level whose texel is the ring spacing (LAYER 0 and 2); the
// accumulators read level 0 with the point sampler (a coarser texel would mix radii across a depth edge).
// P[0] = { level 0 SRV, level 1, level 2, level 3 } (DdofReduce.hlsl: rgb, a = radius; unused levels repeat level 0)
// P[1] = { foreground tiles SRV, background tiles SRV, output UAV (RGBA16F), radius statistics UAV (RG16F, LAYER=2) | none }
// P[2] = { half width, half height, level 0's width, height (padded) }
// P[3] = { ring count (3 .. 5), level count, gather table SRV | none (a disc), edge table SRV | none }
// P[4] = { asuint(the lens's squeeze), asuint(1 / squeeze), asuint(the Petzval box's corner radius), asuint(the
//          picture's width / height) }, P[5] = { asuint(Petzval amount) (0: none), asuint(falloff power), asuint(box half
//          extents x), asuint(y) } (DdofCommon.hlsli ddofPetzval)
// The lens's shape of the kernel (the reference's CocInvSqueeze and ApplyApproxPetzval): the wide layers' sample
// positions are x / squeeze, then squashed by the Petzval matrix of the pixel (one matrix for the kernel: its samples'
// own bokehs are taken as centred where the kernel is); a sample's distance stays its ring's. The slight layer's
// samples stay on the pixel grid and are measured on the lens (x * squeeze), without the Petzval matrix, as the
// full-resolution gather they bound.
#include "Bindless.hlsli"
#include "Passes/Shading/DdofCommon.hlsli"

#ifndef LAYER
#define LAYER 0
#endif
#define NONE 0xFFFFFFFFu
#define MAX_RINGS 5u
#if LAYER == DDOF_BACKGROUND
#define MIRROR -1
#else
#define MIRROR 1
#endif

struct Kernel
{
    float2 centre;    // level-0 pixels
    float radius;
    float spacing;    // between rings, level-0 pixels
    float sharpness;  // of a sample's reach: 1 / the level's texel
    uint rings, level;
    bool bilinear;
    float4 petzval;   // lens offsets to the picture (DdofCommon.hlsli ddofPetzval)
};
struct Tap
{
    float3 colour;
    float coc;
    float away;  // from the kernel's centre
    float hit;   // how far its disc reaches the centre, 0 .. 1
};

float gradientNoise(float2 pixel, float index)
{
    pixel += index * (float2(47, 17) * 0.695);
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

float sampleWeight(float coc) { return rcp(DDOF_PI * max(coc * coc, 0.0625)); }  // (a disc no smaller than a full-resolution pixel)

float reachOf(Kernel k, float coc, float away)
{
#if LAYER == DDOF_SLIGHT
    return saturate((abs(coc) - away) * 4.0 + 0.5);
#else
    return saturate((abs(coc) - away) * k.sharpness + 0.5);
#endif
}

// The kernel of radius k.radius about 'pixel': its level, ring spacing and shifted centre.
void place(inout Kernel k, float2 pixel, float2 random, uint levels, bool coarse)
{
#if LAYER == DDOF_SLIGHT
    k.level = 0;
    k.spacing = 1;
    k.centre = pixel;
#else
    const float unit = rcp((float)k.rings + 0.5);
    const float level = coarse ? floor(0.5 + log2(max(k.radius * unit, 1e-4))) : 0.0;
    k.level = (uint)clamp(level, 0.0, (float)levels - 1.0);
    float range = k.radius * unit;
    if (k.bilinear) range = max(k.radius - (float)k.rings - (float)(1u << k.level), 0.0) * unit;
    const float turn = 2.0 * DDOF_PI * random.y;
    k.centre = pixel + (0.48 * sqrt(random.x)) * float2(cos(turn), sin(turn)) * range;
    k.spacing = k.radius * unit;
#endif
    k.sharpness = rcp((float)(1u << k.level));
}

// (outside the view the picture is mirrored at its border)
Tap fetch(Kernel k, float2 position)
{
    const float2 limit = float2(P[2].xy) - 0.5 * (float)(1u << k.level);
    position = max(position, -position);
    position = min(position, 2.0 * limit - position);
    position = min(position, limit);
    const float2 uv = position / float2(P[2].zw);
    Texture2D<float4> source = ResourceDescriptorHeap[P[0][k.level]];
    float4 raw;
    if (k.bilinear) raw = source.SampleLevel(g_linearClamp, uv, 0);
    else raw = source.SampleLevel(g_pointClamp, uv, 0);
    Tap t;
    t.colour = raw.rgb;
    t.coc = raw.a;
    t.away = 0;
    t.hit = 1;
    return t;
}

// The two samples of pair (batch, quarter) of ring 'ring' (from 1): batch 0 .. 2 ring - 1, quarter 0 or 1 (a quarter
// turn on), each with its mirrored sample - 8 ring samples.
void pairOf(Kernel k, uint ring, uint batch, uint quarter, out float2 a, out float2 b)
{
    int2 square = batch < ring ? int2(ring, batch) : int2(2 * ring - batch, ring);  // (on the square ring)
    if (quarter == 1) square = int2(-square.y, square.x);
#if LAYER == DDOF_SLIGHT
    a = float2(square);
    b = -a;
#else
    if (P[3].z != NONE)
    {
        Texture2D<float4> table = ResourceDescriptorHeap[P[3].z];
        a = k.spacing * table.Load(int3(DDOF_LUT / 2 + MIRROR * square, 0)).xy;
        b = k.spacing * table.Load(int3(DDOF_LUT / 2 - MIRROR * square, 0)).xy;
    }
    else
    {
        // (the rings are turned against each other by half a sample: no two rings' samples line up)
        const float angle = DDOF_PI * ((float)batch + 1.0 + ((ring & 1) ? 0.0 : 0.5)) / (float)(4 * ring);
        float sn, cs;
        sincos(angle, sn, cs);
        const float2 direction = quarter == 1 ? float2(-sn, cs) : float2(cs, sn);
        a = direction * ((float)ring * k.spacing);
        b = -a;
    }
    a.x *= asfloat(P[4].y);
    b.x *= asfloat(P[4].y);
    a = ddofTransform(k.petzval, a);
    b = ddofTransform(k.petzval, b);
#endif
}

#if LAYER == DDOF_FOREGROUND
struct Accumulator
{
    float3 front, back;  // the layer nearest the camera and the one behind it
    float frontWeight, backWeight;
    float foreground;    // the samples that are foreground, whether their discs reach the centre or not
    float closest;       // the tile's nearest radius
};
float fading(float coc) { return saturate(-coc - (DDOF_RECOMBINE_COC - 1.0)); }  // (the slight foreground is the recombine's)
void add(inout Accumulator a, Tap t, float weight)
{
    const float hit = (t.coc < 0.0 ? t.hit : 0.0) * fading(t.coc);
    float behind = saturate((t.coc - a.closest) * 0.5);
    behind = behind * behind * (3.0 - 2.0 * behind);
    a.back += t.colour * (behind * hit * weight);
    a.backWeight += behind * hit * weight;
    a.front += t.colour * ((1.0 - behind) * hit * weight);
    a.frontWeight += (1.0 - behind) * hit * weight;
    a.foreground += t.coc < 0.0 ? fading(t.coc) : 0.0;
}
void addCentre(inout Accumulator a, Tap t)
{
    t.hit *= fading(t.coc);
    add(a, t, sampleWeight(t.coc));
}
void addPair(inout Accumulator a, Tap s, Tap m)
{
    float ws = sampleWeight(s.coc), wm = sampleWeight(m.coc);
    s.hit *= fading(s.coc);
    m.hit *= fading(m.coc);
    if (m.coc > s.coc)
    {
        m.hit = s.hit;
        m.coc = s.coc;
        wm = ws;
    }
    else
    {
        s.hit = m.hit;
        s.coc = m.coc;
        ws = wm;
    }
    add(a, s, ws);
    add(a, m, wm);
}
#elif LAYER == DDOF_HOLE_FILLING
struct Accumulator
{
    float3 colour;
    float weight;
    float nearestForeground;  // the distance of the nearest foreground sample
};
void add(inout Accumulator a, Tap t)
{
    if (t.coc < -(DDOF_RECOMBINE_COC - 1.0)) a.nearestForeground = min(a.nearestForeground, t.away);
    else
    {
        a.colour += t.colour * t.hit;
        a.weight += t.hit;
    }
}
void addCentre(inout Accumulator a, Tap t) { add(a, t); }
void addPair(inout Accumulator a, Tap s, Tap m)
{
    if (s.coc < 0.0 && m.coc > s.coc) m.hit = max(m.hit, s.hit);
    else if (m.coc < 0.0 && s.coc > m.coc) s.hit = max(m.hit, s.hit);
    add(a, s);
    add(a, m);
}
#elif LAYER == DDOF_SLIGHT
struct Accumulator
{
    float3 colour;
    float weight;
    float opacity, opacityWeight;
};
void add(inout Accumulator a, Tap t, float weight)
{
    const float w = t.hit * weight * saturate(DDOF_RECOMBINE_COC - abs(t.coc));
    a.colour += t.colour * w;
    a.weight += w;
    a.opacity += t.hit * saturate(DDOF_RECOMBINE_COC - t.coc);
    a.opacityWeight += t.hit;
}
void addCentre(inout Accumulator a, Tap t) { add(a, t, sampleWeight(t.coc)); }
void addPair(inout Accumulator a, Tap s, Tap m)
{
    float ws = sampleWeight(s.coc), wm = sampleWeight(m.coc);
    if (m.coc > s.coc)
    {
        m.hit = s.hit;
        wm = ws;
    }
    else
    {
        s.hit = m.hit;
        ws = wm;
    }
    add(a, s, ws);
    add(a, m, wm);
}
#else
struct Accumulator
{
    // the buckets gathered so far, composed; and the ring being gathered
    float3 earlierColour;
    float earlierCoc, earlierCocSquare, earlierWeight;
    float3 ringColour;
    float ringCoc, ringCocSquare, ringWeight, ringTranslucency;
    float bordering;  // the radius between this ring's bucket and the earlier ones
    bool first;
};
void add(inout Accumulator a, Tap t, float weight)
{
    const float w = (t.coc < 0.0 ? 0.0 : t.hit) * saturate(abs(t.coc) - (DDOF_RECOMBINE_COC - 2.0)) * weight;
    float wider = saturate(t.coc - a.bordering + 0.5);
    wider = a.first ? 0.0 : wider * wider * (3.0 - 2.0 * wider);  // (the first ring: all of it is its own bucket)
    const float mine = w * (1.0 - wider), theirs = w * wider;
    a.ringColour += t.colour * mine;
    a.ringCoc += t.coc * mine;
    a.ringCocSquare += t.coc * t.coc * mine;
    a.ringWeight += mine;
    a.earlierColour += t.colour * theirs;
    a.earlierCoc += t.coc * theirs;
    a.earlierCocSquare += t.coc * t.coc * theirs;
    a.earlierWeight += theirs;
    a.ringTranslucency += saturate(t.coc - a.bordering);
}
void addPair(inout Accumulator a, Tap s, Tap m)
{
    add(a, s, sampleWeight(s.coc));
    add(a, m, sampleWeight(m.coc));
}
// The ring's bucket over the earlier ones: additive where they are the same radius, covering where it is nearer.
void compose(inout Accumulator a, float samples)
{
    if (a.ringWeight == 0.0) return;
    const float opacity = saturate(1.0 - a.ringTranslucency / samples);
    const float occluding = saturate(a.earlierCoc * ddofRcp(a.earlierWeight) - a.ringCoc / a.ringWeight);
    const float keep = a.earlierWeight == 0.0 ? 0.0 : 1.0 - opacity * occluding;
    a.earlierColour = a.earlierColour * keep + a.ringColour;
    a.earlierCoc = a.earlierCoc * keep + a.ringCoc;
    a.earlierCocSquare = a.earlierCocSquare * keep + a.ringCocSquare;
    a.earlierWeight = a.earlierWeight * keep + a.ringWeight;
}
void endRing(inout Accumulator a, float samples)
{
    if (a.first)
    {
        a.earlierColour = a.ringColour;
        a.earlierCoc = a.ringCoc;
        a.earlierCocSquare = a.ringCocSquare;
        a.earlierWeight = a.ringWeight;
    }
    else compose(a, samples);
    a.ringColour = 0;
    a.ringCoc = a.ringCocSquare = a.ringWeight = a.ringTranslucency = 0;
}
#endif

void gatherRing(inout Accumulator a, Kernel k, uint ring)
{
    for (uint batch = 0; batch < 2 * ring; ++batch)
        for (uint quarter = 0; quarter < 2; ++quarter)
        {
            float2 oa, ob;
            pairOf(k, ring, batch, quarter, oa, ob);
#if LAYER == DDOF_SLIGHT
            const float2 onLens = oa * float2(asfloat(P[4].x), 1.0);
            if (dot(onLens, onLens) > ((float)k.rings + 0.5) * ((float)k.rings + 0.5)) continue;  // (outside the disc)
            const float away = length(onLens);
#else
            const float away = (float)ring * k.spacing;
#endif
            Tap s = fetch(k, k.centre + oa), m = fetch(k, k.centre + ob);
#if LAYER == DDOF_SLIGHT
            s.coc *= ddofEdgeFactor(P[3].w, onLens);
            m.coc *= ddofEdgeFactor(P[3].w, -onLens);
#endif
            s.away = m.away = away;
            s.hit = reachOf(k, s.coc, away);
            m.hit = reachOf(k, m.coc, away);
            addPair(a, s, m);
        }
}

#if LAYER == DDOF_FOREGROUND || LAYER == DDOF_BACKGROUND
float3 meanOf(Kernel k)
{
    float3 sum = fetch(k, k.centre).colour;
    for (uint ring = 1; ring <= k.rings; ++ring)
        for (uint batch = 0; batch < 2 * ring; ++batch)
            for (uint quarter = 0; quarter < 2; ++quarter)
            {
                float2 oa, ob;
                pairOf(k, ring, batch, quarter, oa, ob);
                sum += fetch(k, k.centre + oa).colour + fetch(k, k.centre + ob).colour;
            }
    return sum / (float)(1u + 4u * k.rings * (k.rings + 1u));
}
#endif

#if LAYER == DDOF_BACKGROUND
void gatherBucket(inout Accumulator a, Kernel k, uint ring, bool first)
{
    a.first = first;
    a.bordering = ((float)ring + 1.5) * (k.radius / ((float)k.rings + 0.5));  // (a ring out, the kernel's sampling error over it)
    gatherRing(a, k, ring);
    endRing(a, 8.0 * (float)ring);
}
#endif

[numthreads(DDOF_TILE, DDOF_TILE, 1)]
void main(uint2 gid : SV_GroupID, uint2 id : SV_DispatchThreadID)
{
    if (any(id >= P[2].xy)) return;
    Texture2D<float4> tilesForeground = ResourceDescriptorHeap[P[1].x];
    Texture2D<float4> tilesBackground = ResourceDescriptorHeap[P[1].y];
    RWTexture2D<float4> output = ResourceDescriptorHeap[P[1].z];
    const DdofTile tile = ddofLoadTile(tilesForeground, tilesBackground, int2(gid));
    const DdofSuggestion suggestion = ddofSuggest(tile, LAYER);
    if (suggestion.skip)
    {
#if LAYER == DDOF_HOLE_FILLING
        output[id] = float4(0, 0, 0, 1);
#else
        output[id] = 0;
#endif
#if LAYER == DDOF_BACKGROUND
        if (P[1].w != NONE)
        {
            RWTexture2D<float2> statistics = ResourceDescriptorHeap[P[1].w];
            statistics[id] = float2(0, 1);
        }
#endif
        return;
    }

    Kernel k;
    k.radius = suggestion.maxAbs;
    k.rings = clamp(P[3].x, 3u, MAX_RINGS);
    k.bilinear = false;
    k.level = 0;
    k.spacing = 1;
    k.sharpness = 1;
    k.centre = 0;
    const float2 pixel = float2(id) + 0.5;
    k.petzval = ddofPetzval(pixel / float2(P[2].xy) * 2.0 - 1.0, asfloat(P[5]), asfloat(P[4].zw), true);
    const float2 random = float2(gradientNoise(float2(id), 0), gradientNoise(float2(id), 1));
    const uint levels = max(P[3].y, 1u);

#if LAYER == DDOF_FOREGROUND || LAYER == DDOF_BACKGROUND
    if (suggestion.plain)
    {
        k.bilinear = true;
        place(k, pixel, random, levels, !(tile.fgdMin < 0.0 && tile.bgdMax > 0.0));
        const float3 mean = meanOf(k);
#if LAYER == DDOF_FOREGROUND
        const float alpha = saturate(k.radius - (DDOF_RECOMBINE_COC - 1.0));
        output[id] = float4(mean * alpha, alpha);
#else
        output[id] = float4(mean, 1);
        if (P[1].w != NONE)
        {
            RWTexture2D<float2> statistics = ResourceDescriptorHeap[P[1].w];
            statistics[id] = float2(k.radius, 1);
        }
#endif
        return;
    }
#endif

#if LAYER == DDOF_FOREGROUND
    place(k, pixel, random, levels, false);
    Accumulator a;
    a.front = a.back = 0;
    a.frontWeight = a.backWeight = a.foreground = 0;
    a.closest = suggestion.closest;
    addCentre(a, fetch(k, k.centre));
    for (uint ring = 1; ring <= k.rings; ++ring) gatherRing(a, k, ring);
    // the nearer layer's opacity: its samples' share of the kernel's area
    const float total = (float)(1u + 4u * k.rings * (k.rings + 1u));
    const float kernelArea = rcp(sampleWeight(k.radius * ((float)k.rings + 0.5) / (float)k.rings));
    const float frontOpacity = saturate((a.backWeight == 0.0 ? 1.0 : 0.0) + kernelArea * a.frontWeight / total);
    const float3 colour = lerp(a.back * ddofRcp(a.backWeight), a.front * ddofRcp(a.frontWeight), frontOpacity);
    const float alpha = a.frontWeight + a.backWeight > 0.0 ? a.foreground / total : 0.0;
    output[id] = float4(colour * alpha, alpha);
#elif LAYER == DDOF_HOLE_FILLING
    place(k, pixel, random, levels, false);
    Accumulator a;
    a.colour = 0;
    a.weight = 0;
    a.nearestForeground = DDOF_LARGE_COC;
    addCentre(a, fetch(k, k.centre));
    for (uint ring = 1; ring <= k.rings; ++ring) gatherRing(a, k, ring);
    float4 filled = float4(0, 0, 0, 1);
    if (a.nearestForeground != DDOF_LARGE_COC && a.weight > 0.0)
    {
        const float translucency = saturate(a.nearestForeground / k.radius);
        filled = float4(a.colour / a.weight * (1.0 - translucency), translucency);
    }
    output[id] = filled;
#elif LAYER == DDOF_SLIGHT
    k.rings = (uint)round(min(suggestion.maxAbs, DDOF_RECOMBINE_COC));
    place(k, pixel, random, levels, false);
    Accumulator a;
    a.colour = 0;
    a.weight = a.opacity = a.opacityWeight = 0;
    for (uint ring = 1; ring <= k.rings; ++ring) gatherRing(a, k, ring);
    addCentre(a, fetch(k, k.centre));
    output[id] = float4(a.colour * ddofRcp(a.weight), a.opacity * ddofRcp(a.opacityWeight));
#else
    place(k, pixel, random, levels, false);
    Accumulator a;
    a.earlierColour = a.ringColour = 0;
    a.earlierCoc = a.earlierCocSquare = a.earlierWeight = 0;
    a.ringCoc = a.ringCocSquare = a.ringWeight = a.ringTranslucency = 0;
    a.bordering = 0;
    a.first = true;
    // the outer rings; then, while the tile's narrow radii need it, the same rings of a smaller kernel; then the rest
    const uint inner = k.rings - k.rings / 2;
    const float shrink = (float)(1u + 2u * inner) / (float)(1u + 2u * k.rings);
    bool first = true;
    for (uint ring = k.rings; ring > inner; --ring)
    {
        gatherBucket(a, k, ring, first);
        first = false;
    }
    for (uint change = 0; change < 3; ++change)
    {
        const float smaller = k.radius * shrink;
        if (!(smaller > (float)k.rings && suggestion.minIntersectable < smaller + (float)(1u << k.level))) break;
        k.radius = smaller;
        const float rescale = rcp(shrink * shrink);  // (the earlier samples stood for a larger area each)
        a.earlierColour *= rescale;
        a.earlierCoc *= rescale;
        a.earlierCocSquare *= rescale;
        a.earlierWeight *= rescale;
        place(k, pixel, random, levels, false);
        for (uint again = k.rings; again > inner; --again) gatherBucket(a, k, again, false);
    }
    for (uint rest = inner; rest >= 1; --rest) gatherBucket(a, k, rest, false);
    // the centre as its own bucket
    Tap centre = fetch(k, k.centre);
    a.first = false;
    a.bordering = 1.5 * k.radius / ((float)k.rings + 0.5);
    add(a, centre, sampleWeight(centre.coc));
    compose(a, 1.0);
    const float norm = ddofRcp(a.earlierWeight);
    output[id] = float4(a.earlierColour * norm, a.earlierWeight > 0.0 ? 1.0 : 0.0);
    if (P[1].w != NONE)
    {
        const float mean = a.earlierCoc * norm;
        RWTexture2D<float2> statistics = ResourceDescriptorHeap[P[1].w];
        statistics[id] = float2(mean, max(a.earlierCocSquare * norm - mean * mean, 1.0));
    }
#endif
}
