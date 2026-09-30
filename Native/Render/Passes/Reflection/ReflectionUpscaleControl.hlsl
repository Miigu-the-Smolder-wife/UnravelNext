// unx-kernel: cs_6_6 main
// Reconstruction control: preserve the deterministic angular response to each
// sample's normal. Only the ray-minus-control residual is temporally filtered.
#include "Passes/Reflection/ReflectionInternal.hlsli"
#include "Passes/GI/ScreenProbes.hlsli"
[numthreads(8, 8, 1)]
void main(uint2 p : SV_DispatchThreadID)
{
    const uint2 size = uint2(P[1].w, P[2].x);
    if (any(p >= size)) return;
    RWTexture2D<float4> output = ResourceDescriptorHeap[P[1].z];
    output[p] = 0;
    Texture2D<float4> reflection = ResourceDescriptorHeap[P[0].x];
    if (reflection.Load(int3(p.x / 8, size.y + p.y / 8, 0)).a < 0.5) return;
    Texture2D<uint> modes = ResourceDescriptorHeap[P[0].y];
    const uint mode = reflMode(modes.Load(int3(p, 0)));
    if (mode != REFL_G) return;
    Texture2D<float> depth = ResourceDescriptorHeap[P[0].z];
    Texture2D<uint2> gbuffer = ResourceDescriptorHeap[P[0].w];
    const ReflSurface s = reflSurface(depth, gbuffer, p);
    Texture2D<uint4> probes = ResourceDescriptorHeap[P[1].x];
    float spacing;
    int2 count;
    const GiProbeFootprint fp = giProbeFootprintAt(probes, p, s.position, s.normal, s.linearDepth, spacing, count);
    const float cone = reflectionLobeHalfAngle(s.roughness, max(dot(s.normal, s.view), 1e-4));
    const float3 control = giProbeFootprintRadiance(probes, fp, count, reflect(-s.view, s.normal), cone, P[1].y);
    output[p] = float4(control, 1);
}
