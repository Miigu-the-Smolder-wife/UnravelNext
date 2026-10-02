// unx-kernel: cs_6_6 main
// unx-variants: MODE=0,1,2,3
// The hair density volume (HairDensity.hlsli), after the frame's strand segments are written:
//   MODE 0  the accumulation words set to 0 (256 words a thread group);
//   MODE 1  one thread per segment of one body: its length x mean diameter / cell volume, in up to 8 equal parts along it
//           (one per half cell), each added to the cell under its middle (atomic add, units of 1 / 1024 per metre);
//           parts outside the body's cells add nothing (the LOD's kept strands alone have segments: HairSimulate STEP 2);
//   MODE 2  the cells' values, R16_FLOAT (4 x 4 x 4 cells a group);
//   MODE 3  their means over 4 x 4 x 4 cells (the 64 cells' words).
// P[0] = { segments SRV (StructuredBuffer<float4>, 2 per segment), accumulation UAV (raw: R^3 words per block, cell
//          x + R (y + R z)), parameters SRV (raw, HairDensity.hlsli), the texture UAV (MODE 2: fine, MODE 3: coarse) }
// P[1] = { MODE 0: words; MODE 1: the body, its first segment, its segments, its block; MODE 2, 3: blocks per row, blocks }
#include "Bindless.hlsli"
#include "Passes/Hair/HairDensity.hlsli"

#if MODE == 0
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    RWByteAddressBuffer sums = ResourceDescriptorHeap[P[0].y];
    const uint word = 4 * id.x;
    if (word < P[1].x) sums.Store4(4 * word, uint4(0, 0, 0, 0));
}
#elif MODE == 1
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= P[1].z) return;
    StructuredBuffer<float4> segments = ResourceDescriptorHeap[P[0].x];
    RWByteAddressBuffer sums = ResourceDescriptorHeap[P[0].y];
    ByteAddressBuffer params = ResourceDescriptorHeap[P[0].z];
    const uint res = params.Load(4);
    const HairDensityBody body = hairDensityBody(params, P[1].x);
    const float4 a = segments[2 * (P[1].y + id.x)], b = segments[2 * (P[1].y + id.x) + 1];
    if (!(a.w > 0 && b.w > 0)) return;
    const float len = length(b.xyz - a.xyz);
    const uint parts = clamp((uint)ceil(len / (0.5f * body.cell)), 1u, 8u);
    const uint units = (uint)(len * (a.w + b.w) / (parts * body.cell * body.cell * body.cell * HAIR_DENSITY_UNIT) + 0.5f);
    [loop] for (uint k = 0; k < parts; ++k)
    {
        const float3 q = (lerp(a.xyz, b.xyz, (k + 0.5f) / parts) - body.origin) / body.cell;
        if (any(q < 0) || any(q >= float3(body.cells))) continue;
        const uint3 c = uint3(q);
        uint was;
        sums.InterlockedAdd(4 * (P[1].w * res * res * res + c.x + res * (c.y + res * c.z)), units, was);
    }
}
#else
[numthreads(4, 4, 4)]
void main(uint3 id : SV_DispatchThreadID)
{
    ByteAddressBuffer sums = ResourceDescriptorHeap[P[0].y];
    ByteAddressBuffer params = ResourceDescriptorHeap[P[0].z];
    RWTexture3D<float> volume = ResourceDescriptorHeap[P[0].w];
    const uint res = params.Load(4), factor = MODE == 3 ? HAIR_DENSITY_COARSE : 1u, side = res / factor;
    const uint2 block = id.xy / side;
    const uint index = block.x + block.y * P[1].x;
    if (block.x >= P[1].x || index >= P[1].y || id.z >= side) return;
    const uint3 first = uint3(id.xy - block * side, id.z) * factor;
    uint sum = 0;
    [loop] for (uint z = 0; z < factor; ++z)
        [loop] for (uint y = 0; y < factor; ++y)
            [loop] for (uint x = 0; x < factor; ++x)
            {
                const uint3 c = first + uint3(x, y, z);
                sum += sums.Load(4 * (index * res * res * res + c.x + res * (c.y + res * c.z)));
            }
    volume[id] = min(sum * (HAIR_DENSITY_UNIT / (factor * factor * factor)), 65000.0f);
}
#endif
