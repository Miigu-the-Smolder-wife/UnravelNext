// unx-kernel: cs_6_6 main
// unx-variants: OUTPUT=0,1 FALLBACK=0,1 AREA=0,1
// Shading kernel of the opaque classes (ARCHITECTURE 2.11; INTERFACES 5.6, 7, 8): one 8 x 8 tile of the class's tile
// list per group (ExecuteIndirect), pixels of other classes skipped. Per pixel, from the G-buffer, depth and material
// word (no vis-buffer re-derivation):
//   sun      model BRDF over the solar disk (ShadingCommon.hlsli shSunSpecular), S's shadow slot 0, S's transmittance;
//   GI       R's screen probes: diffuse irradiance x near occlusion; Foliage transmission from the back hemisphere;
//   specular R's reflection (G/M paths, planar mirrors) or the K path screenProbeRadiance with the lobe half-angle,
//            times the model's specular directional albedo;
//   emission material (x emissive texture, resolved per pixel);
//   air      S's air volume between the camera and the surface (main view, atmosphereAirView: atmosphere, casters'
//            shadows in the air, local lights' air) and the sun's illuminance at the surface;
// then exposure, tone map and the final 4 B (OUTPUT=0) or linear radiance x exposure (OUTPUT=1).
// Classes without their own model yet (Subsurface, Water: INTERFACES 8.1 defines them before P3/P4) use this kernel.
// Local lights: the pixel's froxel list (S, Froxel.hlsli), punctual lights exactly (INTERFACES 8.2); shadow-casting
// lights take S's slots 1-3 in list order (7.3); those past the third read S's overflow list (7.3, v1.20: tile head,
// pixel record, 8-bit run), loads only. Tiles over the list's capacity are left to FALLBACK=1, which runs on S's fallback
// tile list, shades every non-sky class there and evaluates the lights past the third with S's VSM directly (outside
// the main kernel, so its registers stay as they are). Area lights: AreaLight.hlsli, compiled in (AREA=1) only for scenes
// that have area lights (their registers lowered occupancy for every pixel otherwise: +0.23 ms at city 4K, measured). Every view uses its own froxel lists
// and air volume (v1.22; planar reflection views get them from S), taken by whether the SRVs are present.
// P[0] = { gbuffer, depth, material word, color UAV }
// P[1] = { tile lists (raw), list offset (entries), shade class (FALLBACK: 0xFFFFFFFF, every non-sky class), emissive or
//        UNX_NONE }
// P[2] = { shadow visibility, screen probes, reflection, GI cache (planar views) } (UNX_NONE = absent)
// P[3] = { atmosphere transmittance, multi-scatter, S's shadow overflow tile heads (main kernel; UNX_NONE = absent), this
//        view's air volume } (this kernel reads no sky view)
// P[4] = { specular albedo LUT (float2 per grid point), texture table, experiment mask (0; shading.toml), vis id SRV }
// P[5] = { froxel lights (raw) (UNX_NONE = absent), LTC table (StructuredBuffer<float4>, AreaLight.hlsli) }
// P[6] = { edge cos angle, edge footprint tolerance, edge distance tolerance (floats), edge args UAV (raw) }
// P[7] = { edge radiance UAV (RGBA16F), R's screen probe maps (K path; UNX_NONE = absent), S's shadow overflow list (raw;
//        FALLBACK: a raw buffer holding this frame's ShadowSrvs), edge pixel list UAV (raw) }
#include "Bindless.hlsli"
#include "GBuffer.hlsli"
#include "Passes/Material/MaterialInternal.hlsli"
#include "Passes/Material/MaterialSurface.hlsli"
#include "Passes/Shading/ShadingCommon.hlsli"
#include "Passes/Shading/Edge.hlsli"
#include "Passes/Shading/AreaLight.hlsli"
#include "Passes/Atmosphere/Atmosphere.hlsli"
#include "Passes/Shadow/ShadowVisibility.hlsli"
#include "Passes/Atmosphere/Froxel.hlsli"
#include "Passes/GI/ScreenProbes.hlsli"
#include "Passes/GI/GiCache.hlsli"
#include "Passes/Reflection/Reflection.hlsli"

struct ShadedPixel
{
    float3 radiance;  // linear, before exposure
    float linearZ;
    float3 normal;    // shading normal (G-buffer)
};

ShadedPixel shadeSurface(uint2 pixel, uint word, uint materialIndex, GpuMaterial m, Texture2D<uint> words, Texture2D<uint2> gbuffer, Texture2D<float> depthTex,
                         uint overflowHead);

// Edge detection shares the tile's samples: each lane publishes its vis id and edge sample, so a neighbour inside the
// tile costs no loads; neighbours outside it are read from the screen buffers. A lane of another class (or the sky)
// shows another material, which is an edge before any geometry.
#define EDGE_OUTSIDE 0xFFFFFFFFu
groupshared uint gsVis[64];
groupshared uint gsMaterial[64];  // EDGE_OUTSIDE: outside the view
groupshared float3 gsPosition[64];
groupshared float3 gsNormal[64];
groupshared float gsFootprint[64];

[numthreads(8, 8, 1)]
void main(uint3 gid : SV_GroupID, uint2 tid : SV_GroupThreadID)
{
    ByteAddressBuffer tiles = ResourceDescriptorHeap[P[1].x];
    const uint tile = tiles.Load(4 * (P[1].y + gid.x));
    const uint2 tileCoord = uint2(tile & 0xFFFFu, tile >> 16);
    const uint2 pixel = tileCoord * M_TILE + tid;
    uint overflowHead = 0;  // S's overflow tile head (7.3): 0 none, 1 + block start
#if !FALLBACK
    if (P[3].z != UNX_NONE)
    {
        Texture2D<uint> heads = ResourceDescriptorHeap[P[3].z];
        overflowHead = heads[tileCoord];
        if (overflowHead == 0xFFFFFFFFu) return;  // over the list's capacity: the fallback kernel shades this tile
    }
#endif
    Texture2D<uint> words = ResourceDescriptorHeap[P[0].z];
    Texture2D<uint2> gbuffer = ResourceDescriptorHeap[P[0].x];
    Texture2D<float> depthTex = ResourceDescriptorHeap[P[0].y];
    Texture2D<uint> visIds = ResourceDescriptorHeap[P[4].w];
    const bool inView = all(pixel < uint2(g_viewWidth, g_viewHeight));
    const uint word = inView ? words[pixel] : M_MATERIAL_SKY;
    const uint materialIndex = mWordMaterial(word);
    bool active = inView && materialIndex != M_MATERIAL_SKY;
    const GpuMaterial m = loadMaterial(active ? materialIndex : 0);
    active = active && (P[1].z == 0xFFFFFFFFu || mShadeClass(materialClass(m)) == P[1].z);
    ShadedPixel sp = (ShadedPixel)0;
    if (active) sp = shadeSurface(pixel, word, materialIndex, m, words, gbuffer, depthTex, overflowHead);

    // ---- edge (E) pixels keep their exposed linear radiance for the composite (EdgeComposite.hlsl)
    if (P[7].x == UNX_NONE || (P[4].z & 256) != 0) return;
    const uint lane = tid.y * M_TILE + tid.x;
    const EdgePixel c = edgeSample(pixel, materialIndex, sp.linearZ, sp.normal);
    const uint vc = inView ? visIds[pixel] : VIS_NONE;
    gsVis[lane] = vc;
    gsMaterial[lane] = inView ? materialIndex : EDGE_OUTSIDE;
    gsPosition[lane] = c.position;
    gsNormal[lane] = c.normal;
    gsFootprint[lane] = c.footprint;
    GroupMemoryBarrierWithGroupSync();
    bool isEdge = false;
    if (active)
    {
        const EdgeParams ep = { asfloat(P[6].x), asfloat(P[6].y), asfloat(P[6].z) };
        [unroll] for (uint k = 0; k < 9; ++k)
        {
            if (k == 4 || isEdge) continue;
            const int2 o = int2(int(k % 3) - 1, int(k / 3) - 1);
            const int2 q = int2(pixel) + o;
            if (any(q < 0) || q.x >= int(g_viewWidth) || q.y >= int(g_viewHeight)) continue;
            const int2 t = int2(tid) + o;
            EdgePixel e = (EdgePixel)0;
            if (all(t >= 0) && all(t < int(M_TILE)))
            {
                const uint l = uint(t.y) * M_TILE + uint(t.x);
                if (gsVis[l] == vc) continue;
                e.material = gsMaterial[l];
                e.sky = false;
                e.position = gsPosition[l];
                e.normal = gsNormal[l];
                e.footprint = gsFootprint[l];
            }
            else
            {
                if (visIds[uint2(q)] == vc) continue;
                e.material = mWordMaterial(words[uint2(q)]);
                if (e.material == c.material) e = edgeSample(uint2(q), e.material, linearDepth(depthTex[uint2(q)]), octDecode(gbuffer[uint2(q)].x));
            }
            isEdge = e.material != c.material || !edgeSameSurface(c, e, ep);
        }
        if (isEdge)
        {
            RWTexture2D<float4> edgeRadiance = ResourceDescriptorHeap[P[7].x];
            edgeRadiance[pixel] = float4(sp.radiance * g_exposure, 1);
        }
    }
    edgeAppendPixel(pixel, isEdge, P[7].w, P[6].w);
}

// Shades one pixel of this class (writes the output) and returns what edge detection needs.
// Visibility of the shadow-casting light at 'ordinal' (> 3) of the pixel's list order: S's overflow run (main kernel) or
// S's VSM estimator (fallback tiles). 1 without S's resources. 'record' caches the pixel's overflow record (main kernel)
// or marks the receiver as built (fallback: 'receiver', built once per pixel).
float shOverflowVisibility(uint2 pixel, uint ordinal, uint overflowHead, inout uint record, inout ShadowPixelReceiver receiver, uint lightIndex)
{
#if FALLBACK
    // S's own evaluation for the slots and the overflow list (the visibility pass's receiver: position, normal and
    // footprint of the pixel), quantised to 8 bits as S stores it: fallback tiles equal the list bit for bit.
    if (P[7].z == UNX_NONE) return 1;
    if (record == 0xFFFFFFFFu)
    {
        receiver = shadowPixelReceiver(pixel, P[0].y, P[0].x);
        record = 0;
    }
    if (receiver.valid == 0) return 1;
    ByteAddressBuffer b = ResourceDescriptorHeap[P[7].z];
    const uint4 a = b.Load4(0), c = b.Load4(16);
    ShadowSrvs vsm;
    vsm.pageTable = a.x;
    vsm.pool = a.y;
    vsm.blocks = a.z;
    vsm.searchBound = a.w;
    vsm.constants = c.x;
    vsm.lights = c.y;
    vsm.pad0 = c.z;
    vsm.pad1 = c.w;
    return round(saturate(shadowLocalVisibilityAtReceiver(vsm, lightIndex, receiver)) * 255.0) / 255.0;
#else
    if (overflowHead == 0 || P[7].z == UNX_NONE) return 1;
    ByteAddressBuffer b = ResourceDescriptorHeap[P[7].z];
    const uint block = overflowHead - 1;
    if (record == 0xFFFFFFFFu) record = b.Load(4 * (block + (pixel.y % M_TILE) * M_TILE + (pixel.x % M_TILE)));
    const uint j = ordinal - 4;
    if (j >= (record >> 24)) return 1;
    const uint w = b.Load(4 * (block + (record & 0xFFFFFFu) + j / 4));
    return ((w >> (8 * (j & 3))) & 0xFFu) / 255.0;
#endif
}

ShadedPixel shadeSurface(uint2 pixel, uint word, uint materialIndex, GpuMaterial m, Texture2D<uint> words, Texture2D<uint2> gbuffer, Texture2D<float> depthTex,
                         uint overflowHead)
{
    const GBufferSample g = decodeGBuffer(gbuffer[pixel]);
    const float linearZ = linearDepth(depthTex[pixel]);
    float3 D, Dx, Dy;
    mPixelRay(float2(pixel) + 0.5, D, Dx, Dy);
    const float3 offset = D * linearZ;  // camera-relative position (D has unit depth along the view axis)
    const float3 worldPos = g_cameraPosition + offset;
    const float3 v = -normalize(D);
    const float3 n = g.normal;
    const float NoV = dot(n, v);

    ModelSurface s;
    s.cls = materialClass(m);
    s.baseColor = g.baseColor;
    s.roughness = g.roughness;
    s.metallic = mWordMetallic(word);
    s.specular = m.specular;
    s.transmission = m.transmission;
    const float3 diffuse = s.baseColor * ((1 - s.metallic) / SH_PI);  // Lambert f_d before the Foliage split
    const float3 f0 = modelF0(s);
    const float alpha = modelAlpha(s.roughness);
    const bool foliage = s.cls == MATERIAL_FOLIAGE;
    const float3 front = foliage ? diffuse * (1 - s.transmission) : diffuse;
    const float3 back = foliage ? diffuse * s.transmission : 0;

    AtmosphereSrvs atm;
    atm.transmittance = P[3].x;
    atm.multiScatter = P[3].y;
    atm.skyView = UNX_NONE;  // P[3].z carries the shadow overflow heads: this kernel reads no sky view
    atm.aerial = P[3].w;
    const bool haveAtmosphere = atm.transmittance != UNX_NONE;

    float3 radiance = m.emissive;
    if (P[1].w != UNX_NONE && mLoadTextureSet(P[4].y, materialIndex).emissive != UNX_NONE)
    {
        Texture2D<float4> emissive = ResourceDescriptorHeap[P[1].w];
        radiance = emissive[pixel].rgb;  // material emissive x texture, resolved at the footprint
    }

    // ---- sun (INTERFACES 8.1: reflection on the viewer's side of the shading normal, Foliage transmission across it)
    const float3 l0 = normalize(g_sunDirection);
    // S's air volume of this view (v1.22; planar views: integrated from the mirror plane on) gives the air between the
    // camera and the surface and the sun's illuminance at it in one lookup (atmosphereAirView); without one, the sun's
    // transmittance alone.
    float3 E = g_sunIlluminance * g_sunColor, airInscatter = 0, airTransmittance = 1;
    if (haveAtmosphere)
    {
        if (atm.aerial != UNX_NONE && (P[4].z & 8) == 0)
            atmosphereAirView(atm, (float2(pixel) + 0.5) / float2(g_viewWidth, g_viewHeight), linearZ, airInscatter, airTransmittance, E);
        else E = atmosphereSunIlluminance(atm, worldPos);
    }
    float sunVisibility = 1;
    if (P[2].x != UNX_NONE)
    {
        Texture2D<uint> shadow = ResourceDescriptorHeap[P[2].x];
        sunVisibility = shadowSlot(shadow[pixel], 0);
    }
    const float NoL = dot(n, l0);
    const uint experiment = P[4].z;
    if (sunVisibility > 0 && (experiment & 16) == 0)
    {
        const float3 cap = E * (2 / (1 + cos(g_sunAngularRadius)));  // L_sun x solid angle of the disk
        // The disk's parts above and below the shading normal's horizon (cap-averaged clipped cosines).
        const float above = shCapCosine(NoL), below = shCapCosine(-NoL);
        float3 sun = 0;
        if (NoV > 0)
        {
            if (above > 0)
            {
                const float e = modelDirectionalAlbedo(NoV, s.roughness);
                const float3 compensation = 1 + f0 * (1 / e - 1);
                sun = front * above * cap;
                if (experiment & 1) sun += NoL > 0 ? shSpecular(f0, alpha, compensation, n, v, l0, NoV, NoL) * NoL * cap : 0;
                else sun += shSunSpecular(P[4].x, f0, s.roughness, alpha, compensation, n, v, NoV, l0, E, shPixelAngle(D, Dx));
            }
            if (foliage) sun += back * below * cap;
        }
        else if (foliage) sun = back * above * cap;  // viewer behind the shading normal: only light crossing the leaf
        radiance += sun * sunVisibility;
    }

    // ---- local lights (main view: S's froxel lists)
    FroxelSrvs froxels;
    froxels.lights = P[5].x;
    froxels.lightIndices = P[5].x;
    froxels.scattering = UNX_NONE;
    froxels.pad = 0;
    if (froxels.lights != UNX_NONE && (experiment & 32) == 0)  // this view's lists (v1.22)
    {
        uint shadowPacked = 0xFFFFFFFFu;  // all slots lit when S publishes no visibility
        if (P[2].x != UNX_NONE)
        {
            Texture2D<uint> shadow = ResourceDescriptorHeap[P[2].x];
            shadowPacked = shadow[pixel];
        }
        const float e = modelDirectionalAlbedo(max(NoV, 1e-4), s.roughness);
        const float3 compensation = 1 + f0 * (1 / e - 1);
        const uint2 range = froxelLightRange(froxels, pixel, linearZ);
#if AREA
        // Area lights (AreaLight.hlsli): the shading frame, its horizon-flipped twin for Foliage transmission and the
        // LTC transform of the specular lobe, formed once per pixel.
        const float3x3 frame = shShadingFrame(n, v, NoV);
        const float3x3 frameBack = float3x3(frame[0], -frame[1], -frame[2]);
        const float3x3 specular = mul(shLtcInverse(P[5].y, max(NoV, 1e-4), s.roughness), frame);
        const float3 specularAlbedo = shSpecularAlbedo(P[4].x, f0, max(NoV, 1e-4), s.roughness);
#endif
        uint shadowOrdinal = 0, overflowRecord = 0xFFFFFFFFu;
        ShadowPixelReceiver overflowReceiver = (ShadowPixelReceiver)0;
        for (uint i = 0; i < range.y; ++i)
        {
            const uint lightIndex = froxelLight(froxels, range.x + i);
            const GpuLight light = loadLight(lightIndex);
            float visibility = 1;
            if (lightCastsShadow(light))
            {
                ++shadowOrdinal;
                visibility = shadowOrdinal <= 3 ? shadowSlot(shadowPacked, shadowOrdinal)
                                                : shOverflowVisibility(pixel, shadowOrdinal, overflowHead, overflowRecord, overflowReceiver, lightIndex);
            }
            if (visibility <= 0) continue;
            if (lightType(light) > LIGHT_SPOT)
            {
#if AREA
                // L w (f_d pi I + E_s I_ltc) on the viewer's side of n; Foliage transmits what arrives on the other.
                const float3 p = (light.position - g_cameraPosition) - offset;
                const float3 Lw = light.color * (light.intensity * shAreaWindow(light, p) * visibility);
                // Integrals in order: front diffuse, specular, back (Foliage) -- one inlined evaluator.
                const uint first = NoV > 0 ? 0 : 2, last = foliage ? 3 : 2;
                [loop] for (uint j = first; j < last; ++j)
                {
                    const float3x3 T = j == 0 ? frame : (j == 1 ? specular : (NoV > 0 ? frameBack : frame));
                    const float I = shAreaIntegral(light, p, T);
                    radiance += Lw * (j == 0 ? front * (SH_PI * I) : (j == 1 ? specularAlbedo * I : back * (SH_PI * I)));
                }
#endif
                continue;  // AREA=0: the scene has no area lights (ShadingSystem)
            }
            float3 l;
            const float3 E = shPunctualIlluminance(light, (light.position - g_cameraPosition) - offset, l);
            const float cosL = dot(n, l);
            float3 f = 0;
            if (NoV > 0 && cosL > 0) f = front + shSpecular(f0, alpha, compensation, n, v, l, NoV, cosL);
            else if (foliage && NoV * cosL < 0) f = back;
            radiance += f * E * (abs(cosL) * visibility);
        }
    }

    // ---- indirect (R): screen probes (main view) or the world cache (planar views). The viewer's side of the shading
    // normal reflects; Foliage also transmits what arrives on the other side.
    const float3 nv = NoV > 0 ? n : -n;
    const float3 r = reflect(-v, n);
    const float halfAngle = reflectionLobeHalfAngle(s.roughness, NoV);
    float3 irradiance = 0, irradianceBack = 0, incident = 0;
    if (g_viewKind == VIEW_MAIN && P[2].y != UNX_NONE && (experiment & 6) != 6)
    {
        // One probe footprint for the irradiance (both sides for Foliage) and, where R's reflection has no G/M result,
        // the K-path radiance of the lobe (R's hardware-filtered maps).
        ProbeSrvs probes;
        probes.probes = P[2].y;
        probes.occlusion = P[2].y;
        probes.pad0 = P[7].y;
        probes.pad1 = 0;
        const bool specular = NoV > 0 && (experiment & 4) == 0;
        const float4 refl = specular && P[2].z != UNX_NONE ? reflectionRadiance(P[2].z, pixel) : float4(0, 0, 0, 0);
        const bool wantRadiance = specular && refl.a <= 0;
        if ((experiment & 2) == 0 || wantRadiance)
        {
            const ScreenProbeLighting g = screenProbeGather(probes, pixel, worldPos, nv, linearZ, foliage && (experiment & 2) == 0, wantRadiance, r, halfAngle);
            if ((experiment & 2) == 0)
            {
                irradiance = g.irradiance * g.occlusion;
                irradianceBack = g.irradianceBack * g.occlusion;
            }
            if (wantRadiance) incident = g.radiance;
        }
        if (specular && refl.a > 0) incident = refl.rgb;
    }
    else if (g_viewKind == VIEW_PLANAR_REFLECTION && P[2].w != UNX_NONE)
    {
        GiSrvs gi;
        gi.cache = P[2].w;
        gi.hash = P[2].w;
        gi.pad0 = gi.pad1 = 0;
        irradiance = giCacheIrradiance(gi, worldPos, nv);
        if (foliage) irradianceBack = giCacheIrradiance(gi, worldPos, -nv);
        if (NoV > 0) incident = giCacheRadiance(gi, worldPos, n, r, halfAngle);  // looked up in this surface's normal class
    }
    if (NoV > 0) radiance += front * irradiance + incident * shSpecularAlbedo(P[4].x, f0, NoV, s.roughness);
    radiance += back * irradianceBack;

    // ---- air between the camera and the surface (S's air volume: atmosphere, shadowed air, local lights' air)
    radiance = radiance * airTransmittance + airInscatter;

    RWTexture2D<float4> color = ResourceDescriptorHeap[P[0].w];
    color[pixel] = shEncodeOutput(radiance);
    ShadedPixel o;
    o.radiance = radiance;
    o.linearZ = linearZ;
    o.normal = n;
    return o;
}
