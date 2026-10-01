// unx-kernel: lib_6_6 main
// s.ml.volume (shading.mega_lights with shading.mega_lights_volume; Passes/Shading/MegaLights.hlsli; owner A): the local
// lights' in-scattering of the air by light samples, the volume counterpart of m.ml.sample / trace / shade. The structure
// follows Unreal's MegaLights volume (samples per voxel, a shadow ray each, the fog's temporal history); the code is ours.
// One ray generation thread per froxel (tile, slice) of the main view's grid:
//   candidates  the froxel list's lights whose range the slice's segment enters, weighed by log2(1 + exposed luminance of
//               the light's irradiance at the segment's midpoint) (the smooth cut under the minimum sample weight);
//   samples     N (2) by the stratified reservoir (MegaLightsSampling.hlsli); weight = sum / (w N), capped;
//   visibility  shadow casters: one ray from a random point of the segment (and of the tile's footprint) toward the
//               sample's point on the light; lights without shadows need none;
//   value       sum over the samples of weight x the light's in-scattering over the whole segment (FroxelSlice.hlsli
//               airLocalLight: the same 8-point rule the integration uses), in nits;
//   history     the froxel's midpoint in the previous view's grid, trilinear; value = lerp(history, now, 1 / n),
//               n = min(n_prev + 1, max frames). No history: n = 1.
// Output RGBA16F gridX x gridY x S: rgb = the slice's local in-scattering x exposure, a = n. FroxelIntegrate.hlsl adds it
// in place of its loop over the list (P[5].y there). Slices no reader reaches hold 0.
// P[0] = { froxel lights (raw), output UAV, transmittance LUT (the air's parameters), light functions (UNX_NONE: none) }
// P[1] = { previous output SRV (UNX_NONE: no history), N, tile readers SRV (FroxelTileDepth; UNX_NONE: every slice), 0 }
// P[2] = { minimum sample weight, ray bias (m), end bias (m), exposure now / previous } (floats)
// P[3] = { weight cap, max frames } (floats)
// P[6], P[7] = RtSceneSrvs
#include "RayTracing/RayShaders.hlsli"
#include "RayTracing/HitLocalLights.hlsli"
#include "Passes/Atmosphere/FroxelSlice.hlsli"
#define ML_AREA 0  // (the air sees area lights as point sources of their projected intensity: FroxelCommon.hlsli)
#include "Passes/Shading/MegaLightsWorld.hlsli"

[shader("raygeneration")]
void MegaLightsVolumeGen()
{
    const uint3 id = DispatchRaysIndex();
    const uint2 tile = id.xy;
    const uint s = id.z;
    RWTexture3D<float4> output = ResourceDescriptorHeap[P[0].y];
    const FroxelGrid g = froxelGrid(P[0].x);
    const float3 ray = froxelTileRay(g, tile);
    const float toRay = length(ray);
    const float3 dir = ray / toRay;
    const float tStart = airViewStart(g_clipPlane, g_cameraPosition, dir);
    const float zs0 = froxelNodeDepth(g, s), zs1 = froxelNodeDepth(g, s + 1);
    bool wanted = zs1 * toRay > tStart;
    if (wanted && P[1].z != UNX_NONE)
    {
        // a reader of the tile's 3 x 3 neighbourhood reaches this slice: a surface at or behind it, or a sky pixel
        Texture2D<float2> readers = ResourceDescriptorHeap[P[1].z];
        float zSurface = 0;
        bool sky = false;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx)
            {
                const float2 r = readers[clamp(int2(tile) + int2(dx, dy), 0, int2(g.gridX, g.gridY) - 1)];
                zSurface = max(zSurface, r.x);
                sky = sky || r.y > 0;
            }
        wanted = sky || zs0 < zSurface;
    }
    ByteAddressBuffer lists = ResourceDescriptorHeap[P[0].x];
    const uint2 h = lists.Load2(g.headerBase + froxelIndex(g, tile, s) * 8);
    if (!wanted || h.y == 0)
    {
        output[id] = float4(0, 0, 0, wanted ? 1 : 0);
        return;
    }
    const AtmosphereParams a = airParamsFromTexels(P[0].z);
    const float t0 = max(zs0 * toRay, tStart), len = zs1 * toRay - t0;
    const float3 o = g_cameraPosition + dir * t0;
    const float3 pm = airLiftToSurface(a, o + dir * (0.5 * len));
    const AirCoefficients cm = airCoefficients(a, max(0.0, airAltitude(a, pm)));
    const float lateral = froxelTileWidth(g, 0.5 * (zs0 + zs1));
    const uint count = clamp(P[1].y, 1u, ML_MAX_SAMPLES);
    const float minWeight = asfloat(P[2].x);

    // ---- candidates
    MlReservoir r = mlReservoirBegin(mlNoise(tile + uint2(s * 7u, s * 13u), g_frameIndex, 0), count);
    uint i;
    for (i = 0; i < h.y; ++i)
    {
        const uint pos = h.x + i;
        const uint w16 = lists.Load(g.indexBase + (pos >> 1) * 4);
        const uint li = ((pos & 1) ? w16 >> 16 : w16) & 0x7FFFu;
        const GpuLight light = loadLight(li);
        if (airLocalMap(light, o, dir, len).h >= light.range) continue;  // the segment's line never enters the range: adds 0
        const float3 v = (o + dir * (0.5 * len)) - light.position;
        const float d = max(length(v), 0.01);
        const float lum = froxelIntensity(light, v / d) * froxelWindow(light, d) / (d * d) * mlLuminance(light.color) * g_exposure;
        const float w = mlTargetWeight(lum, minWeight);
        if (w > 0) mlOffer(r, count, w, li, true);
    }

    // ---- the samples' in-scattering
    float3 now = 0;
    const RtSceneSrvs scene = rtScene();
    for (i = 0; i < count; ++i)
    {
        if (r.light[i] == ML_LIGHT_NONE) continue;
        const uint li = r.light[i];
        const GpuLight light = loadLight(li);
        if (lightCastsShadow(light))
        {
            const float u = mlNoise(tile + uint2(s * 7u, s * 13u), g_frameIndex, 6 + i);
            const float2 j = float2(mlNoise(tile + uint2(s * 3u, s * 5u), g_frameIndex, 10 + i), mlNoise(tile + uint2(s * 5u, s * 3u), g_frameIndex, 14 + i));
            const float3 jr = froxelRayAt((float2(tile) + j) * g.tilePx);
            const float3 x = g_cameraPosition + jr * ((t0 + len * u) / toRay);
            const float2 uv = float2(mlNoise(tile + uint2(s * 11u, s * 2u), g_frameIndex, 18 + i), mlNoise(tile + uint2(s * 2u, s * 11u), g_frameIndex, 22 + i));
            if (!mlSampleVisible(scene, x, float3(0, 0, 0), li, uv, asfloat(P[2].y), 0, asfloat(P[2].z))) continue;
        }
        const float weight = min(r.sum / r.weight[i], asfloat(P[3].x)) / count;
        now += airLocalLight(light, o, dir, len, cm, a.mieG, P[0].w, li, lateral) * weight;
    }
    now *= g_exposure;
    if (any(isnan(now)) || any(isinf(now))) now = 0;

    // ---- history
    float n = 1;
    float3 value = now;
    if (P[1].x != UNX_NONE)
    {
        const float3 mid = o + dir * (0.5 * len);
        const float4 clip = mul(g_prevViewProj, float4(mid, 1));
        if (clip.w > 0)
        {
            const float2 ndc = clip.xy / clip.w;
            const float3 uvw = float3(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5, max(froxelSliceCoord(g, clip.w), 0.5) / g.slices);  // (slice 0 starts at the camera)
            if (all(uvw > 0) && all(uvw < 1))
            {
                Texture3D<float4> previous = ResourceDescriptorHeap[P[1].x];
                const float4 hist = previous.SampleLevel(g_linearClamp, uvw, 0);
                if (hist.a > 0)
                {
                    n = min(hist.a + 1, asfloat(P[3].y));
                    value = lerp(hist.rgb * asfloat(P[2].w), now, 1 / n);
                }
            }
        }
    }
    output[id] = float4(min(value, 60000.0), n);
}
