// unx-kernel: cs_6_6 main
// Bloom pyramid (Post.cpp), one level up: dst = lerp(dst, tent(src), weight), the coarser level filtered by the tent of
// half-width 2 coarse texels at this texel's centre (normalised), weight = the share that keeps every level's weight
// equal. P[0] = { coarse SRV, destination UAV, destination width, height }, P[1].x = asfloat(weight).
#include "Bindless.hlsli"
#include "Passes/Shading/HalfRound.hlsli"

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    if (any(id >= P[0].zw)) return;
    Texture2D<float4> src = ResourceDescriptorHeap[P[0].x];
    RWTexture2D<float4> dst = ResourceDescriptorHeap[P[0].y];
    uint sw, sh;
    src.GetDimensions(sw, sh);
    const float2 p = (float2(id) + 0.5) * 0.5 - 0.5;  // this texel's centre in the coarse level's texel space
    const int2 b = (int2)floor(p);
    const float2 f = p - b;
    const int2 hi = int2(sw, sh) - 1;
    float3 sum = 0;
    float wsum = 0;
    [unroll] for (int y = -1; y <= 2; ++y)
        [unroll] for (int x = -1; x <= 2; ++x)
        {
            const float2 d = float2(x, y) - f;
            const float2 w2 = max(0.0, 1.0 - abs(d) * 0.5);
            const float w = w2.x * w2.y;
            sum += w * src.Load(int3(clamp(b + int2(x, y), int2(0, 0), hi), 0)).rgb;
            wsum += w;
        }
    dst[id] = float4(halfRound(lerp(dst[id].rgb, sum / wsum, asfloat(P[1].x))), 1);
}
