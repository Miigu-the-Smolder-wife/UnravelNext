// unx-kernel: cs_6_6 main
// Core test kernel (the eye; MaterialModel.hlsli "Eye"): modelEyePoint, modelEyePack, modelEyeOf and modelEyeCosine at
// deterministic points - a surface point's uv around the iris, the refracted view ray in the eye's frame, a light on
// either side of the surface (n = +z; the iris plane's normal is P[1].xyz) - with the eye's parameters from the scene's
// material record P[0].z (loadMaterialEye: GpuScene's record), through FrameConstants. Per point 16 words into a raw
// UAV: uv (2), t (3), l (3), the point's uv (2), mask, darkening, caustic, the eye word, the cosine from that word, 0.
//   P[0].x output UAV (raw), P[0].y point count, P[0].z the eye material; P[1].xyz the iris plane's normal (float bits)
#include "Bindless.hlsli"
#include "Frame.hlsli"
#include "Passes/Common/MaterialModel.hlsli"

float hash01(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return (x >> 8) * (1.0 / 16777216.0);
}

float3 direction(float mu, uint seed)
{
    const float phi = 6.2831853 * hash01(seed);
    const float s = sqrt(1 - mu * mu);
    return float3(s * cos(phi), s * sin(phi), mu);
}

[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    if (i >= P[0].y) return;
    const GpuMaterial m = loadMaterial(P[0].z);
    const GpuMaterialEye e = loadMaterialEye(m.classFlags >> 16);
    const float3 n = float3(0, 0, 1), axis = normalize(asfloat(P[1].xyz));
    // uv: out to 1.5 iris radii from the centre; the ray: into the eye, from head-on to grazing
    const float2 uv = 0.5 + 1.5 * e.irisRadius * sqrt(hash01(6 * i + 1)) * float2(cos(6.2831853 * hash01(6 * i + 2)), sin(6.2831853 * hash01(6 * i + 2)));
    const float3 t = direction(-(0.05 + 0.95 * hash01(6 * i + 3)), 6 * i + 4);
    // the light: three of four points above the surface, the fourth below it (the iris plane may still face it)
    const float3 l = direction((0.02 + 0.98 * hash01(6 * i + 5)) * ((i % 4) == 3 ? -1.0 : 1.0), 6 * i + 6);
    const ModelEyePoint p = modelEyePoint(e, uv, t);
    const uint word = modelEyePack(axis, p.mask, p.caustic);
    const float cosine = modelEyeCosine(modelEyeOf(word, n), dot(n, l), l);
    RWByteAddressBuffer o = ResourceDescriptorHeap[P[0].x];
    const uint b = 64 * i;
    o.Store2(b, asuint(uv));
    o.Store3(b + 8, asuint(t));
    o.Store3(b + 20, asuint(l));
    o.Store2(b + 32, asuint(p.uv));
    o.Store3(b + 40, uint3(asuint(p.mask), asuint(p.darkening), asuint(p.caustic)));
    o.Store3(b + 52, uint3(word, asuint(cosine), 0));
}
