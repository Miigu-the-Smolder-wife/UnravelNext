// unx-kernel: lib_6_6 main
// m.ml.trace (MegaLights.hlsli): one ray generation thread per light sample texel. A sample that asks for a ray is traced
// from its downsampled pixel's surface point (the key's view depth along the pixel's ray; the key's normal for the
// offset) toward its point on the light (HitLocalLights' sampler: the centre of point and spot lights, the sample's
// (u, v) on area lights); a blocked sample loses its visible bit. Lights that cast no shadow and merged samples ask for
// none. Ray: origin moved by the normal bias to the light's side of the surface, TMin = the bias, TMax = distance - the
// end bias (r.MegaLights.HardwareRayTracing.Bias / NormalBias / EndBias of Unreal, in metres here; the end bias default
// is S's rule instead: what lies within 5 cm of a light - its own fixture - casts no shadow, VsmLocalLight::nearM).
// Screen traces first (shading.mega_lights_screen_traces; the reference's r.MegaLights.ScreenTraces, MegaLightsRayTracing.usf
// ScreenSpaceRayTraceSamples): the sample's ray is walked across the depth buffer (ScreenTrace.hlsli) for at most the
// screen trace distance (1 m) from the surface point lifted off the surface by its pixel's footprint on the normal x 2
// (the reference's ApplyScreenSpaceRayBias) plus the normal bias; a hit hides the sample and no world ray follows. It is
// what gives small things - and whatever the ray scene does not hold as it is drawn: deformed and displaced surfaces -
// their contact shadows. A walk without a hit is followed by the world ray from the surface, as without it.
// P[0] = { samples UAV (R32G32_UINT), downsampled key SRV, the dispatch's first row, depth SRV (the view's device depth) }:
//        the sample texture goes in bands of rows, each at most 262,144 rays (MegaLights.cpp; DISPATCH_BOUNDS_KO.md)
// P[1] = { downsampled width, height, factor | N << 8, depth pyramid SRV (ScreenTrace.hlsli; UNX_NONE: no screen traces) }
// P[2] = { ray bias, normal bias, end bias (m, floats), the compacted list's SRV + 1 (raw; 0: none) }
//        shading.mega_lights_compact_traces (the reference's CompactLightSampleTraces): the dispatch is one row of threads
//        over the list of the samples that ask for a ray (MegaLightsCompact.hlsl) - a thread's sample texel is entry
//        DispatchRaysIndex().x + P[0].z of it (P[0].z = the dispatch's first entry; at most 262,144 entries a dispatch)
// P[3] = { screen trace normal bias (m), relative depth thickness, largest distance (m) (floats), iterations }
// P[6], P[7] = RtSceneSrvs (RayShaders.hlsli)
#include "RayTracing/RayShaders.hlsli"
#include "RayTracing/HitLocalLights.hlsli"
#include "Passes/Material/MaterialSurface.hlsli"
#include "Passes/Shading/MegaLightsWorld.hlsli"  // mlSampleVisible
#include "Passes/Reflection/ScreenTrace.hlsli"

[shader("raygeneration")]
void MegaLightsTraceGen()
{
    uint2 texel = uint2(DispatchRaysIndex().x, DispatchRaysIndex().y + P[0].z);
    if (P[2].w != 0)
    {
        ByteAddressBuffer traceList = ResourceDescriptorHeap[P[2].w - 1];
        const uint entry = traceList.Load(16 + 4 * (DispatchRaysIndex().x + P[0].z));
        texel = uint2(entry & 0xFFFFu, entry >> 16);
    }
    RWTexture2D<uint2> samples = ResourceDescriptorHeap[P[0].x];
    const uint2 stored = samples[texel];
    const MlSample s = mlUnpack(stored);
    if (!s.needsRay || s.light == ML_LIGHT_NONE) return;
    const uint factor = P[1].z & 0xFFu, count = (P[1].z >> 8) & 0xFFu;
    const uint2 ds = texel / mlSampleGrid(count);
    Texture2D<uint2> keys = ResourceDescriptorHeap[P[0].y];
    const uint2 key = keys[ds];
    const float linearZ = asfloat(key.x);
    if (!(linearZ > 0)) return;
    const uint2 pixel = mlFullPixel(ds, factor, g_frameIndex);
    float3 D, Dx, Dy;
    mPixelRay(float2(pixel) + 0.5, D, Dx, Dy);
    const float3 x = g_cameraPosition + D * linearZ;
    const float3 n = octDecode(key.y);
    if (P[1].w != UNX_NONE)
    {
        const GpuLight g = loadLight(s.light);
        RtLightSample ls;
        if (rtLightSample(mlRtLight(g), x, s.uv.x, s.uv.y, ls) && dot(n, ls.wi) > 0)
        {
            float3 Dc, Dcx, Dcy;
            mPixelRay(float2(pixel) + 1.0, Dc, Dcx, Dcy);  // the pixel's corner at the surface's depth
            const float lift = abs(dot(g_cameraPosition + Dc * linearZ - x, n)) * 2.0 + asfloat(P[3].x);
            const float reach = min(ls.distance - lightRayEndBias(g, asfloat(P[2].z)), asfloat(P[3].z));
            if (reach > 0)
            {
                Texture2D<float> depth = ResourceDescriptorHeap[P[0].w];
                Texture2D<float> pyramid = ResourceDescriptorHeap[P[1].w];
                const SctResult walk = sctTrace(depth, pyramid, uint2(g_viewWidth, g_viewHeight), x + n * lift, ls.wi, reach, P[3].w, asfloat(P[3].y), 0);
                if (walk.hit)
                {
                    samples[texel] = uint2(stored.x & 0x7FFFFFFFu, stored.y);
                    return;
                }
            }
        }
    }
    const bool visible = mlSampleVisible(rtScene(), x, n, s.light, s.uv, asfloat(P[2].x), asfloat(P[2].y), asfloat(P[2].z));
    if (!visible) samples[texel] = uint2(stored.x & 0x7FFFFFFFu, stored.y);
}
