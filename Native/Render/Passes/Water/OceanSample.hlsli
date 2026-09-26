// The ocean's displaced surface at a rest position (Ocean.h fields), for every geometry consumer of track W (view grid,
// height clipmap). Where a cascade's footprint is below its texel (mip < 1), the field is reconstructed C1 across
// texels from its samples and their exact FFT derivatives (bicubic Hermite with zero twist: the displacement slopes
// and Jacobian are continuous, so a mesh finer than a texel meets no crease; hardware bilinear filtering has slope jumps
// of order 1 at the texel lines for content near the cascade's Nyquist, FEATURES_GAME 1.8 B.2), blended into the
// trilinear filter as the mip rises from 0 to 1; above, the trilinear filter. Built from Loads, the reconstruction is
// bit-identical wherever a point is evaluated again.
//   displacement (Dx, h, Dz, dDx/dz), slopes (dh/dx, dh/dz, dDx/dx, dDz/dz); texel (i, j) of mip 0 is the rest position
//   (i, j) L / 512 (the horizontal displacement is a gradient field: dDz/dx = dDx/dz).
#ifndef UNX_WATER_OCEAN_SAMPLE_HLSLI
#define UNX_WATER_OCEAN_SAMPLE_HLSLI
#include "Bindless.hlsli"

// Displacement (Dx, h, Dz) and its derivatives: dD[0] = (dDx/dx, dh/dx, dDz/dx), dD[1] = (dDx/dz, dh/dz, dDz/dz).
struct OceanPoint
{
    float3 D;
    float3 dDdx, dDdz;
};

// Cubic Hermite basis on [0, 1] and its derivative: values at 0 / 1, slopes at 0 / 1.
void oceanHermite(float t, out float4 b, out float4 db)
{
    const float t2 = t * t, t3 = t2 * t;
    b = float4(2 * t3 - 3 * t2 + 1, -2 * t3 + 3 * t2, t3 - 2 * t2 + t, t3 - t2);
    db = float4(6 * t2 - 6 * t, -6 * t2 + 6 * t, 3 * t2 - 4 * t + 1, 3 * t2 - 2 * t);
}

// One cascade reconstructed C1 at rest position x0 (metres) from mip 0.
OceanPoint oceanCascadeHermite(Texture2DArray<float4> field, Texture2DArray<float4> slopes, uint c, float L, float2 x0)
{
    const float texel = L / 512.0;
    const float2 u = x0 / texel;
    const float2 cell = floor(u), f = u - cell;
    const int2 i0 = int2(cell);
    float4 bx, dbx, bz, dbz;
    oceanHermite(f.x, bx, dbx);
    oceanHermite(f.y, bz, dbz);
    OceanPoint o;
    o.D = 0; o.dDdx = 0; o.dDdz = 0;
    [unroll] for (uint k = 0; k < 4; ++k)
    {
        const int2 q = (i0 + int2(k & 1, k >> 1)) & 511;
        const float4 d = field.Load(int4(q, c, 0)), s = slopes.Load(int4(q, c, 0));
        const float3 value = d.xyz;
        const float3 ddx = float3(s.z, s.x, d.w) * texel, ddz = float3(d.w, s.y, s.w) * texel;  // per texel
        const uint a = k & 1, b = k >> 1;
        // f(u, v) = sum H_a(u) H_b(v) value + G_a(u) H_b(v) ddx + H_a(u) G_b(v) ddz (zero twist)
        const float Ha = bx[a], Ga = bx[2 + a], dHa = dbx[a], dGa = dbx[2 + a];
        const float Hb = bz[b], Gb = bz[2 + b], dHb = dbz[b], dGb = dbz[2 + b];
        o.D += Ha * Hb * value + Ga * Hb * ddx + Ha * Gb * ddz;
        o.dDdx += dHa * Hb * value + dGa * Hb * ddx + dHa * Gb * ddz;
        o.dDdz += Ha * dHb * value + Ga * dHb * ddx + Ha * dGb * ddz;
    }
    o.dDdx /= texel;
    o.dDdz /= texel;
    return o;
}

// The three cascades at rest position x0 with footprint s (m): per cascade mip log2(s 512 / L); C1 Hermite below mip 1
// (blended into the trilinear filter between mip 0 and 1), trilinear above.
OceanPoint oceanSample(uint fieldSrv, uint slopeSrv, float3 lengths, float2 x0, float footprint)
{
    Texture2DArray<float4> field = ResourceDescriptorHeap[fieldSrv];
    Texture2DArray<float4> slopes = ResourceDescriptorHeap[slopeSrv];
    OceanPoint o;
    o.D = 0; o.dDdx = 0; o.dDdz = 0;
    [unroll] for (uint c = 0; c < 3; ++c)
    {
        const float L = lengths[c];
        const float mip = clamp(log2(footprint * 512.0 / L), 0.0, 9.0);
        OceanPoint p;
        if (mip < 1.0)
        {
            p = oceanCascadeHermite(field, slopes, c, L, x0);
            if (mip > 0.0)
            {
                const float3 uv = float3(x0 / L + 0.5 / 512.0, c);
                const float4 d = field.SampleLevel(g_linearWrap, uv, mip), s = slopes.SampleLevel(g_linearWrap, uv, mip);
                p.D = lerp(p.D, d.xyz, mip);
                p.dDdx = lerp(p.dDdx, float3(s.z, s.x, d.w), mip);
                p.dDdz = lerp(p.dDdz, float3(d.w, s.y, s.w), mip);
            }
        }
        else
        {
            const float3 uv = float3(x0 / L + 0.5 / 512.0, c);
            const float4 d = field.SampleLevel(g_linearWrap, uv, mip), s = slopes.SampleLevel(g_linearWrap, uv, mip);
            p.D = d.xyz;
            p.dDdx = float3(s.z, s.x, d.w);
            p.dDdz = float3(d.w, s.y, s.w);
        }
        o.D += p.D;
        o.dDdx += p.dDdx;
        o.dDdz += p.dDdz;
    }
    return o;
}
#endif
