// unx-kernel: cs_6_6 main
// r.gi.sao (LumenShortRangeAO.hlsli): the horizon search, one thread per downsampled pixel. View space here: x right, y
// up, z forward (view depth); V = the unit vector from the surface to the camera. Per slice (an angle phi of the screen,
// stratified over the slices and turned per pixel and frame): the normal projected into the slice's plane, then on each
// side of the pixel 'steps' depth samples at radii ((i + u) / steps)^2 x radius (denser near the pixel) raise that
// side's horizon (the cosine of the sample's direction against V); a sample nearer to or farther from the camera than
// the foreground distance (a fraction of the pixel's depth) fades back to the unoccluded horizon. The slice's
// cosine-weighted visible arc and its bent direction are the closed forms of "Practical Real-Time Strategies for
// Accurate Indirect Occlusion" (Jimenez et al. 2016), algorithms 1 and 2; the slices' sum over the same sum for
// unoccluded horizons is the AO (at least 0.03).
// Depth samples come from V's depth pyramid, mip 0 (half resolution, the farthest of each 2 x 2) when P[1].w says so,
// else from the depth buffer.
// P[0] = { depth, G-buffer, 0, output UAV (R32_UINT: bent normal x AO, 11-11-10) }; sky = device depth 0
// P[1] = { downsampled width, height, factor, depth pyramid SRV (UNX_NONE: the depth buffer) }
// P[2] = { slices, steps per slice, radius (px, float), foreground reject fraction (float) }
// P[3] = { foreground reject power (float), 0, 0, 0 }
#include "Bindless.hlsli"
#include "Frame.hlsli"
#include "GBuffer.hlsli"
#include "Passes/GI/LumenShortRangeAO.hlsli"

float3 saoViewPosition(float2 pixelPos, float z)
{
    const float2 ndc = float2(pixelPos.x / g_viewWidth * 2 - 1, 1 - pixelPos.y / g_viewHeight * 2);
    const float vx = (ndc.x + g_proj[0][2] - g_proj[0][3]) / g_proj[0][0];
    const float vy = (ndc.y + g_proj[1][2] - g_proj[1][3]) / g_proj[1][1];
    return float3(vx * z, vy * z, z);
}

float saoDepthAt(int2 pixel)
{
    if (P[1].w != UNX_NONE)
    {
        Texture2D<float> pyramid = ResourceDescriptorHeap[P[1].w];
        return linearDepth(pyramid.Load(int3(pixel >> 1, 0)));
    }
    Texture2D<float> depthTex = ResourceDescriptorHeap[P[0].x];
    return linearDepth(depthTex[pixel]);
}

// Minimal rotation taking unit a to unit b, applied to v.
float3 saoRotate(float3 a, float3 b, float3 v)
{
    const float3 k = cross(a, b);
    const float c = dot(a, b);
    if (c < -0.9999) return v;
    return v * c + cross(k, v) + k * (dot(k, v) / (1 + c));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint2 ds = id.xy;
    if (any(ds >= P[1].xy)) return;
    RWTexture2D<uint> output = ResourceDescriptorHeap[P[0].w];
    const uint2 viewSize = uint2(g_viewWidth, g_viewHeight);
    const uint2 pixel = lumenAoFullPixel(ds, P[1].z, g_frameIndex, viewSize);
    Texture2D<float> depthTex = ResourceDescriptorHeap[P[0].x];
    if (!(depthTex[pixel] > 0))
    {
        output[ds] = lumenAoPack(float3(0, 0, 0));
        return;
    }
    Texture2D<uint2> gbuffer = ResourceDescriptorHeap[P[0].y];
    const float z = linearDepth(depthTex[pixel]);
    const float3 worldNormal = octDecode(gbuffer[pixel].x);
    const float3 ax = g_view[0].xyz, ay = g_view[1].xyz, az = g_view[2].xyz;
    const float3 position = saoViewPosition(float2(pixel) + 0.5, z);
    const float3 V = -normalize(position);
    const float3 N = float3(dot(ax, worldNormal), dot(ay, worldNormal), -dot(az, worldNormal));
    const float invForeground = 1 / (z * asfloat(P[2].w));
    const float power = asfloat(P[3].x);
    const uint slices = max(P[2].x, 1u), steps = max(P[2].y, 1u);
    const float radius = asfloat(P[2].z);
    const float2 u = float2(lumenAoNoise(ds, g_frameIndex, 0), lumenAoNoise(ds, g_frameIndex, 1));

    float visibility = 0, correction = 0;
    float3 bent = 0;
    for (uint s = 0; s < slices; ++s)
    {
        const float phi = (s + u.x) / slices * 3.14159265;
        const float3 dir = float3(cos(phi), sin(phi), 0);
        const float3 ortho = dir - dot(dir, V) * V;
        const float3 axis = normalize(cross(dir, V));
        const float3 projected = N - axis * dot(N, axis);
        const float sgn = dot(ortho, projected) < 0 ? -1.0 : 1.0;
        const float projectedLength = max(length(projected), 1e-6);
        const float cosN = saturate(dot(projected, V) / projectedLength);
        const float angleN = sgn * acos(cosN), sinN = sgn * sqrt(1 - cosN * cosN);
        correction += projectedLength * (angleN * sinN + cosN);
        const float low0 = -sinN, low1 = sinN;  // the cosines of the unoccluded horizons on the two sides
        float horizon0 = low0, horizon1 = low1;
        for (uint k = 0; k < steps; ++k)
        {
            const float fraction = (k + u.y) / steps;
            const float2 offset = (fraction * fraction * radius + 1.0) * float2(dir.x, -dir.y);  // (pixel y runs down)
            for (uint side = 0; side < 2; ++side)
            {
                const float2 at = float2(pixel) + 0.5 + (side == 0 ? offset : -offset);
                if (any(at < 0) || any(at >= float2(viewSize))) continue;
                const float3 delta = saoViewPosition(floor(at) + 0.5, saoDepthAt(int2(at))) - position;
                const float distance = max(length(delta), 1e-6);
                const float low = side == 0 ? low0 : low1;
                const float c = lerp(dot(delta, V) / distance, low, saturate(pow(abs(delta.z) * invForeground, power)));
                if (side == 0) horizon0 = max(horizon0, c);
                else horizon1 = max(horizon1, c);
            }
        }
        const float h0 = -acos(clamp(horizon1, -1.0, 1.0)), h1 = acos(clamp(horizon0, -1.0, 1.0));
        const float arc0 = (cosN + 2 * h0 * sinN - cos(2 * h0 - angleN)) / 4;
        const float arc1 = (cosN + 2 * h1 * sinN - cos(2 * h1 - angleN)) / 4;
        visibility += projectedLength * (arc0 + arc1);
        const float t0 = (6 * sin(h0 - angleN) - sin(3 * h0 - angleN) + 6 * sin(h1 - angleN) - sin(3 * h1 - angleN) + 16 * sinN -
                          3 * (sin(h0 + angleN) + sin(h1 + angleN))) / 12;
        const float t1 = (-cos(3 * h0 - angleN) - cos(3 * h1 - angleN) + 8 * cos(angleN) - 3 * (cos(h0 + angleN) + cos(h1 + angleN))) / 12;
        bent += saoRotate(float3(0, 0, -1), V, float3(dir.x * t0, dir.y * t0, -t1)) * projectedLength;
    }
    const float ao = clamp(visibility / max(correction, 1e-6), 0.03, 1.0);
    const float bentLength = length(bent);
    const float3 bentView = bentLength > 1e-6 ? bent / bentLength : N;
    const float3 bentWorld = normalize(ax * bentView.x + ay * bentView.y - az * bentView.z);
    output[ds] = lumenAoPack(bentWorld * ao);
}
