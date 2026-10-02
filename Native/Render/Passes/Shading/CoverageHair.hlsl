// unx-kernel: cs_6_6 main
// unx-variants: MODE=0,1,2,3,4,5
// Strand hair records of the coverage layer (COV_SPECIAL_HAIR, CoverageSpecial.hlsli): their radiance, before the
// composite reads it. Owner: M. A record is a strand segment's piece in one pixel (V's HairRaster.ms); its point is the
// pixel's ray at the record's depth, its tangent the segment's, its material the body's (Hair class: eta = ior, beta_M =
// roughness, sigma_a, beta_N, cuticle tilt). The strand is shaded by Passes/Hair/HairScattering.hlsli: the fibre model
// averaged across its width, with the hair between the point and each light from E's density volume
// (Passes/Hair/HairDensity.hlsli; a body without a block: no fibre in front, P[5].y fibres behind).
//   sun        E x S's sun visibility of the record (the opaque scene's shadow, as the composite's fragments take it:
//              covFragmentShadow) x hairStrandLight (the hair's own shadow and scattering). The disk is taken as its
//              centre: the lobes are wider. Not under water's refraction (waterSunLight).
//   local      shading.mega_lights (the default): a MegaLights instance of its own on the pixel's nearest hair record
//              (as the coverage layer's instance on its nearest cluster fragment, and as Unreal runs MegaLights on the
//              hair samples) - MODE 0..3 make its surface, MODE 4 shades its light samples (each visible sample's light
//              through hairStrandLight, weighed by the sample; area lights as their illuminance on the plane facing
//              their centre, from that direction), the temporal filter follows (the spatial one with
//              shading.hair_lights_spatial: off, as the reference's hair input).
//              The same samples are shaded twice, at the pixel's nearest and at its farthest hair record (each with its
//              own strand and its own hair in front of the lights; the samples' visibility is the nearest's), and a record
//              takes the two results by its view depth between them - as S gives the fragments their shadows from the
//              pixel's two ends. Without mega_lights: the froxel list's lights per record with S's fragment slots
//              (covLocalVisibility's rule).
//   indirect   the Lumen translucency volume's SH at the point. The light from four directions (a tetrahedron's: they
//              carry a 2-band function exactly) is weighed by what the hair lets through towards each (hairThrough with
//              the density volume's count) and put back on 2 bands, L(w) = c + g . w per colour; the strand answers it
//              with its kernel's integral and first moment (hairStrandMoments): c albedo + (g . t) along + (g . e)
//              across. The neighbourhood's backward lobe counts with 1 - exp(-mean count of the four directions).
//              Not seen: opaque geometry nearer than the volume's cells (the head under the hair).
// Then the air in front of the record and the exposure, as covShadeFragment.
//   MODE 0  per pixel: nearest hair depth = 0, farthest = the largest word, their elements = none
//   MODE 1  per special entry: the pixel's nearest and farthest hair record in front of band A (atomic max and min of
//           the depth bits)
//   MODE 2  per special entry: a record at one of those depths stores its element there
//   MODE 3  per pixel: the MegaLights instance's inputs - G-buffer (the fibre's normal towards the viewer, base colour 1:
//           the modulation factor of its result is 1), material word, device depth; no hair: sky
//   MODE 4  per pixel: the light samples' lights on the pixel's nearest hair record (the diffuse output) and on its
//           farthest (the specular output), exposed (m.ml.shade's two outputs: both go through the instance's filter)
//   MODE 5  per special entry: the record's radiance (a record behind the band A surface, which the composite never
//           reaches, keeps 0)
// P[0] = { records (StructuredBuffer<uint4>), special list (raw), tile list (raw), MODE 5: record radiance UAV (raw) }
// P[1] = { hair segments (StructuredBuffer<float4>), hair bodies (raw), density parameters (raw; UNX_NONE: none),
//          MODE 1: band A depth }
// P[2] = { nearest depth (R32_UINT; MODE 0..2 UAV), element (R32_UINT; MODE 0, 2 UAV, MODE 3, 4 SRV),
//          MODE 3: G-buffer UAV, MODE 4: light samples; MODE 3: material word UAV, MODE 4: downsampled key }
// P[3] = { MODE 3: depth UAV; MODE 4: diffuse UAV, specular UAV, weight caps (f16 | f16 << 16), factor | N << 8 }
// P[4] = { MODE 5: S's fragment visibility, V's coverageDepthRange, S's per-record sun bytes, froxel lights (the loop
//          without mega_lights; UNX_NONE each: none) }
// P[5] = { march steps, fibres behind a strand whose body has no block (float), experiment mask, light function table }
// P[6] = { MODE 5: atmosphere transmittance, multi-scatter, air volume, the indirect light's source word (GiSource.hlsli) }
// P[7] = { MODE 5: the hair instance's lighting at the nearest record (RGBA16F, exposed; UNX_NONE: the froxel loop),
//          band A depth, 0, 0 }
// P[8] = { farthest depth (R32_UINT; MODE 0..2 UAV, MODE 5 SRV), its element (MODE 0, 2 UAV, MODE 4 SRV), MODE 5: nearest
//          depth SRV, MODE 5: the instance's lighting at the farthest record }
#include "Bindless.hlsli"
#include "Frame.hlsli"
#include "GBuffer.hlsli"
#include "Passes/Material/MaterialInternal.hlsli"
#include "Passes/Material/MaterialSurface.hlsli"
#include "Passes/Shading/CoverageSpecial.hlsli"
#include "Passes/Visibility/CoverageLayer.hlsli"
#include "Passes/Hair/HairScattering.hlsli"
#include "Passes/Hair/HairDensity.hlsli"
#if MODE >= 4
#include "Passes/Shading/ShadingCommon.hlsli"
#include "Passes/Shading/AreaLight.hlsli"
#include "Passes/Lights/LightFunction.hlsli"
#endif
#if MODE == 4
#include "Passes/Shading/MegaLightsUpsample.hlsli"
#endif
#if MODE == 5
#include "Passes/Atmosphere/Atmosphere.hlsli"
#include "Passes/Atmosphere/Froxel.hlsli"
#include "Passes/Shadow/ShadowVisibility.hlsli"
#include "Passes/GI/GiSource.hlsli"
#endif

#define HAIR_BODIES_MAX 4096u  // (the body search's bound, as HairRaster.ms)

// A hair record's point: camera-relative position, direction to the viewer, the fibre's frame (x = tangent) and the
// outgoing direction in it, its body and fibre parameters.
struct HairPoint
{
    bool valid;
    float3 offset, v, tangent, side, up, outgoing;
    float linearZ;
    uint body, material;
    float eta, betaM, betaN, tilt;
    float3 absorption;
};
HairPoint hairPoint(CoverageFragment f, uint2 pixel)
{
    HairPoint p = (HairPoint)0;
    StructuredBuffer<float4> segments = ResourceDescriptorHeap[P[1].x];
    ByteAddressBuffer bodies = ResourceDescriptorHeap[P[1].y];
    const uint s = coverageFragmentHairSegment(f);
    const uint bodyCount = min(bodies.Load(0), HAIR_BODIES_MAX);
    [loop] for (uint k = 0; k < bodyCount && !p.valid; ++k)
    {
        const uint4 h = bodies.Load4(4 + 4 * 8 * k);  // first segment, segments, segments per strand, material
        if (s >= h.x && s - h.x < h.y)
        {
            p.valid = true;
            p.body = k;
            p.material = h.w;
        }
    }
    if (!p.valid) return p;
    const float3 a = segments[2 * s].xyz, b = segments[2 * s + 1].xyz;
    const float len = length(b - a);
    p.tangent = len > 0 ? (b - a) / len : float3(0, 1, 0);
    p.side = normalize(abs(p.tangent.y) < 0.9 ? cross(p.tangent, float3(0, 1, 0)) : cross(p.tangent, float3(1, 0, 0)));
    p.up = cross(p.tangent, p.side);
    float3 D, Dx, Dy;
    mPixelRay(float2(pixel) + 0.5, D, Dx, Dy);
    p.linearZ = linearDepth(coverageFragmentDepth(f));
    p.offset = D * p.linearZ;
    p.v = -normalize(D);
    p.outgoing = float3(dot(p.v, p.tangent), dot(p.v, p.side), dot(p.v, p.up));
    const GpuMaterial m = loadMaterial(p.material);
    if (materialClass(m) == MATERIAL_HAIR)
    {
        p.eta = max(m.ior, 1.05);
        p.betaM = clamp(m.roughness, 0.05, 1.0);
        p.betaN = clamp(m.hairBetaN, 0.05, 1.0);
        p.tilt = m.hairTilt;
        p.absorption = max(m.hairAbsorption, 0.0);
    }
    else
    {
        // a body whose material is of another class (its class slots hold that class's values): keratin's fibre with
        // the absorption of its base colour (scene::model::hairAbsorption's colour rule at beta_N = 0.3)
        p.eta = 1.55;
        p.betaM = p.betaN = 0.3;
        p.tilt = 0.0349;
        const float3 l = log(clamp(m.baseColor, 1e-4, 1.0)) / 5.8884;
        p.absorption = l * l;
    }
    return p;
}
// The fibre's normal on the viewer's side (the part of v across the fibre).
float3 hairNormal(HairPoint p)
{
    const float3 n = p.v - p.tangent * dot(p.v, p.tangent);
    const float l = length(n);
    return l > 1e-4 ? n / l : p.side;
}
float3 hairLocal(HairPoint p, float3 w) { return float3(dot(w, p.tangent), dot(w, p.side), dot(w, p.up)); }
// Fibres from the point towards d and past it (x, y); a body without a block: none in front, P[5].y behind.
float2 hairCounts(HairPoint p, float3 d, uint steps)
{
    const float front = hairFibreCount(P[1].z, p.body, p.offset, d, steps);
    if (front < 0) return float2(0, asfloat(P[5].y));
    return float2(front, max(hairFibreCount(P[1].z, p.body, p.offset, -d, steps), 0.0));
}
// The strand's radiance per unit of illuminance E on the plane facing l (unit, world).
float3 hairLit(HairPoint p, HairStrand strand, float3 l)
{
    const float3 wi = hairLocal(p, l);
    const float2 n = hairCounts(p, l, P[5].x);
    return hairStrandLight(strand, hairAverage(wi.x, p.eta, p.absorption, p.betaM, p.betaN, p.tilt), wi, n.x, n.y);
}

#if MODE >= 4
// A local light's radiance from the strand, before the opaque scene's visibility (its light function included).
float3 hairLocalLight(HairPoint p, HairStrand strand, GpuLight light, uint lightIndex)
{
    const float3 toLight = (light.position - g_cameraPosition) - p.offset;
    float3 l, E;
    if (lightType(light) > LIGHT_SPOT)
    {
        // an area light: its illuminance on the plane facing its centre (the exact diffuse integral), from that direction
        const float window = shAreaWindow(light, toLight);
        if (window <= 0) return 0;
        l = normalize(toLight);
        const float3 t = normalize(abs(l.y) < 0.9 ? cross(l, float3(0, 1, 0)) : cross(l, float3(1, 0, 0)));
        E = light.color * (light.intensity * window * SH_PI * shAreaIntegral(light, toLight, float3x3(t, cross(l, t), l), true));
    }
    else
    {
        E = shPunctualIlluminance(light, toLight, l);
        if (P[5].w != UNX_NONE)
            E *= lightFunction(P[5].w, lightIndex, light.forward, light.right, -l, p.linearZ * (2 * g_tanHalfFovY / g_viewHeight) / max(length(toLight), 1e-4), g_time);
    }
    if (all(E == 0)) return 0;
    return E * hairLit(p, strand, l);
}
#endif

#if MODE == 0
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= uint2(g_viewWidth, g_viewHeight))) return;
    RWTexture2D<uint> nearest = ResourceDescriptorHeap[P[2].x];
    RWTexture2D<uint> element = ResourceDescriptorHeap[P[2].y];
    RWTexture2D<uint> farthest = ResourceDescriptorHeap[P[8].x];
    RWTexture2D<uint> farElement = ResourceDescriptorHeap[P[8].y];
    nearest[id.xy] = 0;
    element[id.xy] = 0xFFFFFFFFu;
    farthest[id.xy] = 0xFFFFFFFFu;
    farElement[id.xy] = 0xFFFFFFFFu;
}
#elif MODE == 1 || MODE == 2
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    ByteAddressBuffer special = ResourceDescriptorHeap[P[0].y];
    if (id.x >= special.Load(0)) return;
    const uint2 entry = special.Load2(4 * (COV_SPECIAL_HEADER + 2 * id.x));
    if (entry.y != COV_SPECIAL_HAIR) return;
    StructuredBuffer<uint4> records = ResourceDescriptorHeap[P[0].x];
    ByteAddressBuffer list = ResourceDescriptorHeap[P[0].z];
    const CoverageFragment f = coverageUnpackRecord(records[entry.x]);
    const uint2 pixel = covRecordPixel(list, entry.x, f);
    if (any(pixel >= uint2(g_viewWidth, g_viewHeight))) return;
    RWTexture2D<uint> nearest = ResourceDescriptorHeap[P[2].x];
    RWTexture2D<uint> farthest = ResourceDescriptorHeap[P[8].x];
    const uint depthBits = f.depthBits & ~COV_DEPTH_SEE_THROUGH;
#if MODE == 1
    Texture2D<float> bandDepth = ResourceDescriptorHeap[P[1].w];
    if (depthBits < asuint(bandDepth[pixel])) return;  // behind the band A surface: the composite never reaches it
    InterlockedMax(nearest[pixel], depthBits);
    InterlockedMin(farthest[pixel], depthBits);
#else
    RWTexture2D<uint> element = ResourceDescriptorHeap[P[2].y];
    RWTexture2D<uint> farElement = ResourceDescriptorHeap[P[8].y];
    if (nearest[pixel] == depthBits) element[pixel] = entry.x;
    if (farthest[pixel] == depthBits) farElement[pixel] = entry.x;
#endif
}
#elif MODE == 3
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint2 pixel = id.xy;
    if (any(pixel >= uint2(g_viewWidth, g_viewHeight))) return;
    Texture2D<uint> element = ResourceDescriptorHeap[P[2].y];
    RWTexture2D<uint2> gbuffer = ResourceDescriptorHeap[P[2].z];
    RWTexture2D<uint> words = ResourceDescriptorHeap[P[2].w];
    RWTexture2D<float> depth = ResourceDescriptorHeap[P[3].x];
    const uint e = element[pixel];
    HairPoint p = (HairPoint)0;
    CoverageFragment f = (CoverageFragment)0;
    if (e != 0xFFFFFFFFu)
    {
        StructuredBuffer<uint4> records = ResourceDescriptorHeap[P[0].x];
        f = coverageUnpackRecord(records[e]);
        p = hairPoint(f, pixel);
    }
    if (!p.valid)
    {
        gbuffer[pixel] = uint2(0, 0);
        words[pixel] = M_MATERIAL_SKY;
        depth[pixel] = 0;
        return;
    }
    GBufferSample g;
    g.normal = hairNormal(p);
    g.baseColor = 1;
    g.roughness = 1;
    gbuffer[pixel] = encodeGBuffer(g);
    words[pixel] = p.material & 0xFFFFu;
    depth[pixel] = coverageFragmentDepth(f);
}
#elif MODE == 4
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint2 pixel = id.xy;
    if (any(pixel >= uint2(g_viewWidth, g_viewHeight))) return;
    Texture2D<uint> element = ResourceDescriptorHeap[P[2].y];
    RWTexture2D<float4> outDiffuse = ResourceDescriptorHeap[P[3].x];
    RWTexture2D<float4> outSpecular = ResourceDescriptorHeap[P[3].y];
    const uint e = element[pixel];
    HairPoint p = (HairPoint)0;
    if (e != 0xFFFFFFFFu)
    {
        StructuredBuffer<uint4> records = ResourceDescriptorHeap[P[0].x];
        p = hairPoint(coverageUnpackRecord(records[e]), pixel);
    }
    if (!p.valid)
    {
        outDiffuse[pixel] = float4(0, 0, 0, 1);
        outSpecular[pixel] = 0;
        return;
    }
    const MlPixelLights mls = mlPixelLights(P[2].z, P[2].w, pixel, g_cameraPosition + p.offset, hairNormal(p), p.linearZ, P[3].w & 0xFFu, (P[3].w >> 8) & 0xFFu,
                                            f16tof32(P[3].z & 0xFFFFu), f16tof32(P[3].z >> 16));
    float3 sum = 0, sumFar = 0;
    if (mls.count > 0)
    {
        const HairStrand strand = hairStrand(p.outgoing, p.eta, p.absorption, p.betaM, p.betaN, p.tilt);
        [loop] for (uint i = 0; i < mls.count; ++i)
            if (mls.weight[i] > 0) sum += hairLocalLight(p, strand, loadLight(mls.light[i]), mls.light[i]) * mls.weight[i];
        // the pixel's farthest hair record under the same samples
        Texture2D<uint> farElement = ResourceDescriptorHeap[P[8].y];
        const uint ef = farElement[pixel];
        sumFar = sum;
        if (ef != e && ef != 0xFFFFFFFFu)
        {
            StructuredBuffer<uint4> records = ResourceDescriptorHeap[P[0].x];
            const HairPoint q = hairPoint(coverageUnpackRecord(records[ef]), pixel);
            if (q.valid)
            {
                const HairStrand strandFar = hairStrand(q.outgoing, q.eta, q.absorption, q.betaM, q.betaN, q.tilt);
                sumFar = 0;
                [loop] for (uint j = 0; j < mls.count; ++j)
                    if (mls.weight[j] > 0) sumFar += hairLocalLight(q, strandFar, loadLight(mls.light[j]), mls.light[j]) * mls.weight[j];
            }
        }
    }
    outDiffuse[pixel] = float4(min(sum * g_exposure, 60000.0), mls.confidence);
    outSpecular[pixel] = float4(min(sumFar * g_exposure, 60000.0), mls.valid ? 1 : 0);
}
#else
float hairByte(uint word, uint b) { return ((word >> (8u * b)) & 255u) / 255.0; }

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    ByteAddressBuffer special = ResourceDescriptorHeap[P[0].y];
    if (id.x >= special.Load(0)) return;
    const uint2 entry = special.Load2(4 * (COV_SPECIAL_HEADER + 2 * id.x));
    if (entry.y != COV_SPECIAL_HAIR) return;
    StructuredBuffer<uint4> records = ResourceDescriptorHeap[P[0].x];
    ByteAddressBuffer list = ResourceDescriptorHeap[P[0].z];
    RWByteAddressBuffer output = ResourceDescriptorHeap[P[0].w];
    const CoverageFragment f = coverageUnpackRecord(records[entry.x]);
    const uint2 pixel = covRecordPixel(list, entry.x, f);
    bool reached = all(pixel < uint2(g_viewWidth, g_viewHeight));
    if (reached)
    {
        Texture2D<float> bandDepth = ResourceDescriptorHeap[P[7].y];
        reached = (f.depthBits & ~COV_DEPTH_SEE_THROUGH) >= asuint(bandDepth[pixel]);
    }
    HairPoint p = (HairPoint)0;
    if (reached) p = hairPoint(f, pixel);
    if (!p.valid)
    {
        output.Store2(entry.x * 8, uint2(0, 0));
        return;
    }
    const uint experiment = P[5].z;
    const HairStrand strand = hairStrand(p.outgoing, p.eta, p.absorption, p.betaM, p.betaN, p.tilt);
    const float3 worldPos = g_cameraPosition + p.offset;
    float3 radiance = 0;

    // ---- S's shadowing of the record (INTERFACES 7.3 v1.41; CoverageShade.hlsli covFragmentShadow): the sun from the
    // 4-point profile between the pixel's nearest and farthest record, or the record's own byte where S flagged the pixel
    float sunVisibility = 1, depthT = 0;
    uint nearSlots = 0xFFFFFFFFu, farSlots = 0xFFFFFFFFu;
    float zNear = p.linearZ, zFar = p.linearZ;
    const bool shadows = P[4].x != UNX_NONE && P[4].y != UNX_NONE;
    if (shadows)
    {
        StructuredBuffer<uint3> visibility = ResourceDescriptorHeap[P[4].x];
        Texture2D<uint2> range = ResourceDescriptorHeap[P[4].y];
        const uint3 w = visibility[pixel.y * g_viewWidth + pixel.x];
        const uint2 r = range[pixel];
        zNear = linearDepth(asfloat(r.x));
        zFar = linearDepth(asfloat(r.y));
        depthT = zFar > zNear ? saturate((p.linearZ - zNear) / (zFar - zNear)) : 0;
        if ((w.y & 1u) != 0 && P[4].z != UNX_NONE)
        {
            ByteAddressBuffer sunBytes = ResourceDescriptorHeap[P[4].z];
            sunVisibility = hairByte(sunBytes.Load((entry.x >> 2) * 4u), entry.x & 3u);
        }
        else
        {
            const float x = depthT * 3;
            const uint k = min((uint)x, 2u);
            sunVisibility = lerp(hairByte(w.x, k), hairByte(w.x, k + 1u), x - k);
        }
        nearSlots = w.y;
        farSlots = w.z;
    }

    // ---- sun (S's air at the record's depth: in-scatter and transmittance in front of it, the sun's illuminance)
    AtmosphereSrvs atm;
    atm.transmittance = P[6].x;
    atm.multiScatter = P[6].y;
    atm.skyView = UNX_NONE;
    atm.aerial = P[6].z;
    float3 E = g_sunIlluminance * g_sunColor, airInscatter = 0, airTransmittance = 1;
    if (atm.transmittance != UNX_NONE)
    {
        if (atm.aerial != UNX_NONE && (experiment & 8) == 0)
            atmosphereAirView(atm, (float2(pixel) + 0.5) / float2(g_viewWidth, g_viewHeight), p.linearZ, airInscatter, airTransmittance, E);
        else E = atmosphereSunIlluminance(atm, worldPos);
    }
    if (sunVisibility > 0 && (experiment & 16) == 0) radiance += E * sunVisibility * hairLit(p, strand, normalize(g_sunDirection));

    // ---- local lights
    if ((experiment & 32) == 0)
    {
        if (P[7].x != UNX_NONE)
        {
            // the instance's results at the pixel's nearest and farthest hair record, by the record's depth between them
            Texture2D<float4> lighting = ResourceDescriptorHeap[P[7].x];
            Texture2D<float4> lightingFar = ResourceDescriptorHeap[P[8].w];
            Texture2D<uint> nearest = ResourceDescriptorHeap[P[8].z];
            Texture2D<uint> farthest = ResourceDescriptorHeap[P[8].x];
            const float hairNear = linearDepth(asfloat(nearest[pixel])), hairFar = linearDepth(asfloat(farthest[pixel]));
            const float between = hairFar > hairNear ? saturate((p.linearZ - hairNear) / (hairFar - hairNear)) : 0;
            radiance += lerp(lighting[pixel].rgb, lightingFar[pixel].rgb, between) / g_exposure;
        }
        else if (P[4].w != UNX_NONE)
        {
            FroxelSrvs froxels;
            froxels.lights = P[4].w;
            froxels.lightIndices = P[4].w;
            froxels.scattering = UNX_NONE;
            froxels.pad = 0;
            const uint2 range = froxelLightRange(froxels, pixel, p.linearZ);
            const uint indexBase = froxelIndexBase(froxels);
            uint3 nearCasters = 0xFFFFFFFFu, farCasters = 0xFFFFFFFFu;
            if (shadows)
            {
                nearCasters = shadowFirstCasters(froxels, pixel, zNear);
                farCasters = shadowFirstCasters(froxels, pixel, zFar);
            }
            uint4 lightWords = 0;
            [loop] for (uint i = 0; i < range.y; ++i)
            {
                const uint lightIndex = froxelLightBuffered(froxels, indexBase, range, i, lightWords);
                float visibility = 1;
                if (shadows)
                {
                    // the light's slot in the froxel list at each end of the pixel's records, between them by depth
                    const uint a = shadowSlotAmong(nearCasters, lightIndex), b = shadowSlotAmong(farCasters, lightIndex);
                    visibility = lerp(a >= 1 && a <= 3 ? hairByte(nearSlots, a) : 1, b >= 1 && b <= 3 ? hairByte(farSlots, b) : 1, depthT);
                }
                if (visibility > 0) radiance += hairLocalLight(p, strand, loadLight(lightIndex), lightIndex) * visibility;
            }
        }
    }

    // ---- indirect: the translucency volume's SH through the hair, on the strand's moments
    if (giSourceIsVolume(P[6].w) && (experiment & 6) != 6)
    {
        const LtvSh sh = ltvSample(ltvParams(giSourceVolume(P[6].w)), worldPos);
        const HairAverage average = hairAverage(p.outgoing.x, p.eta, p.absorption, p.betaM, p.betaN, p.tilt);
        static const float3 kDirections[4] = { float3(0.57735027, 0.57735027, 0.57735027), float3(0.57735027, -0.57735027, -0.57735027),
                                               float3(-0.57735027, 0.57735027, -0.57735027), float3(-0.57735027, -0.57735027, 0.57735027) };
        float3 mean = 0, gx = 0, gy = 0, gz = 0;
        float around = 0;
        const uint steps = max(P[5].x / 2, 1u);
        [loop] for (uint k = 0; k < 4; ++k)
        {
            const float3 d = kDirections[k];
            float count = hairFibreCount(P[1].z, p.body, p.offset, d, steps);
            float behind = count;
            if (count < 0)
            {
                count = 0;
                behind = asfloat(P[5].y);
            }
            const HairThrough through = hairThrough(average, count);
            const float3 L = ltvRadianceOf(sh, d) * (through.direct + through.scattered);
            mean += 0.25 * L;
            gx += 0.75 * L * d.x;
            gy += 0.75 * L * d.y;
            gz += 0.75 * L * d.z;
            around += 0.25 * behind;
        }
        const HairMoments moments = hairStrandMoments(strand, average, 1 - exp(-around));
        const float3 e = hairNormal(p);
        const float3 alongT = gx * p.tangent.x + gy * p.tangent.y + gz * p.tangent.z, acrossE = gx * e.x + gy * e.y + gz * e.z;
        radiance += max(mean * moments.albedo + alongT * moments.along + acrossE * moments.across, 0.0);
    }
    output.Store2(entry.x * 8, covPackRadiance((radiance * airTransmittance + airInscatter) * g_exposure));
}
#endif
