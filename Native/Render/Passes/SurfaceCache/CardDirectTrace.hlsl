// unx-kernel: lib_6_6 main
// r.card.direct.trace (CardLighting.hlsli): one ray generation thread per (listed tile, light slot, texel), at most ONE
// shadow ray and nothing alive across it but the bit's address - the structure of r.sc.pairs.trace (SurfaceCacheLightPairs
// .hlsli). The dispatch is a band of at most 262,144 threads. A thread without a ray returns at once: no light in the
// slot, a light that casts no shadow (the store takes it as visible), no surface in the texel, the texel facing away, or
// - when the light's visibility was uniform over the tile at its last update - a texel that is not the first of its
// 2 x 2 (the store copies that one's bit).
// A light's ray: from the texel's point moved by the bias to the light's side, to the light's centre less its radius
// and end bias, shadow casters only. The sun's: toward its centre, TMin 0, the GI mask.
// P[0] = { card frame SRV, select SRV, frame index, flags (bit 10: lights without their shadow rays, bit 11: alpha-tested
//          casters taken as opaque) }
// P[1].w = the sun ray's length
// P[4] = { tile lights SRV (raw), tile shadow UAV (raw), the dispatch's first thread, page capacity }
// P[5] = { direct list capacity, asuint(default ray end bias, m), 0, 0 }
// P[6], P[7] = RtSceneSrvs
#include "RayTracing/RayShaders.hlsli"
#include "Passes/SurfaceCache/CardLighting.hlsli"

[shader("raygeneration")]
void CardDirectTraceGen()
{
    const uint thread = DispatchRaysIndex().x + P[4].z;
    const uint index = thread / CL_TRACE_THREADS, rest = thread % CL_TRACE_THREADS;
    const uint slot = rest >> 6, t = rest & 63u;
    ByteAddressBuffer select = ResourceDescriptorHeap[P[0].y];
    if (index >= min(select.Load(clSelectContext(0) + CL_SELECT_TILES), P[5].x)) return;
    ByteAddressBuffer tileLights = ResourceDescriptorHeap[P[4].x];
    const uint base = index * CL_TILE_LIGHT_BYTES;
    const uint4 state = tileLights.Load4(base + 32);  // valid mask (2), uniform slots, sun
    if ((((t < 32 ? state.x >> t : state.y >> (t - 32))) & 1u) == 0) return;
    if (((state.z >> slot) & 1u) != 0 && (t & 9u) != 0) return;  // (uniform: the texels with even x and even y alone)
    const bool sun = slot == CL_SUN_SLOT;
    uint light = MC_NONE;
    if (sun)
    {
        if (state.w == 0) return;
    }
    else
    {
        light = tileLights.Load(base + slot * 4);
        if (light == MC_NONE) return;
    }
    const McFrame f = mcFrame(P[0].x);
    uint pageIndex;
    uint2 tile;
    clUnpackTile(select.Load(clTileListOffset(P[4].w, P[5].x, 0, index)), pageIndex, tile);
    const McCardPage page = mcLoadCardPage(f, pageIndex);
    const McCard card = mcLoadCard(f, page.card);
    const McTexel texel = mcPageTexel(f, page, card, tile * MC_TILE + uint2(t & 7u, t >> 3));
    if (!texel.valid) return;
    const float bias = 1e-3 + 2e-4 * distance(texel.position, g_cameraPosition);
    RayDesc ray;
    uint mask;
    if (sun)
    {
        ray.Direction = normalize(g_sunDirection);
        if (!(dot(texel.normal, ray.Direction) > 0)) return;
        ray.Origin = texel.position + texel.normal * bias;
        ray.TMin = 0;
        ray.TMax = asfloat(P[1].w);
        mask = RT_MASK_HIT_SHADOW;  // (the sun's casters, as the view's shadow maps: RayScene.hlsli)
    }
    else
    {
        const GpuLight g = loadLight(light);
        if (!lightCastsShadow(g) || (P[0].w & 1024u) != 0) return;
        const float3 d = g.position - texel.position;
        const float dist = length(d);
        if (!(dist > 0)) return;
        const uint type = lightType(g);
        if (type <= LIGHT_SPOT && !(dot(texel.normal, d) > 0)) return;  // (a punctual light behind the texel: no light)
        const float radius = type == LIGHT_SPHERE ? g.size.x : type == LIGHT_TUBE ? g.size.y : 0.0;
        const float reach = dist - radius - lightRayEndBias(g, asfloat(P[5].y));
        ray.Direction = d / dist;
        ray.Origin = texel.position + texel.normal * (dot(texel.normal, ray.Direction) < 0 ? -bias : bias);
        ray.TMin = bias;
        ray.TMax = reach > bias ? reach : bias;
        mask = RT_MASK_SHADOW;
    }
    const float dd = dot(ray.Direction, ray.Direction);
    if (!(all(abs(ray.Origin) < 1e9) && dd > 0.98 && dd < 1.02 && ray.TMin >= 0 && ray.TMax >= ray.TMin && ray.TMax < 1e30)) return;
    const uint rayFlags = (P[0].w & 2048u) != 0 ? RAY_FLAG_FORCE_OPAQUE : RAY_FLAG_NONE;
    if (rtVisible(rtScene(), ray, mask, rayFlags))
    {
        RWByteAddressBuffer tileShadow = ResourceDescriptorHeap[P[4].y];
        tileShadow.InterlockedOr(index * CL_TILE_SHADOW_BYTES + slot * 8 + (t >> 5) * 4, 1u << (t & 31u));
    }
}
