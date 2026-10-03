// unx-kernel: cs_6_6 main
// unx-variants: TYPE=0,1,2,3,4 DIM=2,3
// Regrid temporal records by normalized view coordinates. Point sampling keeps depth, keys,
// radiance and frame counts on the same source sample. Probe maps retain their 8x8 direction blocks.
#include "Bindless.hlsli"
#if TYPE == 0
#define RECORD float
#elif TYPE == 1
#define RECORD float2
#elif TYPE == 2
#define RECORD float4
#elif TYPE == 3
#define RECORD uint
#else
#define RECORD uint2
#endif
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint3 dstSize = P[1].xyz;
    if (any(id >= dstSize)) return;
    const uint block = max(P[0].z, 1u);
    const uint2 dstBlock = id.xy / block;
    const uint2 srcBlocks = P[2].xy, dstBlocks = P[2].zw;
    RECORD value = (RECORD)0;
    if (all(dstBlock < dstBlocks))
    {
        const uint2 q = min((uint2)((float2(dstBlock) + 0.5) * float2(srcBlocks) / float2(dstBlocks)), srcBlocks - 1u) * block + id.xy % block;
#if DIM == 3
        Texture3D<RECORD> src = ResourceDescriptorHeap[P[0].x];
        uint w, h, depth; src.GetDimensions(w, h, depth);
        const uint z = min((uint)((id.z + 0.5) * depth / dstSize.z), depth - 1u);
        value = src[uint3(q, z)];
#else
        Texture2D<RECORD> src = ResourceDescriptorHeap[P[0].x];
        value = src[q];
#endif
    }
#if DIM == 3
    RWTexture3D<RECORD> dst = ResourceDescriptorHeap[P[0].y];
#else
    RWTexture2D<RECORD> dst = ResourceDescriptorHeap[P[0].y];
#endif
#if DIM == 3
    dst[id] = value;
#else
    dst[id.xy] = value;
#endif
}
