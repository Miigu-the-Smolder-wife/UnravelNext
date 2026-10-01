// unx-kernel: cs_6_6 main
// unx-variants: AREA=0,1
// m.ml.sample (MegaLights.hlsli): one thread per downsampled pixel. The pixel of the block this frame stands on gives the
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
// P[5] = { B2 stable area lights' mask (UNX_NONE: none), 0, 0, 0 }
#include "Bindless.hlsli"
#include "GBuffer.hlsli"
#include "Passes/Material/MaterialInternal.hlsli"
#include "Passes/Material/MaterialSurface.hlsli"
#include "Passes/Shading/ShadingCommon.hlsli"
#include "Passes/Shading/AreaLight.hlsli"
#include "Passes/Atmosphere/Froxel.hlsli"
#include "Passes/GI/GiScreenHistory.hlsli"
#include "Passes/Shading/MegaLights.hlsli"

struct MlReservoir
{
    uint light[ML_MAX_SAMPLES];
    float weight[ML_MAX_SAMPLES];
    bool wasVisible[ML_MAX_SAMPLES];
    float u[ML_MAX_SAMPLES];
    float sum;
};

// One candidate into the N strata: each stratum keeps its light with probability sum / (sum + w) and its random number
// stays uniform either way.
void mlOffer(inout MlReservoir r, uint n, float w, uint light, bool wasVisible)
{
    const float keep = r.sum / (r.sum + w);
    r.sum += w;
    for (uint i = 0; i < n; ++i)
    {
        if (r.u[i] < keep) r.u[i] /= keep;
        else
        {
            r.u[i] = (r.u[i] - keep) / (1 - keep);
            r.light[i] = light;
            r.weight[i] = w;
            r.wasVisible[i] = wasVisible;
        }
        r.u[i] = clamp(r.u[i], 0.0, 0.99999994);
    }
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint2 ds = id.xy;
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
    const float3 diffuse = s.baseColor * ((1 - s.metallic) / SH_PI);
    const float3 f0 = modelF0(s);
    const float alpha = modelAlpha(s.roughness);
    const bool foliage = s.cls == MATERIAL_FOLIAGE;
    const float3 front = foliage ? diffuse * (1 - s.transmission) : diffuse;
    const float3 back = foliage ? diffuse * s.transmission : 0;
    const float e = modelDirectionalAlbedo(max(NoV, 1e-4), s.roughness);
    const float3 compensation = 1 + f0 * (1 / e - 1);
#if AREA
    const float3x3 frame = shShadingFrame(n, v, NoV);
    const float3x3 frameBack = float3x3(frame[0], -frame[1], -frame[2]);
    const float3x3 specular = mul(shLtcInverse(P[2].w, max(NoV, 1e-4), s.roughness), frame);
    const float3 specularAlbedo = shSpecularAlbedo(f0, max(NoV, 1e-4), s.roughness);
#endif

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
    MlReservoir r;
    r.sum = 0;
    const float u0 = mlNoise(ds, g_frameIndex, 0);
    for (i = 0; i < ML_MAX_SAMPLES; ++i)
    {
        r.light[i] = ML_LIGHT_NONE;
        r.weight[i] = 0;
        r.wasVisible[i] = true;
        r.u[i] = (u0 + i) / max(count, 1u);
    }
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
        float lum = 0;
        if (lightType(light) > LIGHT_SPOT)
        {
#if AREA
            const float3 p = (light.position - g_cameraPosition) - offset;
            const float window = shAreaWindow(light, p);
            if (window <= 0) continue;
            float3 c = 0;
            if (NoV > 0)
            {
                c = front * (SH_PI * shAreaIntegral(light, p, frame, true));
                if (!shSpecularInReflections(P[5].x, lightIndex)) c += specularAlbedo * shAreaIntegral(light, p, specular, false);
            }
            if (foliage) c += back * (SH_PI * shAreaIntegral(light, p, NoV > 0 ? frameBack : frame, true));
            lum = mlLuminance(light.color * c) * (light.intensity * window);
#else
            continue;  // AREA=0: the scene has no area lights
#endif
        }
        else
        {
            const float3 toLight = (light.position - g_cameraPosition) - offset;
            float3 l;
            const float3 E = shPunctualIlluminance(light, toLight, l);
            const float cosL = dot(n, l);
            if (all(E == 0)) continue;
            float3 f = 0;
            if (NoV > 0 && cosL > 0) f = front + shSpecular(f0, alpha, compensation, n, v, l, NoV, cosL);
            else if (foliage && NoV * cosL < 0) f = back;
            lum = mlLuminance(f * E) * abs(cosL);
        }
        lum *= g_exposure;
        float w = log2(1 + lum * mlFalloffMask(lum, minWeight));
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
            o.weight = r.sum / r.weight[i];
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
