// unx-kernel: cs_6_6 main
// r.card.direct.cull (CardLighting.hlsli): one thread per tile of the frame's direct list. The tile's valid texels, its
// CL_LIGHTS strongest lights (the reference's MaxLightsPerTile: by unshadowed importance at the tile's mean point,
// lights wholly behind every texel's horizon left out) and, per light, whether its visibility was uniform over the tile
// at the tile's last update (then the trace takes one ray per 2 x 2 texels). No ray. The tile's visible bits are cleared.
// P[0] = { card frame SRV, select SRV, frame index, flags (bit 7: no local lights, bit 8: no sun) }
// P[4] = { tile lights UAV (raw), tile shadow UAV (raw), uniform bits SRV (raw), page capacity }
// P[5] = { direct list capacity, 0, 0, 0 }
// P[6], P[7] = RtSceneSrvs (the light grid: word 7)
#include "RayTracing/HitLocalLights.hlsli"
#include "Passes/SurfaceCache/CardLighting.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    ByteAddressBuffer select = ResourceDescriptorHeap[P[0].y];
    const uint listed = min(select.Load(clSelectContext(0) + CL_SELECT_TILES), P[5].x);
    const uint index = id.x;
    if (index >= listed) return;
    const McFrame f = mcFrame(P[0].x);
    RWByteAddressBuffer tileLights = ResourceDescriptorHeap[P[4].x];
    RWByteAddressBuffer tileShadow = ResourceDescriptorHeap[P[4].y];
    uint k;
    for (k = 0; k < CL_TILE_SHADOW_BYTES; k += 4) tileShadow.Store(index * CL_TILE_SHADOW_BYTES + k, 0u);
    const uint base = index * CL_TILE_LIGHT_BYTES;
    for (k = 0; k < CL_TILE_LIGHT_BYTES; k += 4) tileLights.Store(base + k, k < CL_LIGHTS * 4 ? MC_NONE : 0u);

    uint pageIndex;
    uint2 tile;
    clUnpackTile(select.Load(clTileListOffset(P[4].w, P[5].x, 0, index)), pageIndex, tile);
    const McCardPage page = mcLoadCardPage(f, pageIndex);
    const McCard card = mcLoadCard(f, page.card);
    // the tile's valid texels, their mean point and normal, the normals' widest angle from the mean
    uint2 valid = 0;
    float3 sumP = 0, sumN = 0;
    float count = 0;
    uint t;
    for (t = 0; t < 64; ++t)
    {
        const McTexel texel = mcPageTexel(f, page, card, tile * MC_TILE + uint2(t & 7u, t >> 3));
        if (!texel.valid) continue;
        if (t < 32) valid.x |= 1u << t;
        else valid.y |= 1u << (t - 32);
        sumP += texel.position;
        sumN += texel.normal;
        count += 1;
    }
    tileLights.Store2(base + 32, valid);
    if (count == 0) return;
    const float3 position = sumP / count;
    const float3 cardNormal = mcCardToWorldVector(card, float3(0, 0, 1));
    const float3 normal = dot(sumN, sumN) > 1e-6 ? normalize(sumN) : cardNormal;
    float minCos = 1;
    for (t = 0; t < 64; ++t)
    {
        if (((t < 32 ? valid.x >> t : valid.y >> (t - 32)) & 1u) == 0) continue;
        const McTexel texel = mcPageTexel(f, page, card, tile * MC_TILE + uint2(t & 7u, t >> 3));
        minCos = min(minCos, dot(texel.normal, normal));
    }
    const float slack = sqrt(saturate(1 - minCos * minCos)) + 0.05;  // sine of the widest angle (+ the stored normals' precision)

    const RtSceneSrvs scene = rtSceneSrvs(P[6], P[7]);
    uint chosen[CL_LIGHTS];
    float weight[CL_LIGHTS];
    uint held = 0;
    if (scene.pad != 0xFFFFFFFFu && (P[0].w & 128u) == 0)
    {
        g_rtLightData = scene.pad;
        ByteAddressBuffer lights = ResourceDescriptorHeap[scene.pad];
        const RtLightGrid grid = lights.Load<RtLightGrid>(0);
        const uint gridCell = rtLightCell(grid, position);
        if (gridCell != ~0u)
        {
            const uint k0 = rtLightCellStart(gridCell), k1 = rtLightCellStart(gridCell + 1);
            [loop] for (uint g = k0; g < k1; ++g)
            {
                const uint li = rtLightCellLight(g);
                const RtLight l = rtLightFetch(li);
                float w = rtLightImportance(l, position);
                if (!(w > 0)) continue;
                // (a light below the horizon of every texel of the tile lights none of them)
                const float3 d = l.position - position;
                const float dist = sqrt(max(dot(d, d), 1e-12));
                const float extent = l.type == kRtLightRect ? 0.5 * length(l.size)
                                   : l.type == kRtLightDisk || l.type == kRtLightSphere ? l.size.x
                                   : l.type == kRtLightTube ? 0.5 * l.size.x + l.size.y
                                   : 0.0;
                w *= saturate(dot(normal, d) / dist + min(extent / dist, 1.0) + slack);
                if (!(w > 0)) continue;
                uint at = held;
                if (held == CL_LIGHTS)
                {
                    if (w <= weight[CL_LIGHTS - 1]) continue;
                    at = CL_LIGHTS - 1;
                }
                else ++held;
                [loop] while (at > 0 && weight[at - 1] < w)
                {
                    weight[at] = weight[at - 1];
                    chosen[at] = chosen[at - 1];
                    --at;
                }
                weight[at] = w;
                chosen[at] = li;
            }
        }
    }
    // the uniform bits of the tile's last update
    ByteAddressBuffer uniformBits = ResourceDescriptorHeap[P[4].z];
    const uint2 atlasTile = (uint2(page.atlasRect.xy) + tile * MC_TILE) / MC_TILE;
    const uint uniformBase = (atlasTile.x + atlasTile.y * (f.atlasSize / MC_TILE)) * CL_UNIFORM_BYTES;
    uint uniformSlots = 0;
    for (k = 0; k < held; ++k)
    {
        tileLights.Store(base + k * 4, chosen[k]);
        const uint bit = chosen[k] & 255u;
        if ((uniformBits.Load(uniformBase + (bit >> 5) * 4) >> (bit & 31u)) & 1u) uniformSlots |= 1u << k;
    }
    const bool sun = (P[0].w & 256u) == 0 && dot(normal, normalize(g_sunDirection)) > -slack;
    if (sun && ((uniformBits.Load(uniformBase + (CL_SUN_BIT >> 5) * 4) >> (CL_SUN_BIT & 31u)) & 1u) != 0) uniformSlots |= 1u << CL_SUN_SLOT;
    tileLights.Store2(base + 40, uint2(uniformSlots, sun ? 1u : 0u));
}
