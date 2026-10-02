// unx-kernel: cs_6_6 main
// unx-variants: AREA=0,1
// m.ml.sample (MegaLights.hlsli): one group per listed downsampled tile (m.ml.tiles), one thread per downsampled pixel. The pixel of the block this frame stands on gives the
// surface (G-buffer, depth, material word); every light of its froxel list is weighed by
//   w = log2(1 + L m(L)),  L = luminance of the light's unshadowed radiance at the pixel x exposure
// (the base model's diffuse and specular; area lights by their exact diffuse and LTC integrals; m: the smooth cut under the
// minimum sample weight), times the hidden weight when the light was not among the visible lights of the previous
// frame's tile; a stratified weighted reservoir (one random number, N strata) keeps N of them, each with weight
// sum(w) / w. A sample of a shadow-casting light asks for a ray; consecutive samples of one light share one ray unless
// the light is an area light in a penumbra (a tile that saw it both visible and hidden), where each gets its own point.
// P[0] = { G-buffer, depth, material word, froxel lights (raw) }
// P[1] = { samples UAV (R32G32_UINT, N per downsampled pixel), downsampled key UAV (R32G32_UINT: view depth bits, the
//          G-buffer's packed normal; 0 = no surface), the previous frame's tile sets (raw, 24 B per tile; UNX_NONE: none),
//          the previous frame's view depth (R32_FLOAT, m.ml.temporal's key; UNX_NONE: none) }
// P[2] = { downsampled width, height, factor | N << 8 | flags << 16 (1: guide by history, 2: merge rays), LTC table }
// P[3] = { minimum sample weight, hidden weight, hidden weight without history, history distance threshold } (floats)
// P[4] = { vis id, visible clusters (UNX_NONE: static reprojection), sets' tiles X, tiles Y }
// P[5] = { B2 stable area lights' mask (UNX_NONE: none), the tile list (raw; MegaLightsTiles.hlsl), 0, 0 }
// P[6] = { weight cap, weight cap of a light guided as hidden, the caps' scale with the effective number of lights
//          (mlWeightCap, MegaLightsSampling.hlsli; 0: the constant caps) } (floats)
#include "Bindless.hlsli"
#include "GBuffer.hlsli"
#include "Passes/Material/MaterialInternal.hlsli"
#include "Passes/Material/MaterialSurface.hlsli"
#define ML_AREA AREA
#include "Passes/Shading/MegaLightsSampling.hlsli"  // the point, the target weight and the reservoir (shared with world points)
#include "Passes/Atmosphere/Froxel.hlsli"
#include "Passes/GI/GiScreenHistory.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID)
{
    ByteAddressBuffer tileList = ResourceDescriptorHeap[P[5].y];
    const uint tile = tileList.Load(16 + 4 * gid.x);
    const uint2 ds = uint2(tile & 0xFFFFu, tile >> 16) * 8 + tid.xy;
    if (any(ds >= P[2].xy)) return;
    const uint factor = P[2].z & 0xFFu, count = (P[2].z >> 8) & 0xFFu, flags = P[2].z >> 16;
    const bool guide = (flags & 1u) != 0, merge = (flags & 2u) != 0;
    const uint2 pixel = mlFullPixel(ds, factor, g_frameIndex);
    RWTexture2D<uint2> samples = ResourceDescriptorHeap[P[1].x];
    RWTexture2D<uint2> keys = ResourceDescriptorHeap[P[1].y];
    Texture2D<uint> words = ResourceDescriptorHeap[P[0].z];
    Texture2D<uint2> gbuffer = ResourceDescriptorHeap[P[0].x];
    Texture2D<float> depthTex = ResourceDescriptorHeap[P[0].y];
    const uint word = words[pixel];
    const uint2 gbPacked = gbuffer[pixel];
    const float depthValue = depthTex[pixel];
    const uint materialIndex = mWordMaterial(word);
    uint i;
    if (materialIndex == M_MATERIAL_SKY || P[0].w == UNX_NONE)
    {
        for (i = 0; i < count; ++i) samples[mlSampleCoord(ds, count, i)] = mlPack(mlNoSample());
        keys[ds] = uint2(0, 0);
        return;
    }
    const GpuMaterial m = loadMaterial(materialIndex);
    const GBufferSample g = decodeGBuffer(gbPacked);
    const float linearZ = linearDepth(depthValue);
    keys[ds] = uint2(asuint(linearZ), gbPacked.x);
    float3 D, Dx, Dy;
    mPixelRay(float2(pixel) + 0.5, D, Dx, Dy);
    const float3 offset = D * linearZ;
    const float3 worldPos = g_cameraPosition + offset;
    const float3 v = -normalize(D);
    const float3 n = mNormalTowardsViewer(g.normal, v);
    const float NoV = dot(n, v);

    ModelSurface s;
    s.cls = materialClass(m);
    s.baseColor = g.baseColor;
    s.roughness = g.roughness;
    s.metallic = mWordMetallic(word);
    s.specular = m.specular;
    s.transmission = m.transmission;
    const MlPoint surfacePoint = mlPointOf(s, offset, n, v, P[2].w);

    // ---- the previous frame's tile sets at this surface point (a random offset of half a tile stands in for a bilinear
    // lookup of the sets); without history every light counts as visible
    const float minWeight = asfloat(P[3].x);
    uint4 visibleSet = 0xFFFFFFFFu;
    uint2 hiddenSet = 0;
    bool historyValid = false;
    if (guide && P[1].z != UNX_NONE)
    {
        float2 prevPixel = float2(pixel) + 0.5;
        if (P[1].w != UNX_NONE)
        {
            float3 prevP, prevN;
            uint instance;
            giPreviousSurface(P[4].x, P[4].y, pixel, worldPos, n, prevP, prevN, instance);
            float2 pp;
            float prevDepth;
            if (giPreviousPixel(prevP, float2(g_viewWidth, g_viewHeight), pp, prevDepth) && all(pp >= 0) && all(pp < float2(g_viewWidth, g_viewHeight)))
            {
                Texture2D<float> prevKeys = ResourceDescriptorHeap[P[1].w];
                const float d = prevKeys[uint2(pp)];
                historyValid = d > 0 && abs(d - prevDepth) < prevDepth * asfloat(P[3].w) / lerp(0.1, 1.0, saturate(NoV));
                prevPixel = pp;
            }
        }
        prevPixel += (float2(mlNoise(ds, g_frameIndex, 1), mlNoise(ds, g_frameIndex, 2)) - 0.5) * ML_HASH_TILE;
        const uint2 tile = uint2(clamp(int2(floor(prevPixel / ML_HASH_TILE)), int2(0, 0), int2(P[4].zw) - 1));
        ByteAddressBuffer sets = ResourceDescriptorHeap[P[1].z];
        const uint at = (tile.y * P[4].z + tile.x) * (4 * ML_HASH_WORDS);
        visibleSet = sets.Load4(at);
        hiddenSet = sets.Load2(at + 16);
    }

    // ---- candidates: the froxel list
    MlReservoir r = mlReservoirBegin(mlNoise(ds, g_frameIndex, 0), count);
    FroxelSrvs froxels;
    froxels.lights = P[0].w;
    froxels.lightIndices = P[0].w;
    froxels.scattering = UNX_NONE;
    froxels.pad = 0;
    const uint2 range = froxelLightRange(froxels, pixel, linearZ);
    const uint indexBase = froxelIndexBase(froxels);
    uint4 lightWords = 0;
    for (i = 0; i < range.y; ++i)
    {
        const uint lightIndex = froxelLightBuffered(froxels, indexBase, range, i, lightWords);
        const GpuLight light = loadLight(lightIndex);
        const float lum = mlLuminance(mlLightUnshadowed(surfacePoint, light, lightIndex, P[5].x)) * g_exposure;
        float w = mlTargetWeight(lum, minWeight);
        if (!(w > 0)) continue;
        bool wasVisible = true;
        if (guide)
        {
            // (a light under the minimum weight was not sampled last frame: not being in the set says nothing about it)
            wasVisible = mlInVisible(visibleSet, lightIndex) || lum < minWeight;
            if (!wasVisible) w *= historyValid ? asfloat(P[3].y) : asfloat(P[3].z);
        }
        mlOffer(r, count, w, lightIndex, wasVisible);
    }

    // ---- the N samples
    for (i = 0; i < count; ++i)
    {
        MlSample o = mlNoSample();
        if (r.light[i] != ML_LIGHT_NONE)
        {
            const GpuLight light = loadLight(r.light[i]);
            const bool area = lightType(light) > LIGHT_SPOT;
            const uint2 coord = mlSampleCoord(ds, count, i);
            o.light = r.light[i];
            o.visible = true;
            o.guidedVisible = guide ? r.wasVisible[i] : true;
            // (the weight caps are applied here, with the place's effective number of lights - mlWeightCap; the shading
            // kernels' own caps are open)
            o.weight = min(r.sum / r.weight[i], mlWeightCap(r, o.guidedVisible ? asfloat(P[6].x) : asfloat(P[6].y), asfloat(P[6].z)));
            o.needsRay = lightCastsShadow(light);
            if (area) o.uv = float2(mlNoise(coord, g_frameIndex, 3), mlNoise(coord, g_frameIndex, 4));
            bool penumbra = true;
            if (guide && historyValid) penumbra = r.wasVisible[i] && mlInHidden(hiddenSet, r.light[i]);
            if (merge && !(area && penumbra) && i + 1 < count && r.light[i + 1] == r.light[i])
            {
                o.merged = true;  // the next sample of this light carries the ray and the weight of both
                o.visible = false;
                o.needsRay = false;
                o.weight = 0;
            }
        }
        samples[mlSampleCoord(ds, count, i)] = mlPack(o);
    }
}
