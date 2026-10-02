// unx-kernel: cs_6_6 main
// unx-variants: MODE=0,1,2,3,4,5,6,7,8,9,10,11
// Strand hair records of the coverage layer (COV_SPECIAL_HAIR, CoverageSpecial.hlsli): their radiance, before the
// composite reads it. Owner: M. A record is a strand segment's piece in one pixel (V's HairRaster.ms); its point is the
// pixel's ray at the record's depth, its tangent the segment's, its material the body's (Hair class: eta = ior, beta_M =
// roughness, sigma_a, beta_N, cuticle tilt). The strand is shaded by Passes/Hair/HairScattering.hlsli: the fibre model
// averaged across its width, with the hair between the point and each light from E's density volume
// (Passes/Hair/HairDensity.hlsli; a body without a block: no fibre in front, P[5].y fibres behind). The hair in front is
// the strand's own body's as far as the light and, with shading.hair_shadows, the other bodies' on that path
// (hairFibreCountOthers: one groom shadows another as it shadows what is not hair).
//   sun        E x S's sun visibility of the record (the opaque scene's shadow, as the composite's fragments take it:
//              covFragmentShadow) x hairStrandLight (the hair's own shadow and scattering). The disk is taken as its
//              centre: the lobes are wider. Not under water's refraction (waterSunLight).
//              The records of a segment share the strand's part (shading.hair_segment_shading): MODE 7..10 list the
//              segments that have a record, MODE 11 evaluates hairStrandLight for the sun and the indirect light below at
//              HAIR_SEGMENT_POINTS points along each, and a record takes the two points around its place on the segment
//              (hairBetween) - the strand's light changes along a segment with the hair in front of it, which the
//              density volume resolves no finer than its cells, and not across the strand's width. E, S's visibility,
//              the local lights and the air stay the record's own.
//   local      shading.mega_lights (the default): a MegaLights instance of its own on the pixel's nearest hair record
//              (as the coverage layer's instance on its nearest cluster fragment, and as Unreal runs MegaLights on the
//              hair samples) - MODE 0..3 make its surface, MODE 4 shades its light samples (each visible sample's light
//              through hairStrandLight, weighed by the sample; area lights as their illuminance on the plane facing
//              their centre, from that direction), the temporal filter follows (the spatial one with
//              shading.hair_lights_spatial: off, as the reference's hair input).
//              The same samples are shaded twice, at the pixel's nearest and at its farthest hair record (each with its
//              own strand and its own hair in front of the lights), and a record takes the two results by its view
//              depth between them - as S gives the fragments their shadows from the pixel's two ends. The samples'
//              shadow rays (the opaque scene between the hair and the light) start where the pixel's ray first meets
//              the groom (shading.hair_lights_ray_depths, MODE 6): the strands drawn stand for the body's hair -
//              far away a few wide ones - and the first of them in a pixel lies at a depth drawn from hairFirstFibre's
//              distribution, so a shadow's edge would fall at another place on each strand. The ray's origin is drawn
//              from that distribution anew every frame (a quantile that steps by the golden ratio) and the instance's
//              temporal filter keeps the mean: the opaque scene's shadow on the hair the pixel shows, the same for
//              every strand. Without it the rays start at the nearest record.
//              Without mega_lights: the froxel list's lights per record with S's fragment slots (covLocalVisibility's
//              rule).
//   indirect   the Lumen translucency volume's SH at the point. The light from four directions (a tetrahedron's: they
//              carry a 2-band function exactly) is weighed by what the hair lets through towards each (hairThrough with
//              the density volume's count) and put back on 2 bands, L(w) = c + g . w per colour; the strand answers it
//              with its kernel's integral and first moment (hairStrandMoments): c albedo + (g . t) along + (g . e)
//              across. The neighbourhood's backward lobe counts with 1 - exp(-mean count of the four directions).
//              Not seen: opaque geometry nearer than the volume's cells (the head under the hair).
// Then the air in front of the record and the exposure, as covShadeFragment.
// Every march through the density volume takes its samples at a phase drawn for the evaluation (the pixel, the record,
// the segment's point) and the frame (hairJitter): the volume's cells leave no fixed pattern along a shadow's edge on
// the hair - the temporal filters (the instance's, the upscaler's) take the mean.
// The light per-record modes (1, 2, 8) walk V's records by block (one group of 256 threads per block of the tile list,
// 4 records a thread; the group finds its tile once, a record's pixel is its place in the tile) and take the hair
// records; MODE 5 goes by a list (a thread per entry, groups of 64: the record kernel is slower in larger groups - 1.2
// times in groups of 256 with the segments' light, eight times in groups of 1,024 [measured, hair_ball 1080p]).
// The composite never reaches a record behind the band A surface: MODE 8 lists the records in front of it and their
// segments and sets the others' radiance to 0; MODE 5 shades the listed ones (a record left out among listed ones would
// keep its lane's place in the wave: the list has none). [measured, hair_ball 1080p: leaving out also the records behind
// a hair record whose own mask is full - the composite stops there - saved 0.06 ms of MODE 5 and cost 0.10 ms for the
// pass that finds those records: the strands are about a pixel wide, few records cover their pixel alone]
//   MODE 0  per pixel: nearest hair depth = 0, farthest = the largest word, their elements = none
//   MODE 1  per record: the pixel's nearest and farthest hair record in front of band A (atomic max and min of the depth
//           bits)
//   MODE 2  per record: a record at one of those depths stores its element there
//   MODE 3  per pixel: the MegaLights instance's inputs - G-buffer (the fibre's normal towards the viewer, base colour 1:
//           the modulation factor of its result is 1), material word, device depth; no hair: sky
//   MODE 4  per pixel: the light samples' lights on the pixel's nearest hair record (the diffuse output) and on its
//           farthest (the specular output), exposed (m.ml.shade's two outputs: both go through the instance's filter)
//   MODE 5  per listed record (MODE 8's list; without it per special entry): the record's radiance (a record behind
//           the band A surface, which the composite never reaches, keeps 0). A listed record's pixel comes from its
//           entry's tile; a special entry's from a search of the tile list (covRecordPixel: about 14 loads a record)
//   MODE 6  per downsampled pixel of the instance: the key its shadow rays start from (the sample key with the view depth
//           of the first fibre on the pixel's ray; the key itself where the volume holds no hair on the ray)
//   MODE 7  the visible-segment bits set to 0 (256 words a group), the two lists' headers
//   MODE 8  per record: a hair record in front of band A joins the record list (one atomic per wave: its element and
//           its tile's coordinate, which the block walk has once per group) and sets its segment's bit; another hair
//           record's radiance is set to 0
//   MODE 9  per word of the bits (64 a group): its segments appended to the segment list (one atomic per word)
//   MODE 10 one thread: MODE 11's and MODE 5's dispatch arguments (64 entries a group; x up to 65535, then y)
//   MODE 11 per listed segment and point: the strand's kernel under the sun and its indirect light
// P[0] = { records (StructuredBuffer<uint4>), MODE 5: special list (raw), tile list (raw), MODE 5: record radiance UAV (raw) }
// P[1] = { hair segments (StructuredBuffer<float4>), hair bodies (raw), density parameters (raw; UNX_NONE: none),
//          MODE 1: band A depth }
// P[2] = { nearest depth (R32_UINT; MODE 0..2 UAV), element (R32_UINT; MODE 0, 2 UAV, MODE 3, 4, 6 SRV),
//          MODE 3: G-buffer UAV, MODE 4: light samples; MODE 3: material word UAV, MODE 4, 6: downsampled key }
// P[3] = { MODE 3: depth UAV; MODE 4: diffuse UAV, specular UAV, weight caps (f16 | f16 << 16), factor | N << 8;
//          MODE 6: the rays' key UAV (R32G32_UINT), 0, 0, factor | N << 8 }
// P[4] = { MODE 5: S's fragment visibility, V's coverageDepthRange, S's per-record sun bytes, froxel lights (the loop
//          without mega_lights; UNX_NONE each: none) }
// P[5] = { march steps | shading.hair_shadows << 30 | shading.hair_march_jitter << 31, fibres behind a strand whose
//          body has no block (float), experiment mask, light function table }
// P[6] = { MODE 5: atmosphere transmittance, multi-scatter, air volume; MODE 5, 11: the indirect light's source word
//          (GiSource.hlsli) }
// P[7] = { MODE 5: the hair instance's lighting at the nearest record (RGBA16F, exposed; UNX_NONE: the froxel loop),
//          MODE 5, 6, 8: band A depth, 0, 0 }
// P[8] = { farthest depth (R32_UINT; MODE 0..2 UAV, MODE 5 SRV), its element (MODE 0, 2 UAV, MODE 4 SRV), MODE 5: nearest
//          depth SRV, MODE 5: the instance's lighting at the farthest record }
// P[9] = { MODE 7..9: the visible-segment bits UAV (raw, a bit per segment of the hair segments), MODE 7, 9, 10: the
//          segment list UAV (raw: count, MODE 11's arguments, then the segments), MODE 11: SRV, MODE 5: the record list
//          SRV (UNX_NONE: the special list); the segments' light (raw, HAIR_SEGMENT_POINTS x 16 B per segment: f16 rgb
//          of the sun's kernel, f16 rgb of the indirect radiance x exposure; MODE 11 UAV, MODE 5 SRV - UNX_NONE: every
//          record shades its own point), the frame's segments }
// P[10] = { MODE 7, 8, 10: the record list UAV (raw: count, MODE 5's arguments, then per record 2 words: its element,
//           its tile x | y << 16), its capacity (records), MODE 8: record radiance UAV, 0 }
#include "Bindless.hlsli"
#include "Frame.hlsli"
#include "GBuffer.hlsli"
#include "Passes/Material/MaterialInternal.hlsli"
#include "Passes/Material/MaterialSurface.hlsli"
#include "Passes/Shading/CoverageSpecial.hlsli"
#include "Passes/Visibility/CoverageLayer.hlsli"
#include "Passes/Hair/HairScattering.hlsli"
#include "Passes/Hair/HairDensity.hlsli"
#if MODE == 4 || MODE == 5 || MODE == 11
#include "Passes/Shading/ShadingCommon.hlsli"
#include "Passes/Shading/AreaLight.hlsli"
#include "Passes/Lights/LightFunction.hlsli"
#endif
#if MODE == 4
#include "Passes/Shading/MegaLightsUpsample.hlsli"
#endif
#if MODE == 6
#include "Passes/Shading/MegaLights.hlsli"
#endif
#if MODE == 5
#include "Passes/Atmosphere/Atmosphere.hlsli"
#include "Passes/Atmosphere/Froxel.hlsli"
#include "Passes/Shadow/ShadowVisibility.hlsli"
#endif
#if MODE == 5 || MODE == 11
#include "Passes/GI/GiSource.hlsli"
#endif

#define HAIR_BODIES_MAX 4096u  // (the body search's bound, as HairRaster.ms)
#define HAIR_SEGMENT_POINTS 3u  // points along a segment at which MODE 11 evaluates the strand (ShadingSystem.cpp kHairSegmentPoints)
#define HAIR_SEGMENT_LIST 4u    // header words of the segment list: count, MODE 11's dispatch arguments
#define HAIR_STEPS (P[5].x & 0xFFFFu)  // shading.hair_density_steps

// A block of V's records (CoverageTiles.hlsli; the walk of MegaLightsCoverage.hlsl): its tile's coordinate, records and
// record base, and the block's first record in the tile. false: no such block (uniform over the group).
struct HairBlock
{
    uint2 tile;
    uint records, base, first;
};
bool hairBlock(uint3 gid, out HairBlock b)
{
    ByteAddressBuffer list = ResourceDescriptorHeap[P[0].z];
    const uint block = gid.x + gid.y * 65535u;
    b = (HairBlock)0;
    if (block >= list.Load(4 * COV_LIST_BLOCKS)) return false;
    const uint4 info = coverageTileInfo(list, coverageBlockTile(list, block));  // tile, records, record base, block base
    const uint tilesX = list.Load(4 * COV_LIST_TILES_X);
    b.tile = uint2(info.x % tilesX, info.x / tilesX);
    b.records = info.y;
    b.base = info.z;
    b.first = (block - info.w) * COV_BLOCK;
    return true;
}
uint2 hairBlockPixel(HairBlock b, CoverageFragment f)
{
    const uint p = coverageFragmentPixel(f);
    return b.tile * COV_TILE_PX + uint2(p % COV_TILE_PX, p / COV_TILE_PX);
}

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
// The body holding segment s (a bounded search of the bodies' header) and its material: false when none does.
bool hairBodyOf(uint s, out uint body, out uint material)
{
    ByteAddressBuffer bodies = ResourceDescriptorHeap[P[1].y];
    const uint bodyCount = min(bodies.Load(0), HAIR_BODIES_MAX);
    bool found = false;
    body = material = 0;
    [loop] for (uint k = 0; k < bodyCount && !found; ++k)
    {
        const uint4 h = bodies.Load4(4 + 4 * 8 * k);  // first segment, segments, segments per strand, material
        if (s >= h.x && s - h.x < h.y)
        {
            found = true;
            body = k;
            material = h.w;
        }
    }
    return found;
}
// The point of segment s at 'offset' (camera-relative) seen along v (unit, to the viewer) at view depth linearZ.
HairPoint hairPointAt(uint s, float3 offset, float3 v, float linearZ)
{
    HairPoint p = (HairPoint)0;
    p.valid = hairBodyOf(s, p.body, p.material);
    if (!p.valid) return p;
    StructuredBuffer<float4> segments = ResourceDescriptorHeap[P[1].x];
    const float3 a = segments[2 * s].xyz, b = segments[2 * s + 1].xyz;
    const float len = length(b - a);
    p.tangent = len > 0 ? (b - a) / len : float3(0, 1, 0);
    p.side = normalize(abs(p.tangent.y) < 0.9 ? cross(p.tangent, float3(0, 1, 0)) : cross(p.tangent, float3(1, 0, 0)));
    p.up = cross(p.tangent, p.side);
    p.linearZ = linearZ;
    p.offset = offset;
    p.v = v;
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
// A record's point: the pixel's ray at the record's depth.
HairPoint hairPoint(CoverageFragment f, uint2 pixel)
{
    float3 D, Dx, Dy;
    mPixelRay(float2(pixel) + 0.5, D, Dx, Dy);
    const float linearZ = linearDepth(coverageFragmentDepth(f));
    return hairPointAt(coverageFragmentHairSegment(f), D * linearZ, -normalize(D), linearZ);
}
// The fibre's normal on the viewer's side (the part of v across the fibre).
float3 hairNormal(HairPoint p)
{
    const float3 n = p.v - p.tangent * dot(p.v, p.tangent);
    const float l = length(n);
    return l > 1e-4 ? n / l : p.side;
}
float3 hairLocal(HairPoint p, float3 w) { return float3(dot(w, p.tangent), dot(w, p.side), dot(w, p.up)); }
// Fibres from the point towards d as far as 'reach' (the light's distance) and past the point (x, y); a body without a
// block: none of its own in front, P[5].y behind. With shading.hair_shadows (P[5].x bit 30) the other bodies' fibres on
// the path count in front. jitter: the marches' phase (HairDensity.hlsli), a number of the evaluation's own that changes
// every frame - hairJitter.
float2 hairCounts(HairPoint p, float3 d, float reach, uint steps, float jitter)
{
    const float others = (P[5].x & 0x40000000u) != 0 ? hairFibreCountOthers(P[1].z, p.body, p.offset, d, reach, 2 * steps, jitter) : 0;
    const float front = hairFibreCountWithin(P[1].z, p.body, p.offset, d, reach, steps, jitter);
    if (front < 0) return float2(others, asfloat(P[5].y));
    return float2(front + others, max(hairFibreCount(P[1].z, p.body, p.offset, -d, steps, jitter), 0.0));
}
// A unit number for evaluation 'id' (a pixel, a record, a segment's point) and its k-th march, new every frame.
// (shading.hair_march_jitter off: the midpoint rule)
float hairJitter(uint id, uint k) { return (P[5].x >> 31) != 0 ? hairDensityUnit(id * 0x9E3779B1u + (g_frameIndex & 0xFFFFu) * 0x85EBCA77u, k) : 0.5f; }
// The strand's radiance per unit of illuminance E on the plane facing l (unit, world), the light 'reach' metres away.
float3 hairLit(HairPoint p, HairStrand strand, float3 l, float reach, float jitter)
{
    const float3 wi = hairLocal(p, l);
    const float2 n = hairCounts(p, l, reach, HAIR_STEPS, jitter);
    return hairStrandLight(strand, hairAverage(wi.x, p.eta, p.absorption, p.betaM, p.betaN, p.tilt), wi, n.x, n.y);
}

#if MODE == 4 || MODE == 5
// A local light's radiance from the strand, before the opaque scene's visibility (its light function included).
float3 hairLocalLight(HairPoint p, HairStrand strand, GpuLight light, uint lightIndex, float jitter)
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
    return E * hairLit(p, strand, l, length(toLight), jitter);
}
#endif

#if MODE == 5 || MODE == 11
// The strand's indirect light at its point: the translucency volume's SH through the hair, on the strand's moments (the
// header's 'indirect'). No volume: 0. id: the evaluation's (hairJitter).
float3 hairIndirect(HairPoint p, HairStrand strand, uint id)
{
    if (!giSourceIsVolume(P[6].w)) return 0;
    const LtvSh sh = ltvSample(ltvParams(giSourceVolume(P[6].w)), g_cameraPosition + p.offset);
    const HairAverage average = hairAverage(p.outgoing.x, p.eta, p.absorption, p.betaM, p.betaN, p.tilt);
    static const float3 kDirections[4] = { float3(0.57735027, 0.57735027, 0.57735027), float3(0.57735027, -0.57735027, -0.57735027),
                                           float3(-0.57735027, 0.57735027, -0.57735027), float3(-0.57735027, -0.57735027, 0.57735027) };
    float3 mean = 0, gx = 0, gy = 0, gz = 0;
    float around = 0;
    const uint steps = max(HAIR_STEPS / 2, 1u);
    [loop] for (uint k = 0; k < 4; ++k)
    {
        const float3 d = kDirections[k];
        float count = hairFibreCount(P[1].z, p.body, p.offset, d, steps, hairJitter(id, 8 + k));
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
    return max(mean * moments.albedo + alongT * moments.along + acrossE * moments.across, 0.0);
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
[numthreads(256, 1, 1)]
void main(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
    HairBlock block;
    if (!hairBlock(gid, block)) return;
    StructuredBuffer<uint4> records = ResourceDescriptorHeap[P[0].x];
    RWTexture2D<uint> nearest = ResourceDescriptorHeap[P[2].x];
    RWTexture2D<uint> farthest = ResourceDescriptorHeap[P[8].x];
    [unroll] for (uint q = 0; q < COV_BLOCK / 256; ++q)
    {
        const uint i = block.first + q * 256 + gi;
        if (i >= block.records) continue;
        const CoverageFragment f = coverageUnpackRecord(records[block.base + i]);
        if (!coverageFragmentIsHair(f)) continue;
        const uint2 pixel = hairBlockPixel(block, f);
        if (any(pixel >= uint2(g_viewWidth, g_viewHeight))) continue;
        const uint depthBits = f.depthBits & ~COV_DEPTH_SEE_THROUGH;
#if MODE == 1
        Texture2D<float> bandDepth = ResourceDescriptorHeap[P[1].w];
        if (depthBits < asuint(bandDepth[pixel])) continue;  // behind the band A surface: the composite never reaches it
        InterlockedMax(nearest[pixel], depthBits);
        InterlockedMin(farthest[pixel], depthBits);
#else
        RWTexture2D<uint> element = ResourceDescriptorHeap[P[2].y];
        RWTexture2D<uint> farElement = ResourceDescriptorHeap[P[8].y];
        if (nearest[pixel] == depthBits) element[pixel] = block.base + i;
        if (farthest[pixel] == depthBits) farElement[pixel] = block.base + i;
#endif
    }
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
        const uint marches = pixel.x + pixel.y * 65536u;  // (hairJitter's id)
        [loop] for (uint i = 0; i < mls.count; ++i)
            if (mls.weight[i] > 0) sum += hairLocalLight(p, strand, loadLight(mls.light[i]), mls.light[i], hairJitter(marches, i)) * mls.weight[i];
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
                    if (mls.weight[j] > 0) sumFar += hairLocalLight(q, strandFar, loadLight(mls.light[j]), mls.light[j], hairJitter(marches, 16 + j)) * mls.weight[j];
            }
        }
    }
    outDiffuse[pixel] = float4(min(sum * g_exposure, 60000.0), mls.confidence);
    outSpecular[pixel] = float4(min(sumFar * g_exposure, 60000.0), mls.valid ? 1 : 0);
}
#elif MODE == 5
float hairByte(uint word, uint b) { return ((word >> (8u * b)) & 255u) / 255.0; }
// A segment's light between two of MODE 11's points (t in [0, 1] from a to b): each colour along a geometric line - what
// lies in front of a strand thins its light as exp(-fibres), and the fibres change about linearly along a segment. A
// colour that is 0 at one of the two runs from 1e-4 of the other; 0 at both: 0.
float3 hairBetween(float3 a, float3 b, float t)
{
    const float3 top = max(a, b), low = max(1e-4 * top, 1e-30);
    const float3 mixed = exp(lerp(log(max(a, low)), log(max(b, low)), t));
    return float3(top.x > 0 ? mixed.x : 0, top.y > 0 ? mixed.y : 0, top.z > 0 ? mixed.z : 0);
}

// The radiance of hair record f, element 'element' of V's records, in 'pixel'. listed: MODE 8 found that the composite
// can reach it.
void hairShadeRecord(uint element, CoverageFragment f, uint2 pixel, bool listed)
{
    RWByteAddressBuffer output = ResourceDescriptorHeap[P[0].w];
    bool reached = all(pixel < uint2(g_viewWidth, g_viewHeight));
    if (reached && !listed)
    {
        Texture2D<float> bandDepth = ResourceDescriptorHeap[P[7].y];
        reached = (f.depthBits & ~COV_DEPTH_SEE_THROUGH) >= asuint(bandDepth[pixel]);
    }
    // The record's point: the pixel's ray at its depth. With the segments' light (P[9].z) the strand's part is read from
    // its segment and the strand itself is made only for the froxel loop.
    const uint segment = coverageFragmentHairSegment(f);
    const bool bySegment = P[9].z != UNX_NONE && segment < P[9].w;
    float3 D, Dx, Dy;
    mPixelRay(float2(pixel) + 0.5, D, Dx, Dy);
    const float linearZ = linearDepth(coverageFragmentDepth(f));
    const float3 offset = D * linearZ;
    HairPoint p = (HairPoint)0;
    HairStrand strand = (HairStrand)0;
    if (reached && !bySegment)
    {
        p = hairPoint(f, pixel);
        reached = p.valid;
        if (reached) strand = hairStrand(p.outgoing, p.eta, p.absorption, p.betaM, p.betaN, p.tilt);
    }
    if (!reached)
    {
        output.Store2(element * 8, uint2(0, 0));
        return;
    }
    const uint experiment = P[5].z;
    const float3 worldPos = g_cameraPosition + offset;
    float3 radiance = 0;
    float3 sunKernel = 0, indirect = 0;  // the strand per unit of the sun's illuminance on the plane facing it; its indirect radiance
    if (bySegment)
    {
        StructuredBuffer<float4> segments = ResourceDescriptorHeap[P[1].x];
        ByteAddressBuffer light = ResourceDescriptorHeap[P[9].z];
        const float3 a = segments[2 * segment].xyz, ab = segments[2 * segment + 1].xyz - a;
        const float along = saturate(dot(offset - a, ab) / max(dot(ab, ab), 1e-20)) * (HAIR_SEGMENT_POINTS - 1);
        const uint k = min((uint)along, HAIR_SEGMENT_POINTS - 2);
        const uint4 l0 = light.Load4(16 * (segment * HAIR_SEGMENT_POINTS + k)), l1 = light.Load4(16 * (segment * HAIR_SEGMENT_POINTS + k + 1));
        sunKernel = hairBetween(covUnpackRadiance(l0.xy), covUnpackRadiance(l1.xy), along - k);
        indirect = hairBetween(covUnpackRadiance(l0.zw), covUnpackRadiance(l1.zw), along - k) / g_exposure;
    }

    // ---- S's shadowing of the record (INTERFACES 7.3 v1.41; CoverageShade.hlsli covFragmentShadow): the sun from the
    // 4-point profile between the pixel's nearest and farthest record, or the record's own byte where S flagged the pixel
    float sunVisibility = 1, depthT = 0;
    uint nearSlots = 0xFFFFFFFFu, farSlots = 0xFFFFFFFFu;
    float zNear = linearZ, zFar = linearZ;
    const bool shadows = P[4].x != UNX_NONE && P[4].y != UNX_NONE;
    if (shadows)
    {
        StructuredBuffer<uint3> visibility = ResourceDescriptorHeap[P[4].x];
        Texture2D<uint2> range = ResourceDescriptorHeap[P[4].y];
        const uint3 w = visibility[pixel.y * g_viewWidth + pixel.x];
        const uint2 r = range[pixel];
        zNear = linearDepth(asfloat(r.x));
        zFar = linearDepth(asfloat(r.y));
        depthT = zFar > zNear ? saturate((linearZ - zNear) / (zFar - zNear)) : 0;
        if ((w.y & 1u) != 0 && P[4].z != UNX_NONE)
        {
            ByteAddressBuffer sunBytes = ResourceDescriptorHeap[P[4].z];
            sunVisibility = hairByte(sunBytes.Load((element >> 2) * 4u), element & 3u);
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
            atmosphereAirView(atm, (float2(pixel) + 0.5) / float2(g_viewWidth, g_viewHeight), linearZ, airInscatter, airTransmittance, E);
        else E = atmosphereSunIlluminance(atm, worldPos);
    }
    if (sunVisibility > 0 && (experiment & 16) == 0)
    {
        if (!bySegment) sunKernel = hairLit(p, strand, normalize(g_sunDirection), 3.0e38f, hairJitter(element, 0));
        radiance += E * sunVisibility * sunKernel;
    }

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
            const float between = hairFar > hairNear ? saturate((linearZ - hairNear) / (hairFar - hairNear)) : 0;
            radiance += lerp(lighting[pixel].rgb, lightingFar[pixel].rgb, between) / g_exposure;
        }
        else if (P[4].w != UNX_NONE)
        {
            if (bySegment)
            {
                p = hairPoint(f, pixel);
                strand = hairStrand(p.outgoing, p.eta, p.absorption, p.betaM, p.betaN, p.tilt);
            }
            FroxelSrvs froxels;
            froxels.lights = P[4].w;
            froxels.lightIndices = P[4].w;
            froxels.scattering = UNX_NONE;
            froxels.pad = 0;
            const uint2 range = froxelLightRange(froxels, pixel, linearZ);
            const uint indexBase = froxelIndexBase(froxels);
            uint3 nearCasters = 0xFFFFFFFFu, farCasters = 0xFFFFFFFFu;
            if (shadows)
            {
                nearCasters = shadowFirstCasters(froxels, pixel, zNear);
                farCasters = shadowFirstCasters(froxels, pixel, zFar);
            }
            uint4 lightWords = 0;
            [loop] for (uint i = 0; i < (p.valid ? range.y : 0u); ++i)
            {
                const uint lightIndex = froxelLightBuffered(froxels, indexBase, range, i, lightWords);
                float visibility = 1;
                if (shadows)
                {
                    // the light's slot in the froxel list at each end of the pixel's records, between them by depth
                    const uint a = shadowSlotAmong(nearCasters, lightIndex), b = shadowSlotAmong(farCasters, lightIndex);
                    visibility = lerp(a >= 1 && a <= 3 ? hairByte(nearSlots, a) : 1, b >= 1 && b <= 3 ? hairByte(farSlots, b) : 1, depthT);
                }
                if (visibility > 0) radiance += hairLocalLight(p, strand, loadLight(lightIndex), lightIndex, hairJitter(element, 1 + (i & 3u))) * visibility;
            }
        }
    }

    // ---- indirect
    if (!bySegment && (experiment & 6) != 6) indirect = hairIndirect(p, strand, element);
    radiance += indirect;
    output.Store2(element * 8, covPackRadiance((radiance * airTransmittance + airInscatter) * g_exposure));
}

[numthreads(64, 1, 1)]
void main(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
    const uint index = (gid.x + gid.y * 65535) * 64 + gi;
    const bool listed = P[9].y != UNX_NONE;
    uint element;
    uint2 tile = 0;
    if (listed)
    {
        ByteAddressBuffer recordList = ResourceDescriptorHeap[P[9].y];
        if (index >= recordList.Load(0)) return;
        const uint2 entry = recordList.Load2(4 * (HAIR_SEGMENT_LIST + 2 * index));
        element = entry.x;
        tile = uint2(entry.y & 0xFFFFu, entry.y >> 16);
    }
    else
    {
        ByteAddressBuffer special = ResourceDescriptorHeap[P[0].y];
        if (index >= special.Load(0)) return;
        const uint2 entry = special.Load2(4 * (COV_SPECIAL_HEADER + 2 * index));
        if (entry.y != COV_SPECIAL_HAIR) return;
        element = entry.x;
    }
    StructuredBuffer<uint4> records = ResourceDescriptorHeap[P[0].x];
    ByteAddressBuffer list = ResourceDescriptorHeap[P[0].z];
    const CoverageFragment f = coverageUnpackRecord(records[element]);
    uint2 pixel;
    if (listed)
    {
        const uint inTile = coverageFragmentPixel(f);
        pixel = tile * COV_TILE_PX + uint2(inTile % COV_TILE_PX, inTile / COV_TILE_PX);
    }
    else pixel = covRecordPixel(list, element, f);
    hairShadeRecord(element, f, pixel, listed);
}
#elif MODE == 6
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint2 ds = id.xy;
    const uint factor = P[3].w & 0xFFu;
    if (any(ds >= (uint2(g_viewWidth, g_viewHeight) + factor - 1) / factor)) return;
    Texture2D<uint2> keys = ResourceDescriptorHeap[P[2].w];
    RWTexture2D<uint2> rayKeys = ResourceDescriptorHeap[P[3].x];
    uint2 key = keys[ds];
    if (asfloat(key.x) > 0)
    {
        // the downsampled pixel's surface is its pixel's nearest hair record: that record's body along the pixel's ray,
        // as far as the opaque surface behind
        const uint2 pixel = mlFullPixel(ds, factor, g_frameIndex);
        Texture2D<uint> element = ResourceDescriptorHeap[P[2].y];
        const uint e = element[pixel];
        uint body = 0, material = 0;
        bool found = false;
        if (e != 0xFFFFFFFFu)
        {
            StructuredBuffer<uint4> records = ResourceDescriptorHeap[P[0].x];
            found = hairBodyOf(coverageFragmentHairSegment(coverageUnpackRecord(records[e])), body, material);
        }
        if (found)
        {
            float3 D, Dx, Dy;
            mPixelRay(float2(pixel) + 0.5, D, Dx, Dy);
            const float perDepth = length(D);  // metres along the ray per metre of view depth
            Texture2D<float> bandDepth = ResourceDescriptorHeap[P[7].y];
            // the quantile: the pixel's own number, moved every frame by the golden ratio - the frames a history holds
            // spread evenly over [0, 1), so the mean of a few of them is close to the distribution's
            const float u = frac(mlNoise(ds, 0, 6) + 0.61803399 * (g_frameIndex & 1023u));
            float met;
            const float t = hairFirstFibre(P[1].z, body, float3(0, 0, 0), D / perDepth, min(linearDepth(bandDepth[pixel]), 1.0e6) * perDepth, u, met);
            if (met > 1e-3) key.x = asuint(t / perDepth);
        }
    }
    rayKeys[ds] = key;
}
#elif MODE == 7
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    RWByteAddressBuffer bits = ResourceDescriptorHeap[P[9].x];
    const uint words = (P[9].w + 31) / 32, word = 4 * id.x;
    if (word < words) bits.Store4(4 * word, uint4(0, 0, 0, 0));
    if (id.x == 0)
    {
        RWByteAddressBuffer segmentList = ResourceDescriptorHeap[P[9].y];
        RWByteAddressBuffer recordList = ResourceDescriptorHeap[P[10].x];
        segmentList.Store4(0, uint4(0, 0, 1, 1));
        recordList.Store4(0, uint4(0, 0, 1, 1));
    }
}
#elif MODE == 8
[numthreads(256, 1, 1)]
void main(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
    HairBlock block;
    if (!hairBlock(gid, block)) return;
    StructuredBuffer<uint4> records = ResourceDescriptorHeap[P[0].x];
    RWByteAddressBuffer bits = ResourceDescriptorHeap[P[9].x];
    RWByteAddressBuffer recordList = ResourceDescriptorHeap[P[10].x];
    RWByteAddressBuffer output = ResourceDescriptorHeap[P[10].z];
    Texture2D<float> bandDepth = ResourceDescriptorHeap[P[7].y];
    [unroll] for (uint q = 0; q < COV_BLOCK / 256; ++q)
    {
        const uint i = block.first + q * 256 + gi;
        bool hair = false, visible = false;
        uint s = 0;
        if (i < block.records)
        {
            const CoverageFragment f = coverageUnpackRecord(records[block.base + i]);
            hair = coverageFragmentIsHair(f);
            s = coverageFragmentHairSegment(f);
            const uint2 pixel = hairBlockPixel(block, f);
            if (hair && all(pixel < uint2(g_viewWidth, g_viewHeight)))
            {
                const uint depthBits = f.depthBits & ~COV_DEPTH_SEE_THROUGH;
                visible = depthBits >= asuint(bandDepth[pixel]);  // in front of the band A surface
            }
        }
        // the records the composite can reach to the list (one atomic per wave), the others' radiance to 0
        const uint count = WaveActiveCountBits(visible);
        uint at = 0;
        if (WaveIsFirstLane() && count) recordList.InterlockedAdd(0, count, at);
        at = WaveReadLaneFirst(at) + WavePrefixCountBits(visible);
        if (visible && at < P[10].y) recordList.Store2(4 * (HAIR_SEGMENT_LIST + 2 * at), uint2(block.base + i, block.tile.x | (block.tile.y << 16)));
        if (hair && !visible) output.Store2(8 * (block.base + i), uint2(0, 0));
        if (visible && s < P[9].w)
        {
            // (a segment has many records: most find the bit set and take no atomic; a bit read as 0 while another record
            // sets it is set once more)
            const uint bit = 1u << (s & 31u);
            if ((bits.Load(4 * (s >> 5)) & bit) == 0) bits.InterlockedOr(4 * (s >> 5), bit);
        }
    }
}
#elif MODE == 9
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (P[9].w + 31) / 32) return;
    RWByteAddressBuffer bits = ResourceDescriptorHeap[P[9].x];
    RWByteAddressBuffer segmentList = ResourceDescriptorHeap[P[9].y];
    uint word = bits.Load(4 * id.x);
    const uint count = countbits(word);
    if (count == 0) return;
    uint at;
    segmentList.InterlockedAdd(0, count, at);
    [loop] for (uint k = 0; k < count; ++k)
    {
        const uint bit = firstbitlow(word);
        word &= word - 1;
        segmentList.Store(4 * (HAIR_SEGMENT_LIST + at + k), 32 * id.x + bit);
    }
}
#elif MODE == 10
[numthreads(1, 1, 1)]
void main()
{
    RWByteAddressBuffer segmentList = ResourceDescriptorHeap[P[9].y];
    RWByteAddressBuffer recordList = ResourceDescriptorHeap[P[10].x];
    const uint groups = (min(segmentList.Load(0), P[9].w) * HAIR_SEGMENT_POINTS + 63) / 64;
    segmentList.Store3(4, uint3(min(groups, 65535u), groups == 0 ? 0 : (groups + 65534) / 65535, 1));
    const uint listed = min(recordList.Load(0), P[10].y);
    recordList.Store(0, listed);
    const uint recordGroups = (listed + 63) / 64;
    recordList.Store3(4, uint3(min(recordGroups, 65535u), recordGroups == 0 ? 0 : (recordGroups + 65534) / 65535, 1));
}
#else
[numthreads(64, 1, 1)]
void main(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
    ByteAddressBuffer segmentList = ResourceDescriptorHeap[P[9].y];
    const uint index = (gid.x + gid.y * 65535) * 64 + gi, slot = index / HAIR_SEGMENT_POINTS, node = index % HAIR_SEGMENT_POINTS;
    if (slot >= min(segmentList.Load(0), P[9].w)) return;
    const uint s = segmentList.Load(4 * (HAIR_SEGMENT_LIST + slot));
    StructuredBuffer<float4> segments = ResourceDescriptorHeap[P[1].x];
    RWByteAddressBuffer light = ResourceDescriptorHeap[P[9].z];
    // the point on the segment's axis, seen from the camera
    const float3 offset = lerp(segments[2 * s].xyz, segments[2 * s + 1].xyz, node / float(HAIR_SEGMENT_POINTS - 1));
    const float toCamera = length(offset);
    const HairPoint p = hairPointAt(s, offset, toCamera > 0 ? -offset / toCamera : float3(0, 0, 1), -dot(g_view[2].xyz, offset));
    float3 sunKernel = 0, indirect = 0;
    if (p.valid)
    {
        const uint experiment = P[5].z;
        const HairStrand strand = hairStrand(p.outgoing, p.eta, p.absorption, p.betaM, p.betaN, p.tilt);
        if ((experiment & 16) == 0) sunKernel = hairLit(p, strand, normalize(g_sunDirection), 3.0e38f, hairJitter(s * HAIR_SEGMENT_POINTS + node, 0));
        if ((experiment & 6) != 6) indirect = hairIndirect(p, strand, s * HAIR_SEGMENT_POINTS + node);
    }
    light.Store4(16 * (s * HAIR_SEGMENT_POINTS + node), uint4(covPackRadiance(sunKernel), covPackRadiance(indirect * g_exposure)));
}
#endif
