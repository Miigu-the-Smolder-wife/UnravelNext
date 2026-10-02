// unx-kernel: cs_6_6 main
// r.card.radiosity.integrate (CardLighting.hlsli): one group (8 x 8 threads) per tile of the frame's radiosity list, one
// thread per texel. The texel's indirect irradiance = the SH of the four probes around it (bilinear over the probes'
// jittered places, widened by one texel so none of the four has weight 0; a probe that is not valid or fails the plane
// weight is left out), evaluated for the texel's normal; blended into the indirect atlas over the tile's accumulated
// frames (alpha = 1 / frames, at most P[4].y frames). The final lighting atlas gets (direct + indirect) x albedo / pi +
// emission.
// P[0] = { card frame SRV, select SRV, frame index, 0 }
// P[4] = { page light SRV (raw), asuint(max frames accumulated), frames atlas UAV (R8_UINT, atlas / 8), page capacity }
// P[5] = { direct list capacity, radiosity list capacity, indirect atlas UAV, final atlas UAV }
// P[6] = { SH atlas SRV red, green, blue, direct atlas SRV }
#include "Frame.hlsli"
#include "Passes/SurfaceCache/CardLighting.hlsli"

groupshared float g_frames;

[numthreads(8, 8, 1)]
void main(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
    ByteAddressBuffer select = ResourceDescriptorHeap[P[0].y];
    const uint index = group.x;
    const bool listed = index < min(select.Load(clSelectContext(1) + CL_SELECT_TILES), P[5].y);
    const McFrame f = mcFrame(P[0].x);
    uint pageIndex = 0;
    uint2 tile = 0;
    if (listed) clUnpackTile(select.Load(clTileListOffset(P[4].w, P[5].x, 1, index)), pageIndex, tile);
    const McCardPage page = mcLoadCardPage(f, pageIndex);
    if (listed && all(thread.xy == 0))
    {
        RWTexture2D<uint> frames = ResourceDescriptorHeap[P[4].z];
        const uint2 atlasTile = uint2(page.atlasRect.xy) / MC_TILE + tile;
        const float n = min((float)frames[atlasTile] + 1, max(asfloat(P[4].y), 1.0));
        frames[atlasTile] = (uint)n;
        g_frames = n;
    }
    GroupMemoryBarrierWithGroupSync();
    if (!listed) return;
    const uint2 coord = tile * MC_TILE + thread.xy;
    if (any(float2(coord) >= page.sizeInTexels)) return;
    const McCard card = mcLoadCard(f, page.card);
    const McTexel texel = mcPageTexel(f, page, card, coord);
    RWTexture2D<float3> indirect = ResourceDescriptorHeap[P[5].z];
    RWTexture2D<float3> final = ResourceDescriptorHeap[P[5].w];
    if (!texel.valid)
    {
        indirect[texel.atlas] = 0;
        final[texel.atlas] = 0;
        return;
    }
    ByteAddressBuffer pageLight = ResourceDescriptorHeap[P[4].x];
    const uint temporalIndex = pageLight.Load(pageIndex * CL_PAGE_LIGHT_BYTES + 12);
    const uint2 coordInCard = clPageOriginInCard(page) + coord;
    const uint2 fromProbe = uint2(max(int2(coordInCard) - int2(clProbeTexelOffset(temporalIndex)), 0));
    const uint2 probe00 = fromProbe / CL_PROBE_SPACING;
    const float2 bilinear = (float2(fromProbe - probe00 * CL_PROBE_SPACING) + 1.0) / (CL_PROBE_SPACING + 2.0);
    const float4 weights = float4((1 - bilinear.x) * (1 - bilinear.y), bilinear.x * (1 - bilinear.y), (1 - bilinear.x) * bilinear.y, bilinear.x * bilinear.y);
    Texture2D<float4> shR = ResourceDescriptorHeap[P[6].x];
    Texture2D<float4> shG = ResourceDescriptorHeap[P[6].y];
    Texture2D<float4> shB = ResourceDescriptorHeap[P[6].z];
    float4 r = 0, g = 0, b = 0;
    float sum = 0;
    for (uint k = 0; k < 4; ++k)
    {
        const ClProbe probe = clProbeAt(f, card, page, pageIndex, pageLight, int2(probe00 + uint2(k & 1u, k >> 1)));
        if (!probe.valid) continue;
        const float w = min(weights[k], clPlaneWeight(texel.position, texel.normal, probe.texel.position));
        if (!(w > 0)) continue;
        const int3 at = int3(probe.atlasProbe, 0);
        r += w * shR.Load(at);
        g += w * shG.Load(at);
        b += w * shB.Load(at);
        sum += w;
    }
    float3 e = 0;
    if (sum > 0)
    {
        const float4 transfer = clShIrradianceTransfer(texel.normal) / (sum * CL_RADIANCE_SCALE);
        e = max(float3(dot(r, transfer), dot(g, transfer), dot(b, transfer)), 0.0);
    }
    if (any(isnan(e)) || any(isinf(e))) e = 0;
    const float3 before = indirect[texel.atlas] / CL_IRRADIANCE_SCALE;
    e = lerp(before, e, 1 / max(g_frames, 1.0));
    indirect[texel.atlas] = e * CL_IRRADIANCE_SCALE;
    Texture2D<float3> direct = ResourceDescriptorHeap[P[6].w];
    Texture2D<float4> albedo = ResourceDescriptorHeap[f.albedo];
    Texture2D<float3> emissive = ResourceDescriptorHeap[f.emissive];
    const int3 at = int3(texel.atlas, 0);
    const float3 irradiance = e + direct.Load(at) / CL_IRRADIANCE_SCALE;
    final[texel.atlas] = (irradiance * mcDecodeAlbedo(albedo.Load(at).rgb) / 3.14159265 + mcDecodeEmissive(emissive.Load(at))) * CL_RADIANCE_SCALE;
}
