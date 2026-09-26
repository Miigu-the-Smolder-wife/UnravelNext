// unx-kernel: cs_6_6 main
// Debug overlay composite (E, A15): the premultiplied overlay (RGBA16F: display-encoded sRGB colour x opacity) over the
// view's colour. P[0] = { overlay SRV, copy of the colour SRV (its format), colour UAV, linear }: linear = 0 for the
// display-encoded SDR output (blend in the encoded values, as UI blends), 1 for linear outputs (HDR display light with
// paper white 1, or validation radiance): the overlay's colour is decoded to linear first. Pixels the overlay does not
// touch are not written (the colour already holds them). P[1].xy view size.
#include "Bindless.hlsli"

float debugSrgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : pow((c + 0.055f) / 1.055f, 2.4f); }

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= P[1].x || id.y >= P[1].y) return;
    Texture2D<float4> overlay = ResourceDescriptorHeap[P[0].x];
    const float4 o = overlay.Load(int3(id.xy, 0));
    if (o.a <= 0) return;
    Texture2D<float4> source = ResourceDescriptorHeap[P[0].y];
    RWTexture2D<float4> colour = ResourceDescriptorHeap[P[0].z];
    const float4 dst = source.Load(int3(id.xy, 0));
    float3 add = o.rgb;
    if (P[0].w != 0)
    {
        const float3 straight = saturate(o.rgb / o.a);
        add = float3(debugSrgbToLinear(straight.r), debugSrgbToLinear(straight.g), debugSrgbToLinear(straight.b)) * o.a;
    }
    colour[id.xy] = float4(add + dst.rgb * (1 - o.a), dst.a);
}
