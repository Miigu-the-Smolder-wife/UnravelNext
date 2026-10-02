// unx-kernel: cs_6_6 main
// Diaphragm depth of field, the post filter of a gathered layer (DiaphragmDof.cpp; the reference's
// DOFPostfiltering.usf): a gather's few samples leave a bright point as an outline of dots; per channel the median of
// the 3 x 3 neighbourhood (or its largest value, method 2) closes them. The foreground and the background layers, before
// the sprites are drawn over them. A tile the layer's gather skipped is written 0, as the gather wrote it.
// P[0] = { layer SRV (RGBA16F), filtered UAV, foreground tiles SRV, background tiles SRV }
// P[1] = { half width, half height, layer (DdofCommon.hlsli: 0 foreground, 2 background), method (1 median, 2 largest) }
#include "Bindless.hlsli"
#include "Passes/Shading/DdofCommon.hlsli"

// The lowest, the median and the highest of three.
void sort3(float4 a, float4 b, float4 c, out float4 low, out float4 mid, out float4 high)
{
    const float4 x = min(b, c), y = max(b, c), z = max(a, x);
    low = min(a, x);
    mid = min(z, y);
    high = max(z, y);
}
float4 median3(float4 a, float4 b, float4 c)
{
    float4 low, mid, high;
    sort3(a, b, c, low, mid, high);
    return mid;
}

[numthreads(DDOF_TILE, DDOF_TILE, 1)]
void main(uint2 gid : SV_GroupID, uint2 id : SV_DispatchThreadID)
{
    const uint2 size = P[1].xy;
    if (any(id >= size)) return;
    Texture2D<float4> layer = ResourceDescriptorHeap[P[0].x];
    RWTexture2D<float4> filtered = ResourceDescriptorHeap[P[0].y];
    Texture2D<float4> tilesForeground = ResourceDescriptorHeap[P[0].z];
    Texture2D<float4> tilesBackground = ResourceDescriptorHeap[P[0].w];
    if (ddofSuggest(ddofLoadTile(tilesForeground, tilesBackground, int2(gid)), P[1].z).skip)
    {
        filtered[id] = 0;
        return;
    }
    float4 s[9];
    [unroll] for (uint i = 0; i < 9; ++i)
        s[i] = layer.Load(int3(clamp(int2(id) + int2((int)(i % 3) - 1, (int)(i / 3) - 1), 0, int2(size) - 1), 0));
    float4 result;
    if (P[1].w == 2)
    {
        result = s[0];
        [unroll] for (uint j = 1; j < 9; ++j) result = max(result, s[j]);
    }
    else
    {
        // the median of nine from the rows' sorted triples (Smith 1996)
        float4 low[3], mid[3], high[3];
        [unroll] for (uint r = 0; r < 3; ++r) sort3(s[3 * r], s[3 * r + 1], s[3 * r + 2], low[r], mid[r], high[r]);
        result = median3(max(max(low[0], low[1]), low[2]), median3(mid[0], mid[1], mid[2]), min(min(high[0], high[1]), high[2]));
    }
    filtered[id] = result;
}
