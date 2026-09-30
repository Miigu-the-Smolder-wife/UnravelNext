// unx-kernel: cs_6_6 main
// Frame-local illumination at the GI cell's scale, with two separable Gaussian
// sums in one dispatch. Fixed 17 taps per axis; 9 KiB shared memory. All lanes
// reach both barriers, including partial edge groups.
// P[0] = { linear colour SRV, low-frequency colour UAV, width, height }.
#include "Bindless.hlsli"
groupshared float3 sourceTile[24 * 24];
groupshared float3 horizontal[24 * 8];
static const float weights[17] = {0.0139601889, 0.0223083183, 0.0334887522, 0.0472267103, 0.0625652260, 0.0778636818, 0.0910318666, 0.0999789464, 0.1031526189, 0.0999789464, 0.0910318666, 0.0778636818, 0.0625652260, 0.0472267103, 0.0334887522, 0.0223083183, 0.0139601889};
[numthreads(8, 8, 1)]
void main(uint2 group : SV_GroupID, uint2 local : SV_GroupThreadID, uint lane : SV_GroupIndex)
{
    const uint2 size = P[0].zw;
    Texture2D<float4> source = ResourceDescriptorHeap[P[0].x];
    RWTexture2D<float4> output = ResourceDescriptorHeap[P[0].y];
    const int2 first = int2(group * 8) - 8;
    [loop] for (uint i = lane; i < 24 * 24; i += 64)
    {
        const int2 q = clamp(first + int2(i % 24, i / 24), 0, int2(size) - 1);
        const float3 c = source.Load(int3(q, 0)).rgb;
        sourceTile[i] = all(isfinite(c)) ? max(c, 0) : 0;
    }
    GroupMemoryBarrierWithGroupSync();
    [loop] for (uint i = lane; i < 24 * 8; i += 64)
    {
        const uint row = i / 8, col = i % 8;
        float3 sum = 0;
        [unroll] for (uint k = 0; k < 17; ++k) sum += weights[k] * sourceTile[row * 24 + col + k];
        horizontal[i] = sum;
    }
    GroupMemoryBarrierWithGroupSync();
    const uint2 pixel = group * 8 + local;
    if (any(pixel >= size)) return;
    float3 sum = 0;
    [unroll] for (uint k = 0; k < 17; ++k) sum += weights[k] * horizontal[(local.y + k) * 8 + local.x];
    output[pixel] = float4(sum, 0);
}
