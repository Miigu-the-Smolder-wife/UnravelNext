// unx-kernel: cs_6_6 main
// R gate only: the M-facing screen-probe lookups at every pixel, as M's shading kernel calls them, so their per-pixel
// cost is measured in isolation (request 20260925_M_shading_lookup_cost.md). Per pixel: the G-buffer normal (viewer
// side), the K path's reflection direction and lobe half-angle (reflectionLobeHalfAngle), then by P[1].x:
//   1 = screenProbeIrradiance, 2 = screenProbeRadiance (K path; every pixel, as a worst case), 3 = both,
//   4 = the four-probe footprint alone (its weights), 0 = none (the kernel's own reads and write),
//   8 = screenProbeGather (irradiance + K radiance in one footprint, maps atlas; v1.13),
//   16 = M's GI cache irradiance alone (giCacheIrradianceScreen, front side: ScreenProbes pad1), in a kernel of its own
//   occupancy (against its cost inside M's shading kernel).
// P[1].w = view.screenProbeMaps SRV, P[2].x = GI cache SRV (mode 16).
// The sum is written to an RGBA16F target so nothing is optimised away.
// P[0] = { probes SRV, depth SRV, gbuffer SRV, output UAV }, P[1] = { mode, width, height, 0 }; b1 = the main view.
#include "GBuffer.hlsli"
#include "Passes/GI/ScreenProbes.hlsli"
#include "Passes/Reflection/Reflection.hlsli"

[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    if (any(pixel >= P[1].yz)) return;
    Texture2D<float> depth = ResourceDescriptorHeap[P[0].y];
    Texture2D<uint2> gbuffer = ResourceDescriptorHeap[P[0].z];
    RWTexture2D<float4> output = ResourceDescriptorHeap[P[0].w];
    const float d = depth.Load(int3(pixel, 0));
    if (d <= 0)
    {
        output[pixel] = 0;
        return;
    }
    const GBufferSample g = decodeGBuffer(gbuffer.Load(int3(pixel, 0)));
    const float3 position = worldFromDepth(float2(pixel), d);
    const float3 v = normalize(g_cameraPosition - position);
    const float NoV = dot(g.normal, v);
    const float3 n = NoV > 0 ? g.normal : -g.normal;
    const float z = linearDepth(d);
    const ProbeSrvs probes = { P[0].x, P[0].x, 0, 0 };
    float4 result = 0;
    if (P[1].x & 1) result += screenProbeIrradiance(probes, pixel, n, z);
    if (P[1].x & 4)
    {
        Texture2D<uint4> t = ResourceDescriptorHeap[P[0].x];
        float spacing;
        int2 count;
        const GiProbeFootprint fp = giProbeFootprint(t, pixel, n, z, spacing, count);
        result += float4(fp.weight[0], fp.weight[1], fp.weight[2], fp.weight[3]) + float4(fp.probe[0] + fp.probe[3], fp.probe[1] + fp.probe[2]);
    }
    if (P[1].x & 8)
    {
        const ProbeSrvs gs = { P[0].x, P[0].x, P[1].w, 0 };
        const ScreenProbeLighting l = screenProbeGather(gs, pixel, position, n, z, false, true, reflect(-v, g.normal), reflectionLobeHalfAngle(g.roughness, abs(NoV)));
        result += float4(l.irradiance + l.radiance, l.occlusion);
    }
    if (P[1].x & 16)
    {
        ByteAddressBuffer cache = ResourceDescriptorHeap[P[2].x];
        const GiHeader h = giHeader(cache);
        float weight;
        result += float4(giCacheIrradianceScreen(cache, h, position, n, weight), weight);
    }
    if (P[1].x & 2) result.rgb += screenProbeRadiance(probes, pixel, g.normal, z, reflect(-v, g.normal), reflectionLobeHalfAngle(g.roughness, abs(NoV)));
    output[pixel] = result;
}
