// unx-kernel: cs_6_6 main
// P[0] = {cache params SRV, indirection SRV, prepared lookup UAV, 0}; common
// probe inputs as LgCommon. One thread per probe, never one per ray direction.
#include "Passes/GI/Lumen/LgRadianceCache.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint2 atlas = id.xy;
    if (atlas.x >= lgProbeViewSize().x || atlas.y >= lgAtlasRows()) return;
    const uint probe = lgProbeIndex(atlas);
    ByteAddressBuffer adaptive = ResourceDescriptorHeap[P[10].z];
    Texture2D<float> depth = ResourceDescriptorHeap[P[10].w];
    RWByteAddressBuffer output = ResourceDescriptorHeap[P[0].z];
    // Inactive rows are overwritten too: a reused transient must never expose
    // last frame's valid lookup after a cut, resize or adaptive-count decrease.
    if (probe >= lgProbeCount(adaptive) || !(depth[atlas] > 0))
    {
        output.Store4(probe * LG_PROBE_CACHE_BYTES, uint4(0, 0, asuint(1e7), 0));
        return;
    }
    Texture2D<float4> position = ResourceDescriptorHeap[P[11].y];
    lgRcPrepare(output, probe, lrcParams(P[0].x), P[0].y, position[atlas].xyz, lgRcDither(atlas));
}
