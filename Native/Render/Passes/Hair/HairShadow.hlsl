// unx-kernel: cs_6_6 main
// unx-variants: MODE=0,1,2
// The hair's shadow on what is not hair (shading.hair_shadows; owner E, recorded by S and by M's MegaLights): the light a
// surface point takes is multiplied by what the grooms on its path to the light let through, exp(-n), n the fibre count
// of E's density volume (HairDensity.hlsli hairTransmittance: the march the strands use for the hair in front of them,
// here from the surface through every body with a block, as far as the light).
//   MODE 0  the sun: per pixel of S's screen visibility, after S's passes - slot 0 times the transmittance towards the
//           sun's centre (pixels in the umbra and the sky are left). Without shading.mega_lights (S's local slots are
//           then what M's froxel-list loop reads): slots 1..3 - the first three shadow-casting lights of the pixel's
//           froxel list - times the transmittance towards each light's position (a light past the third has no slot,
//           so no hair shadow; nor has a light that casts none).
//   MODE 1  shading.mega_lights' samples, after m.ml.trace: the weight of a visible sample of a shadow-casting light is
//           multiplied by exp(-n) towards its point on the light (under 1 / 256 the sample is hidden, so the next
//           frames' sampling knows the light as hidden there). The weight carries it to every reader of the samples
//           without noise; the instance reads a sample of smaller weight as a likelier one, so its history is
//           shorter where hair shadows a light (MegaLightsUpsample.hlsli's confidence).
//   MODE 2  the coverage layer's fragments under the sun (M's composite, CoverageShade.hlsli covFragmentShadow): per
//           pixel with records, the transmittance towards the sun at the 4 depths of S's fragment profile - z_near +
//           (z_far - z_near) k / 3 on the pixel's ray (ViewResources::coverageDepthRange) -, 4 x unorm8 (R32_UINT;
//           a pixel without records: all 255). The composite multiplies a cluster fragment's sun visibility by the
//           value at its depth; the hair records do not read it (a strand counts the hair itself).
// The hair records are not this kernel's: a strand counts the hair in front of it itself (CoverageHair.hlsl).
// The march reads the cells along the whole path (hairTransmittance: steps of a coarse cell, the ones with hair in four
// samples of the cells), its samples moved along the path by a number drawn per pixel and frame (the blue-noise tile):
// what is left of the cells' pattern in a shadow's edge is noise for the temporal filters, as the reference's voxel
// shadow (a jittered traversal converged by its temporal filter).
// Limits: the volume's cells (about 1 cm on a head) - the shadow has no strands in it, and a surface nearer to the hair
// than half a cell is not shadowed by that half cell; the bodies with a block (shading.hair_density_bodies, at most
// HAIR_SHADOW_BODIES); the fibres' directions are not kept (pi / 4 for all). Frames without a density volume record
// neither pass.
// P[0] = { density parameters (raw), the view's device depth (MODE 1: the downsampled key, R32G32_UINT; MODE 2: V's
//          coverageDepthRange, R32G32_UINT), S's visibility UAV (R32_UINT; MODE 1: the samples UAV, R32G32_UINT; MODE 2:
//          the profile UAV, R32_UINT), the march's steps at most (shading.hair_shadow_steps) }
// P[1] = { this view's camera - the volume's origin camera (FrameResources::hairOrigin), xyz (m, floats),
//          shading.hair_march_jitter (0: the midpoint rule) }
// P[2] = { MODE 0: S's froxel light lists (raw SRV; UNX_NONE: the local slots are left); MODE 1: the sample texture's
//          width, height, factor | N << 8, 0 }
#include "Bindless.hlsli"
#include "Frame.hlsli"
#include "Passes/Common/BlueNoise.hlsli"
#if MODE == 0
#include "Passes/Shadow/ShadowVisibility.hlsli"
#endif
#if MODE == 1
#include "RayTracing/HitLocalLights.hlsli"
#include "Passes/Material/MaterialSurface.hlsli"
#include "Passes/Shading/MegaLights.hlsli"
#endif
#include "Passes/Hair/HairDensity.hlsli"

#if MODE == 0
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint2 pixel = id.xy;
    if (any(pixel >= uint2(g_viewWidth, g_viewHeight))) return;
    Texture2D<float> depthTex = ResourceDescriptorHeap[P[0].y];
    const float depth = depthTex[pixel];
    if (depth <= 0) return;
    RWTexture2D<uint> visibility = ResourceDescriptorHeap[P[0].z];
    const uint word = visibility[pixel];
    const uint sun = word & 0xFFu;
    const bool locals = P[2].x != UNX_NONE;
    if (sun == 0 && !locals) return;
    const float3 world = worldFromDepth(float2(pixel), depth);
    const float3 p = world - g_cameraPosition + asfloat(P[1].xyz);
    const float jitter = P[1].w != 0 ? blueNoise1(pixel, g_frameIndex) : 0.5f;
    uint next = word;
    if (sun != 0) next = (next & 0xFFFFFF00u) | (uint)round(sun * hairTransmittance(P[0].x, p, normalize(g_sunDirection), 3.0e38f, P[0].w, jitter));
    if (locals)
    {
        // S's slots 1..3: the first three shadow-casting lights of the pixel's froxel list (ShadowVisibility.hlsli)
        FroxelSrvs froxels;
        froxels.lights = P[2].x;
        froxels.lightIndices = P[2].x;
        froxels.scattering = UNX_NONE;
        froxels.pad = 0;
        const uint3 casters = shadowFirstCasters(froxels, pixel, linearDepth(depth));
        [unroll] for (uint k = 0; k < 3; ++k)
        {
            const uint shift = 8u * (k + 1u), slot = (word >> shift) & 0xFFu;
            if (casters[k] == 0xFFFFFFFFu || slot == 0) continue;
            const float3 toLight = loadLight(casters[k]).position - world;
            const float reach = length(toLight);
            if (!(reach > 1e-4)) continue;
            next = (next & ~(0xFFu << shift)) | ((uint)round(slot * hairTransmittance(P[0].x, p, toLight / reach, reach, P[0].w, jitter)) << shift);
        }
    }
    if (next != word) visibility[pixel] = next;
}
#elif MODE == 2
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint2 pixel = id.xy;
    if (any(pixel >= uint2(g_viewWidth, g_viewHeight))) return;
    Texture2D<uint2> ranges = ResourceDescriptorHeap[P[0].y];
    RWTexture2D<uint> profile = ResourceDescriptorHeap[P[0].z];
    const uint2 range = ranges[pixel];
    uint word = 0xFFFFFFFFu;
    if (!(range.x == 0 && range.y == 0xFFFFFFFFu))
    {
        // S's 4 points (ShadowFragments.hlsl MODE 0): linear in view depth between the pixel's nearest and farthest record
        const float zNear = linearDepth(asfloat(range.x)), zFar = linearDepth(asfloat(range.y));
        const float3 pNear = worldFromDepth(float2(pixel), asfloat(range.x)), pFar = worldFromDepth(float2(pixel), asfloat(range.y));
        const float3 sun = normalize(g_sunDirection), offset = asfloat(P[1].xyz) - g_cameraPosition;
        const float jitter = P[1].w != 0 ? blueNoise1(pixel, g_frameIndex) : 0.5f;
        word = 0;
        [unroll] for (uint k = 0; k < 4; ++k)
        {
            const float3 p = lerp(pNear, pFar, zFar > zNear ? k / 3.0 : 0.0);
            word |= (uint)round(hairTransmittance(P[0].x, p + offset, sun, 3.0e38f, P[0].w, jitter) * 255.0) << (8u * k);
        }
    }
    profile[pixel] = word;
}
#else
// The sample's light as the shadow ray's sampler takes it (MegaLightsWorld.hlsli mlRtLight; that file holds the ray, which
// a compute kernel cannot include).
RtLight hairShadowLight(GpuLight g)
{
    RtLight l;
    l.position = g.position;
    l.type = lightType(g);
    l.forward = g.forward;
    l.intensity = g.intensity;
    l.right = g.right;
    l.range = max(g.range, 1e-3);
    l.up = cross(g.forward, g.right);
    l.spotScale = g.spotScale;
    l.color = g.color;
    l.spotOffset = g.spotOffset;
    l.size = g.size;
    l.castShadow = lightCastsShadow(g) ? 1u : 0u;
    l.pad = 0;
    return l;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint2 texel = id.xy;
    if (any(texel >= P[2].xy)) return;
    RWTexture2D<uint2> samples = ResourceDescriptorHeap[P[0].z];
    const uint2 stored = samples[texel];
    const MlSample s = mlUnpack(stored);
    if (!s.visible || !s.needsRay || s.light == ML_LIGHT_NONE) return;
    const uint factor = P[2].z & 0xFFu, count = (P[2].z >> 8) & 0xFFu;
    const uint2 ds = texel / mlSampleGrid(count);
    Texture2D<uint2> keys = ResourceDescriptorHeap[P[0].y];
    const float linearZ = asfloat(keys[ds].x);
    if (!(linearZ > 0)) return;
    float3 D, Dx, Dy;
    mPixelRay(float2(mlFullPixel(ds, factor, g_frameIndex)) + 0.5, D, Dx, Dy);
    const float3 offset = D * linearZ;
    RtLightSample ls;
    const GpuLight sampled = loadLight(s.light);
    RtLight l = hairShadowLight(sampled);
    if (!rtLightBarnDoors(sampled, g_cameraPosition + offset, l)) return;  // (the point m.ml.trace sampled: MegaLightsWorld.hlsli)
    if (!rtLightSample(l, g_cameraPosition + offset, s.uv.x, s.uv.y, ls)) return;
    const float through = hairTransmittance(P[0].x, offset + asfloat(P[1].xyz), ls.wi, ls.distance, P[0].w, P[1].w != 0 ? blueNoise4(texel, g_frameIndex).y : 0.5f);
    if (through < 1) samples[texel] = through < 1.0 / 256 ? uint2(stored.x & 0x7FFFFFFFu, stored.y) : uint2(stored.x, asuint(s.weight * through));
}
#endif
