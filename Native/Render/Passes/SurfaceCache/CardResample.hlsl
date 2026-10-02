// unx-kernel: cs_6_6 main
// r.card.resample (SurfaceCacheCards.cpp): a card that changes its resolution keeps its lighting - Unreal's
// ResampleLightingHistory (LumenSceneLighting.usf). It runs between the frame's capture and the upload of the frame's
// card records: the record buffers and the lighting atlases still hold the state of the frame before, so a texel of a
// re-allocated page reads the card's old pages through the old page table (mcCardSample: the highest level the card
// had there - a feedback page starts with the resident level's lighting, a resident level that changes keeps what
// its feedback pages held), bilinear over the old texels
// that had a surface. The result goes to two images laid out like the capture atlas; r.card.copy moves it into the
// page's new place after the records are uploaded.
// One group row per capture of the frame (group.y), 16 x 16 groups of 8 x 8 texels over a page of at most 128 x 128.
// P[0] = { captures SRV (raw, CC_CAPTURE_BYTES each), captures, card frame SRV, radiosity frames atlas SRV (R8_UINT, atlas / 8) }
// P[1] = { resampled direct UAV (RGBA16F: rgb lux x CL_IRRADIANCE_SCALE, a = 1 where the old card had the texel),
//          resampled indirect UAV (RGBA16F: rgb, a = the old tile's accumulated radiosity frames), 0, 0 }
#include "Passes/SurfaceCache/CardLighting.hlsli"
#include "Passes/SurfaceCache/CardCaptureList.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
    if (group.y >= P[0].y) return;
    const CcCapture cap = ccLoadCapture(P[0].x, group.y);
    if ((cap.flags & CC_FLAG_RESAMPLE) == 0) return;
    const uint2 p = uint2(group.x & 15u, group.x >> 4) * 8u + thread.xy;
    if (any(p >= cap.captureSize)) return;
    RWTexture2D<float4> outDirect = ResourceDescriptorHeap[P[1].x];
    RWTexture2D<float4> outIndirect = ResourceDescriptorHeap[P[1].y];
    const uint2 at = cap.captureOrigin + p;
    outDirect[at] = 0;
    outIndirect[at] = 0;
    const McFrame f = mcFrame(P[0].z);
    const McCard card = mcLoadCard(f, cap.card);
    const float2 uv = cap.cardUvRect.xy + (float2(p) + 0.5) / float2(cap.captureSize) * (cap.cardUvRect.zw - cap.cardUvRect.xy);
    const McCardSample s = mcCardSample(f, card, (uv * 2 - 1) * card.extent.xy, true);
    if (!s.valid) return;
    Texture2D<float> depth = ResourceDescriptorHeap[f.depth];
    Texture2D<float3> direct = ResourceDescriptorHeap[f.directLighting];
    Texture2D<float3> indirect = ResourceDescriptorHeap[f.indirectLighting];
    const int3 at0 = int3(s.texel00, 0);
    const float4 depths = float4(depth.Load(at0), depth.Load(at0, int2(1, 0)), depth.Load(at0, int2(0, 1)), depth.Load(at0, int2(1, 1)));
    const float4 w = select(depths < MC_DEPTH_NONE, s.weights, float4(0, 0, 0, 0));
    const float sum = w.x + w.y + w.z + w.w;
    if (!(sum > 0)) return;
    const float3 d = (w.x * direct.Load(at0) + w.y * direct.Load(at0, int2(1, 0)) + w.z * direct.Load(at0, int2(0, 1)) + w.w * direct.Load(at0, int2(1, 1))) / sum;
    const float3 e = (w.x * indirect.Load(at0) + w.y * indirect.Load(at0, int2(1, 0)) + w.z * indirect.Load(at0, int2(0, 1)) + w.w * indirect.Load(at0, int2(1, 1))) / sum;
    Texture2D<uint> frames = ResourceDescriptorHeap[P[0].w];
    outDirect[at] = float4(d, 1);
    outIndirect[at] = float4(e, (float)frames.Load(int3(s.tile, 0)));
}
