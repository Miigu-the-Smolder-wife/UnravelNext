// unx-kernel: cs_6_6 main
// Heat haze composite of M (FEATURES_GAME 0.A-8; E's Passes/Volume produce the fields; Distortion.cpp): a pixel whose opaque
// depth lies behind the haze's front (its device depth below distortionDepth's) re-reads the shaded image at
// p + D (1 - z_haze / z_pixel) - D the far-field deflection of the bent view ray, the factor its share at the pixel's
// depth (0 just behind the haze, 1 for the sky). D is a smooth field: bilinear from the 1/4 resolution; the haze depth is a
// classification: the nearest 1/4 texel. Other pixels copy.
// P[0] = { image SRV, output UAV, offset SRV (RG16F 1/4), haze depth SRV (R16F 1/4) }, P[1] = { opaque depth SRV, width,
// height, 0 }; frame constants of the view (near plane).
#include "Bindless.hlsli"
#include "Frame.hlsli"

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    const uint2 size = P[1].yz;
    if (any(id >= size)) return;
    Texture2D<float4> image = ResourceDescriptorHeap[P[0].x];
    RWTexture2D<float4> output = ResourceDescriptorHeap[P[0].y];
    Texture2D<float2> offset = ResourceDescriptorHeap[P[0].z];
    Texture2D<float> hazeDepth = ResourceDescriptorHeap[P[0].w];
    Texture2D<float> depth = ResourceDescriptorHeap[P[1].x];
    uint qw, qh;
    hazeDepth.GetDimensions(qw, qh);
    const float haze = hazeDepth.Load(int3(min(id / 4u, uint2(qw, qh) - 1), 0));
    const float opaque = depth.Load(int3(id, 0));
    if (!(haze > 0) || !(opaque < haze))
    {
        output[id] = image.Load(int3(id, 0));
        return;
    }
    const float2 p = float2(id) + 0.5f;
    const float2 D = offset.SampleLevel(g_linearClamp, p / (4.0f * float2(qw, qh)), 0);
    // z_haze / z_pixel = (near / haze) / (near / opaque) = opaque / haze (the sky: opaque = 0, the whole deflection)
    const float2 q = p + D * (1.0f - opaque / haze);
    output[id] = image.SampleLevel(g_linearClamp, q / float2(size), 0);
}
