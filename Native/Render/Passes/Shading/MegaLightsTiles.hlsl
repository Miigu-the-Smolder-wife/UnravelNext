// unx-kernel: cs_6_6 main
// unx-variants: MODE=0,1
// m.ml.tiles (MegaLights.hlsli): the downsampled tiles (8 x 8 downsampled pixels) that hold a surface, as a list the
// sample kernel is dispatched over (one group per listed tile; ExecuteIndirect reads the count); the downsampled pixels
// of the other tiles get empty samples and an empty key here. The counterpart of Unreal's tile classification for the
// sampling: its split of the tiles by shading model and by rectangle lights is not made here - our kernels are chosen by
// M's shade class lists (m.ml.shade) and by whether the scene has area lights at all.
// List (raw): words 0..3 = dispatch arguments (tiles, 1, 1) and a pad, then one word per tile (x | y << 16), in the
// order the groups finish (the tiles are independent: the order changes no value).
// MODE 0 (one thread): writes the arguments (0, 1, 1).   P[0].y = list UAV
// MODE 1 (one group per tile): P[0] = { material word, list UAV, samples UAV, keys UAV }, P[1] = { downsampled width,
//         height, factor | N << 8, 0 }
#include "Bindless.hlsli"
#include "Frame.hlsli"
#include "Passes/Material/MaterialInternal.hlsli"
#include "Passes/Shading/MegaLights.hlsli"

#if MODE == 0
[numthreads(1, 1, 1)]
void main()
{
    RWByteAddressBuffer list = ResourceDescriptorHeap[P[0].y];
    list.Store4(0, uint4(0, 1, 1, 0));
}
#else
groupshared uint gs_any;

[numthreads(8, 8, 1)]
void main(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID)
{
    if (tid.x == 0 && tid.y == 0) gs_any = 0;
    GroupMemoryBarrierWithGroupSync();
    const uint2 ds = gid.xy * 8 + tid.xy;
    const bool inside = all(ds < P[1].xy);
    const uint factor = P[1].z & 0xFFu, count = (P[1].z >> 8) & 0xFFu;
    bool surface = false;
    if (inside)
    {
        Texture2D<uint> words = ResourceDescriptorHeap[P[0].x];
        surface = mWordMaterial(words[mlFullPixel(ds, factor, g_frameIndex)]) != M_MATERIAL_SKY;
    }
    if (surface) InterlockedOr(gs_any, 1u);
    GroupMemoryBarrierWithGroupSync();
    if (gs_any != 0)
    {
        if (tid.x == 0 && tid.y == 0)
        {
            RWByteAddressBuffer list = ResourceDescriptorHeap[P[0].y];
            uint at;
            list.InterlockedAdd(0, 1u, at);
            list.Store(16 + 4 * at, gid.x | (gid.y << 16));
        }
        return;
    }
    if (!inside) return;
    RWTexture2D<uint2> samples = ResourceDescriptorHeap[P[0].z];
    RWTexture2D<uint2> keys = ResourceDescriptorHeap[P[0].w];
    for (uint i = 0; i < count; ++i) samples[mlSampleCoord(ds, count, i)] = mlPack(mlNoSample());
    keys[ds] = uint2(0, 0);
}
#endif
