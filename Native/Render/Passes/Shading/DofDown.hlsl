// unx-kernel: cs_6_6 main
// Depth of field (A5, FEATURES_GAME 4.1; DepthOfField.cpp): the pyramid texels of radius octaves 6 and 7 (64 and 128 px)
// from DofSetup's per-tile sums (32 px tiles, 80 B each: octave 6 at bytes 0..39, octave 7 at 40..79 - rgb, count, sum rho, sum rho^2,
// sum l, sum l x, sum l y, sum l (x^2 + y^2 + 1/6), moments relative to the tile's origin). A thread per level-6 texel
// (2 x 2 tiles); the threads at even level-6 coordinates also write their level-7 texel (4 x 4 tiles).
// P[0] = { tile sums SRV (raw), A6 UAV, S6 UAV, A7 UAV }, P[1] = { S7 UAV, tilesX, tilesY, 0 }, P[2] = { width, height, 0, 0 }
#include "Bindless.hlsli"
#include "Passes/Shading/DofCommon.hlsli"

// The octave's sums over n x n tiles from 'first', moments moved to the texel's origin (the first tile's origin).
void sumTiles(ByteAddressBuffer sums, uint2 first, uint n, uint octave, out float4 colour, out float2 rho, out float4 moment)
{
    colour = 0;
    rho = 0;
    moment = 0;
    for (uint j = 0; j < n * n; ++j)
    {
        const uint2 d = uint2(j % n, j / n), t = first + d;
        if (any(t >= P[1].yz)) continue;
        const uint at = 80 * (t.y * P[1].y + t.x) + 40 * (octave - 6);
        colour += asfloat(sums.Load4(at));
        rho += asfloat(sums.Load2(at + 16));
        const float4 m = asfloat(sums.Load4(at + 24));
        const float2 shift = float2(d) * (float)DOF_TILE;
        moment += float4(m.x, m.yz + shift * m.x, m.w + 2.0f * dot(shift, m.yz) + dot(shift, shift) * m.x);
    }
}

void writeTexel(uint uavA, uint uavS, uint2 texel, float scale, float4 colour, float2 rho, float4 moment)
{
    RWTexture2D<float4> a = ResourceDescriptorHeap[uavA];
    RWTexture2D<float4> shape = ResourceDescriptorHeap[uavS];
    const float inv = 1.0f / (scale * scale);
    a[texel] = float4(colour.rgb * inv, colour.w * inv);
    if (colour.w == 0)
    {
        shape[texel] = 0;
        return;
    }
    const float2 mean = moment.yz / moment.x;
    const float variance = max(moment.w / moment.x - dot(mean, mean), 1.0f / 6.0f);
    const float rhoMean = rho.x / colour.w, rhoVariance = max(rho.y / colour.w - rhoMean * rhoMean, 0.0f);
    shape[texel] = float4(mean - 0.5f * scale, sqrt(6.0f * variance + 12.0f * rhoVariance), rho.x * inv);
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    const uint2 size6 = (P[2].xy + 63u) >> 6, size7 = (P[2].xy + 127u) >> 7;
    if (any(id >= size6)) return;
    ByteAddressBuffer sums = ResourceDescriptorHeap[P[0].x];
    float4 colour, moment;
    float2 rho;
    sumTiles(sums, id * 2, 2, 6, colour, rho, moment);
    writeTexel(P[0].y, P[0].z, id, 64.0f, colour, rho, moment);
    if (any(id & 1) || any((id >> 1) >= size7)) return;
    sumTiles(sums, id * 2, 4, 7, colour, rho, moment);
    writeTexel(P[0].w, P[1].x, id >> 1, 128.0f, colour, rho, moment);
}
