// unx-kernel: cs_6_6 main
// PostTests.cpp inputs. P[0] = { target UAV, x, y, asfloat value }, P[1].x = mode:
//   0: zeros with one texel of value P[0].w (red) at (x, y) (the bloom tail's energy);
//   1: an HDR ramp over the curve's whole range: r = 2^(12 u - 8) (u across the image), g = r (1 - v), b = r v^2 / 2
//      (v down the image), so every curve region (toe offset, linear part, shoulder) and hue is met (PostFinal's reference);
//   2: the constant (asfloat P[0].y, asfloat P[0].z, asfloat P[0].w, 1) (a velocity field, a depth);
//   3: sinusoids along x: r = 0.5 + 0.4 sin(2 pi x / 64), g = 0.5 + 0.4 sin(2 pi x / 23), b = 0.5 (x = pixel centre).
#include "Bindless.hlsli"

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    RWTexture2D<float4> t = ResourceDescriptorHeap[P[0].x];
    uint w, h;
    t.GetDimensions(w, h);
    if (any(id >= uint2(w, h))) return;
    if (P[1].x == 0)
    {
        t[id] = all(id == P[0].yz) ? float4(asfloat(P[0].w), 0, 0, 1) : float4(0, 0, 0, 1);
        return;
    }
    if (P[1].x == 2)
    {
        t[id] = float4(asfloat(P[0].y), asfloat(P[0].z), asfloat(P[0].w), 1);
        return;
    }
    if (P[1].x == 3)
    {
        const float x = id.x + 0.5;
        t[id] = float4(0.5 + 0.4 * sin(6.28318531 * x / 64.0), 0.5 + 0.4 * sin(6.28318531 * x / 23.0), 0.5, 1);
        return;
    }
    const float u = (id.x + 0.5) / w, v = (id.y + 0.5) / h;
    const float r = exp2(12.0 * u - 8.0);
    t[id] = float4(r, r * (1.0 - v), r * v * v * 0.5, 1);
}
