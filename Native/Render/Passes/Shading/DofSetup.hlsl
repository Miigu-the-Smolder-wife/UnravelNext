// unx-kernel: cs_6_6 main
// Depth of field setup (A5, FEATURES_GAME 4.1; DepthOfField.cpp). One group per 32 px tile; the pixels are read and
// written one per thread and step (coalesced) into shared memory, then each thread sums its 4 x 4 block:
//   - every pixel's signed circle-of-confusion radius rho = k0 - k1 * device depth (px; k0 = f_px A / (2 z_f),
//     k1 = f_px A / (2 near): reversed Z, 1/z = device / near; the sky has rho = k0), clamped to +-DOF_MAX_RADIUS (clamped
//     pixels counted in the statistics' word 1);
//   - per tile and radius octave the largest |rho| (+1; 0: no pixel of that octave);
//   - the octave pyramid (DofCommon.hlsli): octave c's pixels over their level-c texel, for c = 1..5 written as the
//     texel's A and S; octaves 6 and 7 as the tile's sums (DofDown.hlsl): per octave rgb, count, sum rho, sum rho^2,
//     sum l, sum l x, sum l y, sum l (x^2 + y^2 + 1/6) (l = dofWeight, x y = pixel centre - tile origin), 10 floats.
// P[0] = { depth SRV, colour SRV, coc UAV (R32F), tile maxima UAV (raw, 8 floats per tile) }
// P[1] = { width, height, tilesX, statistics UAV (raw) }, P[2] = { asfloat k0, asfloat k1, tile sums UAV (raw, 20 floats), 0 }
// P[3] = { A1, A2, A3, A4 }, P[4] = { A5, S1, S2, S3 }, P[5] = { S4, S5, 0, 0 } (level-c UAVs, RGBA16F)
#include "Bindless.hlsli"
#include "Passes/Shading/DofCommon.hlsli"

struct Sums
{
    float4 colour;  // rgb, count
    float2 rho;     // sum rho, sum rho^2
    float4 moment;  // sum l, sum l x, sum l y, sum l (x^2 + y^2 + 1/6)
};
Sums zeroSums()
{
    Sums s;
    s.colour = 0;
    s.rho = 0;
    s.moment = 0;
    return s;
}
Sums addSums(Sums a, Sums b)
{
    a.colour += b.colour;
    a.rho += b.rho;
    a.moment += b.moment;
    return a;
}

groupshared uint s_max[DOF_CATEGORIES];
groupshared float s_pixelRho[DOF_TILE * DOF_TILE];     // the tile's radii (NaN outside the image)
groupshared float3 s_pixelColour[DOF_TILE * DOF_TILE];
groupshared float4 s_colour[5][64];  // octaves 3..7
groupshared float2 s_rho[5][64];
groupshared float4 s_moment[5][64];

uint uavA(uint c) { return c == 1 ? P[3].x : c == 2 ? P[3].y : c == 3 ? P[3].z : c == 4 ? P[3].w : P[4].x; }
uint uavS(uint c) { return c == 1 ? P[4].y : c == 2 ? P[4].z : c == 3 ? P[4].w : c == 4 ? P[5].x : P[5].y; }

// Level-c texel 'texel' of the tile at 'origin' (px) from its octave's sums (moments relative to the tile origin).
void writeTexel(uint c, uint2 texel, uint2 origin, Sums s)
{
    const uint2 size = (P[1].xy + (1u << c) - 1) >> c;
    if (any(texel >= size)) return;
    const float scale = (float)(1u << c), inv = 1.0f / (scale * scale);
    RWTexture2D<float4> a = ResourceDescriptorHeap[uavA(c)];
    RWTexture2D<float4> shape = ResourceDescriptorHeap[uavS(c)];
    a[texel] = float4(s.colour.rgb * inv, s.colour.w * inv);
    if (s.colour.w == 0)
    {
        shape[texel] = 0;
        return;
    }
    const float2 mean = s.moment.yz / s.moment.x, centre = (float2(texel) + 0.5f) * scale - float2(origin);
    const float variance = max(s.moment.w / s.moment.x - dot(mean, mean), 1.0f / 6.0f);
    const float rhoMean = s.rho.x / s.colour.w, rhoVariance = max(s.rho.y / s.colour.w - rhoMean * rhoMean, 0.0f);
    // spread: the positions' (a uniform square of side a has 2D variance a^2 / 6) and the radii's (width sqrt(12 var))
    shape[texel] = float4(mean - centre, sqrt(6.0f * variance + 12.0f * rhoVariance), s.rho.x * inv);
}

[numthreads(8, 8, 1)]
void main(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
    if (gi < DOF_CATEGORIES) s_max[gi] = 0;
    Texture2D<float> depth = ResourceDescriptorHeap[P[0].x];
    Texture2D<float4> colour = ResourceDescriptorHeap[P[0].y];
    RWTexture2D<float> coc = ResourceDescriptorHeap[P[0].z];
    const uint2 size = P[1].xy;
    const float k0 = asfloat(P[2].x), k1 = asfloat(P[2].y);
    const uint2 origin = gid.xy * DOF_TILE, block = origin + tid.xy * 4;

    // 1. one pixel per thread and step (8 x 8 threads over the tile's 4 x 4 steps): rho to the image and to shared memory
    uint clamped = 0;
    for (uint step = 0; step < 16; ++step)
    {
        const uint2 local = uint2(step & 3, step >> 2) * 8 + tid.xy, px = origin + local;
        float rho = asfloat(0x7FC00000u);  // (outside the image: NaN, no octave)
        float3 l = 0;
        if (all(px < size))
        {
            rho = k0 - k1 * depth.Load(int3(px, 0));
            if (!(abs(rho) <= DOF_MAX_RADIUS))
            {
                rho = clamp(rho, -DOF_MAX_RADIUS, DOF_MAX_RADIUS);  // (NaN depth: none in V's buffer)
                ++clamped;
            }
            coc[px] = rho;
            l = colour.Load(int3(px, 0)).rgb;
        }
        s_pixelRho[local.y * DOF_TILE + local.x] = rho;
        s_pixelColour[local.y * DOF_TILE + local.x] = l;
    }
    GroupMemoryBarrierWithGroupSync();

    // 2. the thread's 4 x 4 block from shared memory: octave maxima and sums
    float localMax[DOF_CATEGORIES];
    Sums sums[DOF_CATEGORIES];  // octaves 2..7 of this thread's 4 x 4 block (0, 1 unused)
    [unroll] for (uint k = 0; k < DOF_CATEGORIES; ++k)
    {
        localMax[k] = 0;
        sums[k] = zeroSums();
    }
    for (uint sb = 0; sb < 4; ++sb)
    {
        const uint2 sub = block + uint2(sb & 1, sb >> 1) * 2;
        Sums s1 = zeroSums();
        for (uint j = 0; j < 4; ++j)
        {
            const uint2 local = sub - origin + uint2(j & 1, j >> 1);
            const float rho = s_pixelRho[local.y * DOF_TILE + local.x];
            if (!(abs(rho) <= DOF_MAX_RADIUS)) continue;  // outside the image
            const uint cat = dofCategory(rho);
            [unroll] for (uint k = 0; k < DOF_CATEGORIES; ++k)
                if (cat == k) localMax[k] = max(localMax[k], abs(rho) + 1.0f);
            if (cat == 0) continue;
            Sums p;
            p.colour = float4(s_pixelColour[local.y * DOF_TILE + local.x], 1);
            p.rho = float2(rho, rho * rho);
            const float l = dofWeight(p.colour.rgb);
            const float2 xy = float2(local) + 0.5f;
            p.moment = float4(l, l * xy, l * (dot(xy, xy) + 1.0f / 6.0f));
            if (cat == 1) s1 = addSums(s1, p);
            [unroll] for (uint k2 = 2; k2 < DOF_CATEGORIES; ++k2)
                if (cat == k2) sums[k2] = addSums(sums[k2], p);
        }
        writeTexel(1, sub >> 1, origin, s1);
    }
    writeTexel(2, block >> 2, origin, sums[2]);
    [unroll] for (uint k3 = 3; k3 < DOF_CATEGORIES; ++k3)
    {
        s_colour[k3 - 3][gi] = sums[k3].colour;
        s_rho[k3 - 3][gi] = sums[k3].rho;
        s_moment[k3 - 3][gi] = sums[k3].moment;
    }
    [unroll] for (uint k4 = 0; k4 < DOF_CATEGORIES; ++k4)
        if (localMax[k4] > 0) InterlockedMax(s_max[k4], asuint(localMax[k4]));
    if (clamped)
    {
        RWByteAddressBuffer stats = ResourceDescriptorHeap[P[1].w];
        stats.InterlockedAdd(4, clamped);
    }
    GroupMemoryBarrierWithGroupSync();

    const uint tile = gid.y * P[1].z + gid.x;
    if (gi < DOF_CATEGORIES)
    {
        RWByteAddressBuffer maxima = ResourceDescriptorHeap[P[0].w];
        maxima.Store(4 * (tile * DOF_CATEGORIES + gi), s_max[gi]);
    }
    if (all((tid.xy & 1) == 0))  // octave 3: 2 x 2 threads = one 8 px texel
    {
        Sums s = zeroSums();
        [unroll] for (uint j = 0; j < 4; ++j)
        {
            const uint t = gi + (j & 1) + (j >> 1) * 8;
            s.colour += s_colour[0][t];
            s.rho += s_rho[0][t];
            s.moment += s_moment[0][t];
        }
        writeTexel(3, block >> 3, origin, s);
    }
    if (all((tid.xy & 3) == 0))  // octave 4: 4 x 4 threads = one 16 px texel
    {
        Sums s = zeroSums();
        for (uint j = 0; j < 16; ++j)
        {
            const uint t = gi + (j & 3) + (j >> 2) * 8;
            s.colour += s_colour[1][t];
            s.rho += s_rho[1][t];
            s.moment += s_moment[1][t];
        }
        writeTexel(4, block >> 4, origin, s);
    }
    if (gi < 3)  // octave 5: the tile's texel; octaves 6, 7: the tile's sums
    {
        const uint k = gi + 2;
        Sums s = zeroSums();
        for (uint t = 0; t < 64; ++t)
        {
            s.colour += s_colour[k][t];
            s.rho += s_rho[k][t];
            s.moment += s_moment[k][t];
        }
        if (gi == 0) writeTexel(5, gid.xy, origin, s);
        else
        {
            RWByteAddressBuffer tileSums = ResourceDescriptorHeap[P[2].z];
            const uint at = 80 * tile + 40 * (gi - 1);
            tileSums.Store4(at, asuint(s.colour));
            tileSums.Store2(at + 16, asuint(s.rho));
            tileSums.Store4(at + 24, asuint(s.moment));
        }
    }
}
