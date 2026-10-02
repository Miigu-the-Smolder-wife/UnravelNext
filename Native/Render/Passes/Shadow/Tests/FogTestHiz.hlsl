// unx-kernel: cs_6_6 main
// FogTests: the level of V's depth pyramid that the fog's kernels read (ViewResources::hiz: a texel of mip m holds the
// farthest depth - the smallest reversed-Z value - of its 2^(m+1) x 2^(m+1) pixels), made from the test raster's depth.
// One thread per texel; a texel's pixels outside the view are left out.
// P[0] = { depth SRV, output UAV (RWTexture2D<float>: that mip), the texel's size in pixels, 0 }. Frame constants of the view.
#include "Bindless.hlsli"
#include "Frame.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    RWTexture2D<float> output = ResourceDescriptorHeap[P[0].y];
    uint w, h;
    output.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    Texture2D<float> depth = ResourceDescriptorHeap[P[0].x];
    const uint n = P[0].z;
    float farthest = 1.0;
    for (uint y = 0; y < n; ++y)
        for (uint x = 0; x < n; ++x)
        {
            const uint2 pixel = id.xy * n + uint2(x, y);
            if (all(pixel < uint2(g_viewWidth, g_viewHeight))) farthest = min(farthest, depth.Load(int3(pixel, 0)));
        }
    output[id.xy] = farthest;
}
