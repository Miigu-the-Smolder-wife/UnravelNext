// unx-kernel: cs_6_6 main
// r.card.copy (SurfaceCacheCards.cpp): the frame's captures from the capture atlas into the card atlases, and the state
// of their lighting - Unreal's CopyCardCaptureLightingToAtlas (LumenSceneLighting.usf). Per texel of a captured page:
//   geometry   depth (R16_UNORM; 1.0 = no surface in the texel, a surface's depth at most 65534 / 65535), albedo, normal,
//              emissive;
//   lighting   a new page starts with no light (direct, indirect 0) - or, when the card had pages before this frame,
//              with its old lighting at the new resolution (r.card.resample) - and counts as never lit, so the update
//              selection takes it first; its tiles' radiosity frame counts and uniform-visibility bits start over. A
//              refreshed page (CC_FLAG_REFRESH) keeps all of it.
//   final      (direct + indirect) x albedo / pi + emission with the new albedo and emission, what a hit reads.
// One group row per capture (group.y), 16 x 16 groups of 8 x 8 texels over a page of at most 128 x 128.
// P[0] = { captures SRV (raw), captures, capture depth SRV, capture albedo SRV }
// P[1] = { capture normal SRV, capture emissive SRV, depth atlas UAV, albedo atlas UAV }
// P[2] = { normal atlas UAV, emissive atlas UAV, direct atlas UAV, indirect atlas UAV }
// P[3] = { final atlas UAV, radiosity frames atlas UAV (R8_UINT, atlas / 8), uniform bits UAV (raw), page light UAV (raw) }
// P[4] = { resampled direct SRV, resampled indirect SRV, atlas size, asuint(max radiosity frames kept by a resample) }
// P[5] = { 1: the capture depth is V's (the clusters' capture, CardCaptureCluster.ps.hlsl: reversed - 1 the card's
//          front, 0 its back and where nothing was drawn), 0, 0, 0 }
#include "Passes/SurfaceCache/CardLighting.hlsli"
#include "Passes/SurfaceCache/CardCaptureList.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
    if (group.y >= P[0].y) return;
    const CcCapture cap = ccLoadCapture(P[0].x, group.y);
    const uint2 tile = uint2(group.x & 15u, group.x >> 4);
    const uint2 p = tile * 8u + thread.xy;
    const bool refresh = (cap.flags & CC_FLAG_REFRESH) != 0, resample = (cap.flags & CC_FLAG_RESAMPLE) != 0;
    if (!refresh && group.x == 0 && all(thread.xy == 0))
    {
        RWByteAddressBuffer pageLight = ResourceDescriptorHeap[P[3].w];
        pageLight.Store4(cap.page * CL_PAGE_LIGHT_BYTES, uint4(0, 0, 0, 0));
    }
    if (any(p >= cap.atlasSize)) return;
    const int3 from = int3(cap.captureOrigin + p, 0);
    const uint2 to = cap.atlasOrigin + p;
    Texture2D<float> captureDepth = ResourceDescriptorHeap[P[0].z];
    Texture2D<float4> captureAlbedo = ResourceDescriptorHeap[P[0].w];
    Texture2D<float2> captureNormal = ResourceDescriptorHeap[P[1].x];
    Texture2D<float4> captureEmissive = ResourceDescriptorHeap[P[1].y];
    const float d = P[5].x != 0 ? 1.0 - captureDepth.Load(from) : captureDepth.Load(from);
    const bool surface = d < 1.0;
    const float4 albedo = surface ? captureAlbedo.Load(from) : float4(0, 0, 0, 0);
    const float3 emissive = surface ? captureEmissive.Load(from).rgb : float3(0, 0, 0);
    RWTexture2D<float> depthAtlas = ResourceDescriptorHeap[P[1].z];
    RWTexture2D<float4> albedoAtlas = ResourceDescriptorHeap[P[1].w];
    RWTexture2D<float2> normalAtlas = ResourceDescriptorHeap[P[2].x];
    RWTexture2D<float3> emissiveAtlas = ResourceDescriptorHeap[P[2].y];
    depthAtlas[to] = surface ? min(d, 65534.0 / 65535.0) : 1.0;
    albedoAtlas[to] = albedo;
    normalAtlas[to] = surface ? captureNormal.Load(from) : float2(0.5, 0.5);
    emissiveAtlas[to] = emissive;

    RWTexture2D<float3> direct = ResourceDescriptorHeap[P[2].z];
    RWTexture2D<float3> indirect = ResourceDescriptorHeap[P[2].w];
    RWTexture2D<float3> final = ResourceDescriptorHeap[P[3].x];
    float3 e = 0, ei = 0;
    if (refresh)
    {
        e = direct[to];
        ei = indirect[to];
    }
    else
    {
        float keptFrames = 0;
        if (resample)
        {
            Texture2D<float4> resampledDirect = ResourceDescriptorHeap[P[4].x];
            Texture2D<float4> resampledIndirect = ResourceDescriptorHeap[P[4].y];
            const float4 rd = resampledDirect.Load(from), ri = resampledIndirect.Load(from);
            if (rd.a > 0 && surface)
            {
                e = rd.rgb;
                ei = ri.rgb;
            }
            keptFrames = min(ri.a, asfloat(P[4].w));
        }
        direct[to] = e;
        indirect[to] = ei;
        if (all(thread.xy == 0))
        {
            const uint2 atlasTile = to / MC_TILE;
            RWTexture2D<uint> frames = ResourceDescriptorHeap[P[3].y];
            frames[atlasTile] = (uint)keptFrames;
            RWByteAddressBuffer uniformBits = ResourceDescriptorHeap[P[3].z];
            const uint at = (atlasTile.x + atlasTile.y * (P[4].z / MC_TILE)) * CL_UNIFORM_BYTES;
            uniformBits.Store4(at, uint4(0, 0, 0, 0));
            uniformBits.Store4(at + 16, uint4(0, 0, 0, 0));
        }
    }
    const float3 irradiance = (e + ei) / CL_IRRADIANCE_SCALE;
    final[to] = surface ? (irradiance * mcDecodeAlbedo(albedo.rgb) / 3.14159265 + mcDecodeEmissive(emissive)) * CL_RADIANCE_SCALE : float3(0, 0, 0);
}
