// unx-kernel: cs_6_6 main
// r.gi.ltv.integrate (LumenTranslucencyVolume.hlsli): one thread per cell. The cell's 9 filtered rays projected on 4 SH
// coefficients per colour (each ray stands for 4 pi / 9 sr), blended with the cell's history - the previous frame's
// volume read at the cell centre's place in the previous view (weight P[9].z, Unreal's 0.9; no history where that place
// is outside the previous view, on the first frame and after a cut). A cell the view does not see into keeps its history.
// Stored: ambient = band 0 per colour, directional = band 1 luminance-weighted (LumenTranslucencyVolume.hlsli).
// P[0] = { filtered trace SRV (Texture3D), depth pyramid SRV, previous ambient SRV, previous directional SRV }
// P[1] = { ambient UAV, directional UAV, flags (bit 0: no history), 0 }
// P[4] = { grid x, grid y, grid z, 0 }, P[8] = { cell jitter, frame }, P[9].z = asuint(history weight),
// P[10] = asuint(view size / (grid x 32): the volume's uv scale, x y; 0, 0)
#include "Passes/GI/LumenRadianceCache.hlsli"
#include "Passes/GI/LumenTranslucencyVolumeGrid.hlsli"

[numthreads(4, 4, 4)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint3 grid = ltvGridSize();
    if (any(id >= grid)) return;
    float3 sh0 = 0;                 // band 0 per colour
    float3 sh1[3] = { float3(0, 0, 0), float3(0, 0, 0), float3(0, 0, 0) };  // band 1 (y, z, x) per colour
    const bool visible = ltvCellVisible(id, P[0].y);
    if (visible)
    {
        Texture3D<float3> trace = ResourceDescriptorHeap[P[0].x];
        // (the rays' texels at their centres: the spatial filter has mixed the neighbours' jittered directions)
        const float2 jitter = float2(0.5, 0.5);
        const float solidAngle = 4 * LTV_PI / float(LTV_TRACE_RES * LTV_TRACE_RES);
        for (uint t = 0; t < LTV_TRACE_RES * LTV_TRACE_RES; ++t)
        {
            const uint2 texel = uint2(t % LTV_TRACE_RES, t / LTV_TRACE_RES);
            const float3 radiance = trace.Load(int4(id.xy * LTV_TRACE_RES + texel, id.z, 0));
            const float3 d = lrcUvToDirection((float2(texel) + jitter) / float(LTV_TRACE_RES));
            sh0 += radiance * (0.282095 * solidAngle);
            sh1[0] += radiance * (0.488603 * d.y * solidAngle);
            sh1[1] += radiance * (0.488603 * d.z * solidAngle);
            sh1[2] += radiance * (0.488603 * d.x * solidAngle);
        }
    }
    const float3 lum = float3(0.2126, 0.7152, 0.0722);
    float3 ambient = sh0, directional = float3(dot(sh1[0], lum), dot(sh1[1], lum), dot(sh1[2], lum));

    float alpha = visible ? asfloat(P[9].z) : 1.0;
    if ((P[1].z & 1u) != 0) alpha = 0;
    if (alpha > 0)
    {
        // the cell's centre in the previous view
        const float3 world = ltvCellPosition(float3(id) + 0.5);
        const float4 g = ltvGridPosition(g_prevViewProj, world);
        if (any(g.xy < 0) || any(g.xy >= 1) || g.w <= 0 || g.z >= float(grid.z)) alpha = 0;
        else
        {
            const float3 uvw = float3(g.xy * asfloat(P[10].xy), clamp(g.z / float(grid.z), 0.5 / float(grid.z), 1.0 - 0.5 / float(grid.z)));
            Texture3D<float4> previousAmbient = ResourceDescriptorHeap[P[0].z];
            Texture3D<float4> previousDirectional = ResourceDescriptorHeap[P[0].w];
            ambient = lerp(ambient, previousAmbient.SampleLevel(g_linearClamp, uvw, 0).rgb, alpha);
            directional = lerp(directional, previousDirectional.SampleLevel(g_linearClamp, uvw, 0).rgb, alpha);
        }
    }
    if (any(isnan(ambient)) || any(isinf(ambient)) || any(isnan(directional)) || any(isinf(directional))) ambient = directional = 0;
    RWTexture3D<float4> outAmbient = ResourceDescriptorHeap[P[1].x];
    RWTexture3D<float4> outDirectional = ResourceDescriptorHeap[P[1].y];
    outAmbient[id] = float4(ambient, 0);
    outDirectional[id] = float4(directional, 0);
}
