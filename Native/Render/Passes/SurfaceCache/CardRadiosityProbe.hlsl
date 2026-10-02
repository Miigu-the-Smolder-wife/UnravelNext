// unx-kernel: cs_6_6 main
// r.card.radiosity.probe (CardLighting.hlsli): one thread per probe of the frame's radiosity list (4 of a listed tile).
// The probe's 16 rays' radiance, each filtered with the same ray of the four neighbour probes of its card (the
// reference's spatial filter, kernel 1: the probe's own weight 2, a neighbour 1 when it is valid and passes the plane
// weight), projected on 4 SH coefficients per colour (uniform hemisphere: each ray stands for 2 pi / 16 sr) and stored in
// the SH atlases at the probe's place (radiance x CL_RADIANCE_SCALE).
// P[0] = { card frame SRV, select SRV, frame index, 0 }
// P[4] = { trace atlas SRV, page light SRV (raw), 0, page capacity }
// P[5] = { direct list capacity, radiosity list capacity, 0, 0 }
// P[6] = { SH atlas UAV red, green, blue, 0 }
#include "Frame.hlsli"
#include "Passes/SurfaceCache/CardLighting.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    ByteAddressBuffer select = ResourceDescriptorHeap[P[0].y];
    const uint index = id.x >> 2, inTile = id.x & 3u;
    if (index >= min(select.Load(clSelectContext(1) + CL_SELECT_TILES), P[5].y)) return;
    const McFrame f = mcFrame(P[0].x);
    uint pageIndex;
    uint2 tile;
    clUnpackTile(select.Load(clTileListOffset(P[4].w, P[5].x, 1, index)), pageIndex, tile);
    const McCardPage page = mcLoadCardPage(f, pageIndex);
    const uint2 probeInPage = tile * (MC_TILE / CL_PROBE_SPACING) + uint2(inTile & 1u, inTile >> 1);
    if (any(float2(probeInPage * CL_PROBE_SPACING) >= page.sizeInTexels)) return;
    const McCard card = mcLoadCard(f, page.card);
    ByteAddressBuffer pageLight = ResourceDescriptorHeap[P[4].y];
    const int2 probeInCard = int2(clPageOriginInCard(page) / CL_PROBE_SPACING + probeInPage);
    const ClProbe probe = clProbeAt(f, card, page, pageIndex, pageLight, probeInCard);
    const uint2 atlasProbe = uint2(page.atlasRect.xy) / CL_PROBE_SPACING + probeInPage;
    RWTexture2D<float4> shR = ResourceDescriptorHeap[P[6].x];
    RWTexture2D<float4> shG = ResourceDescriptorHeap[P[6].y];
    RWTexture2D<float4> shB = ResourceDescriptorHeap[P[6].z];
    float4 r = 0, g = 0, b = 0;
    if (probe.valid)
    {
        Texture2D<float3> trace = ResourceDescriptorHeap[P[4].x];
        static const int2 kNeighbours[4] = { int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1) };
        uint2 neighbour[4];
        bool use[4];
        float weights = 2;
        uint k;
        for (k = 0; k < 4; ++k)
        {
            const ClProbe n = clProbeAt(f, card, page, pageIndex, pageLight, probeInCard + kNeighbours[k]);
            use[k] = n.valid && clPlaneWeight(probe.texel.position, probe.texel.normal, n.texel.position) > 0;
            neighbour[k] = n.atlasProbe;
            if (use[k]) weights += 1;
        }
        const uint temporalIndex = pageLight.Load(pageIndex * CL_PAGE_LIGHT_BYTES + 12);
        for (uint ray = 0; ray < CL_PROBE_RAYS * CL_PROBE_RAYS; ++ray)
        {
            const uint2 rayCoord = uint2(ray % CL_PROBE_RAYS, ray / CL_PROBE_RAYS);
            float3 radiance = 2 * trace.Load(int3(atlasProbe * CL_PROBE_RAYS + rayCoord, 0));
            for (k = 0; k < 4; ++k)
                if (use[k]) radiance += trace.Load(int3(neighbour[k] * CL_PROBE_RAYS + rayCoord, 0));
            radiance /= weights;
            const float4 basis = clShBasis(clProbeRayDirection(probe.texel.normal, atlasProbe, rayCoord, temporalIndex)) * (6.28318530718 / (CL_PROBE_RAYS * CL_PROBE_RAYS));
            r += basis * radiance.r;
            g += basis * radiance.g;
            b += basis * radiance.b;
        }
    }
    shR[atlasProbe] = r;
    shG[atlasProbe] = g;
    shB[atlasProbe] = b;
}
