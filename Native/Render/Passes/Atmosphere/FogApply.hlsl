// unx-kernel: cs_6_6 main
// m.fog (FogVolume.hlsli): the fog over the lit opaque image, one thread per pixel, in place. To the pixel's depth: the
// fog's volume (in-scattered radiance and transmittance between the camera and that depth, read between its cells);
// past the volume's end: the exponential height fog in closed form (Fog.hlsli fogOpticalDepth) scattering the column's
// far source. colour = colour x T + L x exposure.
// Sky pixels take P[4].x of it (atmosphere.fog.sky_amount; 0: the sky is left alone, as the reference fogs rendered
// opaque pixels only where a sky atmosphere draws the sky), along the ray to 100 km.
// Runs after the scene colour is kept for the screen traces (they read light before the fog) and before the
// translucent layer.
// P[0] = { grid x | y << 16, z | cell px << 16, asuint(far m), asuint(k) }
// P[1] = { asuint(b), integrated SRV (FogIntegrate.hlsl), far source SRV, colour UAV (RGBA16F, exposed) }
// P[2] = asuint{ density, height falloff, height, phase g }, P[3] = asuint{ albedo r, g, b, start distance }
// P[4] = { asuint(sky amount), depth SRV, 0, 0 }
// Frame constants of the view.
#include "Bindless.hlsli"
#include "Frame.hlsli"
#include "Passes/Atmosphere/FroxelCommon.hlsli"
#include "Passes/Atmosphere/FogVolume.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_viewWidth || id.y >= g_viewHeight) return;
    const FogGrid g = fogGrid(P[0], P[1].x);
    Texture2D<float> depthTexture = ResourceDescriptorHeap[P[4].y];
    const float device = depthTexture[id.xy];
    const bool sky = device <= 0;
    const float amount = sky ? asfloat(P[4].x) : 1.0;
    if (!(amount > 0)) return;
    const float depth = sky ? 3.0e38 : linearDepth(device);
    Texture3D<float4> integrated = ResourceDescriptorHeap[P[1].y];
    const float2 uv = (float2(id.xy) + 0.5) / float2(g_viewWidth, g_viewHeight);
    const float4 inside = fogVolumeAt(integrated, g, uv, depth);
    float3 L = inside.rgb;
    float T = inside.a;
    if (depth > g.farM)
    {
        const FogMedium fog = fogMedium(uint4(1, 0, 0, 0), P[2], P[3]);
        const float3 ray = froxelRayAt(float2(id.xy) + 0.5);
        const float toRay = length(ray);
        const float tau = fogOpticalDepth(fog, g_cameraPosition, ray / toRay, g.farM * toRay, sky ? 1.0e5 : depth * toRay);
        if (tau > 0)
        {
            Texture2D<float4> farSource = ResourceDescriptorHeap[P[1].z];
            const float2 scale = float2(g_viewWidth, g_viewHeight) / float2(g.x * g.cellPx, g.y * g.cellPx);
            const float t = exp(-tau);
            L += T * farSource.SampleLevel(g_linearClamp, uv * scale, 0).rgb * (1 - t);
            T *= t;
        }
    }
    RWTexture2D<float4> colour = ResourceDescriptorHeap[P[1].w];
    const float4 c = colour[id.xy];
    colour[id.xy] = float4(lerp(c.rgb, c.rgb * T + L * g_exposure, amount), c.a);
}
