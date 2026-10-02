// unx-kernel: cs_6_6 main
// m.post.le.blur (LocalExposure.hlsli): the tiles' mean log luminance under the reference's Gaussian - weight
// exp(-16.7 (d / R)^2), R a quarter of the view's width (its blurred luminance kernel of 50 %: a radius of 25 % of the
// width), here in grid tiles - with the image mirrored at its borders. The grid is a few hundred texels: one pass, the
// full two-dimensional kernel out to 0.6 R (the weight there is under 0.3 %).
// P[0] = { tile mean SRV (Texture2D<float>), blurred UAV (RWTexture2D<float>), grid width, grid height },
// P[1] = { asuint(R in tiles), tiles of the view along x and y (the last ones may overhang it), 0 }
#include "Bindless.hlsli"

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    if (any(id >= P[0].zw)) return;
    Texture2D<float> mean = ResourceDescriptorHeap[P[0].x];
    const float radius = max(asfloat(P[1].x), 0.5);
    const int reach = (int)clamp(ceil(radius * 0.6), 1.0, 16.0);
    const int2 size = int2(P[0].zw);
    float sum = 0, weight = 0;
    for (int y = -reach; y <= reach; ++y)
        for (int x = -reach; x <= reach; ++x)
        {
            int2 at = int2(id) + int2(x, y);
            // mirrored at the borders (a clamp would weigh the border tiles more)
            at = abs(at);
            at = min(at, 2 * (size - 1) - at);
            at = clamp(at, int2(0, 0), size - 1);
            const float w = exp(-16.7 * float(x * x + y * y) / (radius * radius));
            sum += w * mean.Load(int3(at, 0));
            weight += w;
        }
    RWTexture2D<float> blurred = ResourceDescriptorHeap[P[0].y];
    blurred[id] = sum / weight;
}
