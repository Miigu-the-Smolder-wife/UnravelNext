// unx-kernel: cs_6_6 main
// unx-variants: MODE=0,1
// m.ml.sets (MegaLights.hlsli): the lights whose samples were visible / hidden in each 8 x 8 pixel tile, as two small bit
// sets per tile (24 B), for the next frame's m.ml.sample: a light that was only hidden there is offered with the hidden
// weight, and a light that was both is in a penumbra. One thread per tile.
// MODE 0 (build): reads the tile's light samples after the trace.
//   P[0] = { samples SRV (R32G32_UINT), sets UAV (raw), tiles X, tiles Y }, P[1] = { factor | N << 8, samples width, height, 0 }
// MODE 1 (filter): each tile's sets joined with its 4 neighbours' (a light seen next door is likely seen here next frame).
//   P[0] = { built sets SRV (raw), filtered sets UAV (raw: the history), tiles X, tiles Y }
#include "Bindless.hlsli"
#include "Frame.hlsli"
#include "Passes/Shading/MegaLights.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint2 tile = id.xy;
    if (any(tile >= P[0].zw)) return;
    RWByteAddressBuffer dst = ResourceDescriptorHeap[P[0].y];
    const uint at = (tile.y * P[0].z + tile.x) * (4 * ML_HASH_WORDS);
    uint4 visibleSet = 0;
    uint2 hiddenSet = 0;
#if MODE == 0
    Texture2D<uint2> samples = ResourceDescriptorHeap[P[0].x];
    const uint factor = P[1].x & 0xFFu, count = (P[1].x >> 8) & 0xFFu;
    const uint2 grid = mlSampleGrid(count);
    const uint2 lo = tile * (ML_HASH_TILE / factor) * grid, hi = min((tile + 1) * (ML_HASH_TILE / factor) * grid, P[1].yz);
    for (uint y = lo.y; y < hi.y; ++y)
        for (uint x = lo.x; x < hi.x; ++x)
        {
            const MlSample s = mlUnpack(samples[uint2(x, y)]);
            if (s.light == ML_LIGHT_NONE || s.merged) continue;
            if (s.visible) mlMarkVisible(visibleSet, s.light);
            else mlMarkHidden(hiddenSet, s.light);
        }
#else
    ByteAddressBuffer src = ResourceDescriptorHeap[P[0].x];
    static const int2 offsets[5] = { int2(0, 0), int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1) };
    for (uint k = 0; k < 5; ++k)
    {
        const int2 t = int2(tile) + offsets[k];
        if (any(t < 0) || any(t >= int2(P[0].zw))) continue;
        const uint from = (uint(t.y) * P[0].z + uint(t.x)) * (4 * ML_HASH_WORDS);
        visibleSet |= src.Load4(from);
        hiddenSet |= src.Load2(from + 16);
    }
#endif
    dst.Store4(at, visibleSet);
    dst.Store2(at + 16, hiddenSet);
}
