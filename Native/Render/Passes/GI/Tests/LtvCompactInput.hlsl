// unx-kernel: cs_6_6 main
#include "Frame.hlsli"
#include "Bindless.hlsli"
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    RWTexture2D<float> depth = ResourceDescriptorHeap[P[0].x];
    RWTexture2D<float> hiz = ResourceDescriptorHeap[P[0].y];
    if (id.x < P[0].z && id.y < P[0].w)
        depth[id.xy] = g_nearPlane / (0.15 + float((id.x * 7 + id.y * 11) % 29));
    if (id.x < P[1].x && id.y < P[1].y)
        hiz[id.xy] = P[1].z == 0 ? 0.0 : P[1].z == 1 ? 1.0 :
            g_nearPlane / (0.05 + float((id.x * 13 + id.y * 3) % 17));
}
