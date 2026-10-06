// unx-kernel: cs_6_6 main
#include "Passes/GI/Lumen/LgRadianceCache.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint seed = P[0].z;
    const float shift = seed >= 8 ? 1024.0 : 0.0;
    if (all(id == 0))
    {
        LrcParams p = (LrcParams)0;
        p.cornerCell[0] = float4(float3(-8, -8, -8) + shift, 2);
        p.cornerCell[1] = float4(float3(-16, -16, -16) + shift, 4);
        p.clipmaps = 2; p.grid = 8; p.probeResolution = 4; p.atlasProbes = 16;
        p.reprojectionRadiusScale = 1.5; p.invFadeSize = 1; p.tMinScale = 1; p.traceDistance = 20;
        p.frame = seed; p.maxProbes = 256; p.finalResolution = 6;
        RWByteAddressBuffer params = ResourceDescriptorHeap[P[0].w]; params.Store<LrcParams>(0, p);
        RWByteAddressBuffer adaptive = ResourceDescriptorHeap[P[2].y];
        adaptive.Store4(0, uint4(P[2].z, 0, 0, 0));
    }
    if (id.x < 16 && id.y < 8 && id.z < 8)
    {
        const uint cell = id.x + 16 * (id.y + 8 * id.z);
        uint slot = (cell * 37 + seed * 17) % 256;
        if (seed % 8 == 1 && cell % 11 == 0) slot = LRC_INVALID;
        if (seed % 8 == 2 && cell % 7 == 0) slot = LRC_USED;
        RWTexture3D<uint> indirection = ResourceDescriptorHeap[P[1].x]; indirection[id] = slot;
    }
    if (id.z != 0) return;
    if (id.x < 96 && id.y < 96)
    {
        float alpha = seed % 8 == 3 ? 0 : seed % 8 == 4 ? float((id.x + 3 * id.y) % 5 == 0) : 1;
        RWTexture2D<float4> atlas = ResourceDescriptorHeap[P[1].y];
        atlas[id.xy] = float4(float3(1 + id.x % 7, 1 + id.y % 11, 1 + (id.x + id.y) % 13) * (LRC_RADIANCE_SCALE * alpha), alpha);
    }
    if (id.x < 64 && id.y < 64)
    {
        RWTexture2D<uint> depth = ResourceDescriptorHeap[P[1].z];
        const bool blocked = seed % 8 == 5 || (seed % 8 == 6 && (id.x + id.y) % 3 != 0);
        depth[id.xy] = blocked ? lrcEncodeDepth(0.01, true, true, false) : 0xFFFFu;
    }
    if (id.x < P[0].x && id.y < P[0].y)
    {
        const uint i = id.x + id.y * P[0].x;
        float3 p = float3((int(i * 13 % 101) - 50) * 0.07, (int(i * 37 % 103) - 51) * 0.06, 0);
        // Exact cell centres (zero corner weights), fading clipmap edges,
        // negative coordinates, out-of-coverage points and rebased coordinates.
        if (i % 17 == 0) p = float3(1, -1, 1);
        if (i % 17 == 1) p.x = 7;
        if (i % 17 == 2) p.x = -7.000001;
        if (i % 17 == 3) p = 40;
        RWTexture2D<float4> position = ResourceDescriptorHeap[P[2].x]; position[id.xy] = float4(p + shift, 0);
        RWTexture2D<float> probeDepth = ResourceDescriptorHeap[P[1].w]; probeDepth[id.xy] = i % 13 == 0 ? -1 : 5;
    }
}
