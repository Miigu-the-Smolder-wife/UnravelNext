// unx-kernel: cs_6_6 main
// Ocean bounds pyramid (FEATURES_GAME 1.8 B.2, the adaptive near field's distance test): per cascade and mip, texel
// (max h, min h, max |(Dx, Dz)|, 0) over the rest region the texel's mip-0 texels cover, conservative for the C1 Hermite
// reconstruction (OceanSample.hlsli): at mip 0 each sample's h is widened by a quarter texel times its slope sum (a
// cubic Hermite span stays within max(v0, v1) + (|m0| + |m1|) / 4 of its end values, slopes per texel), and the
// horizontal displacement likewise. Mip m (P[1].x) reduces 2 x 2 texels of mip m - 1; the region of mip-m texel (i, j)
// covers mip-0 texels [2^m i, 2^m (i + 1)] (the Hermite span to the next sample included).
// P[0] displacement SRV, slopes SRV, bounds UAV (this mip; Texture2DArray RGBA32F, 3 slices, 10 mips), source UAV (the
// previous mip); P[1] mip; P[2] texel size (m) of cascades 0..2 (L / 512: the slopes are per metre)
#include "Bindless.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint mip = P[1].x, size = 512u >> mip;
    if (id.x >= size || id.y >= size || id.z >= 3) return;
    RWTexture2DArray<float4> bounds = ResourceDescriptorHeap[P[0].z];
    float4 b;
    if (mip == 0)
    {
        // The cell [i, i + 1]^2 of the reconstruction: a bicubic Hermite patch (zero twist) is a combination of the corner
        // values with weights H >= 0 summing to 1, plus slopes times weights G with |G_0| + |G_1| <= 1/4 per axis, so it
        // stays within [min v, max v] widened by texel (max |m_x| + max |m_z|) / 4 (slopes per metre).
        Texture2DArray<float4> field = ResourceDescriptorHeap[P[0].x];
        Texture2DArray<float4> slopes = ResourceDescriptorHeap[P[0].y];
        const float texel = asfloat(P[2][id.z]);
        float hMax = -3.0e38, hMin = 3.0e38, xMax = 0, zMax = 0;
        float4 slopeMax = 0;  // per-corner maxima of |dh/dx|, |dh/dz|, |dDx/dx|, |dDz/dz|
        float crossMax = 0;   // of |dDx/dz| = |dDz/dx|
        [unroll] for (uint k = 0; k < 4; ++k)
        {
            const int2 q = (int2(id.xy) + int2(k & 1, k >> 1)) & 511;
            const float4 d = field.Load(int4(q, id.z, 0)), s = slopes.Load(int4(q, id.z, 0));
            hMax = max(hMax, d.y);
            hMin = min(hMin, d.y);
            xMax = max(xMax, abs(d.x));
            zMax = max(zMax, abs(d.z));
            slopeMax = max(slopeMax, abs(float4(s.x, s.y, s.z, s.w)));  // dh/dx, dh/dz, dDx/dx, dDz/dz
            crossMax = max(crossMax, abs(d.w));                         // dDx/dz = dDz/dx
        }
        // Per axis: the x slopes' term and the z slopes' term, each at most texel / 4 times its largest corner magnitude.
        const float widen = 0.25 * texel, hWiden = widen * (slopeMax.x + slopeMax.y);
        b = float4(hMax + hWiden, hMin - hWiden, length(float2(xMax + widen * (slopeMax.z + crossMax), zMax + widen * (crossMax + slopeMax.w))), 0);
    }
    else
    {
        RWTexture2DArray<float4> source = ResourceDescriptorHeap[P[0].w];
        const uint2 c = id.xy * 2;
        const float4 p = source[uint3(c, id.z)], q = source[uint3(c + uint2(1, 0), id.z)];
        const float4 r = source[uint3(c + uint2(0, 1), id.z)], t = source[uint3(c + uint2(1, 1), id.z)];
        b = float4(max(max(p.x, q.x), max(r.x, t.x)), min(min(p.y, q.y), min(r.y, t.y)), max(max(p.z, q.z), max(r.z, t.z)), 0);
    }
    bounds[id] = b;
}
