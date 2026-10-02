// Receiver of the shadow visibility passes (S internal): world position and geometric normal of a pixel.
// The receiver plane comes from the depth buffer (the geometric surface; a normal-mapped shading normal would tilt the
// plane per pixel and self-shadow bumpy surfaces): on each screen axis the tangent goes to the neighbour whose linear
// depth is closer, so silhouettes take the receiver's own side. Where both neighbours of an axis jump by more than
// jumpFraction of the depth (isolated thin geometry), the G-buffer normal is used instead (8 B read only there).
#ifndef UNX_SHADOW_RECEIVER_HLSLI
#define UNX_SHADOW_RECEIVER_HLSLI
#include "Frame.hlsli"
#include "GBuffer.hlsli"

#define SHADOW_RECEIVER_JUMP 0.05

bool shadowNeighbour(Texture2D<float> depth, int2 q, float z0, out float3 p, out float dz)
{
    p = 0;
    dz = 3.0e38;
    if (any(q < 0) || q.x >= (int)g_viewWidth || q.y >= (int)g_viewHeight) return false;
    const float d = depth.Load(int3(q, 0));
    if (d <= 0) return false;
    p = worldFromDepth(float2(q), d);
    dz = abs(linearDepth(d) - z0);
    return true;
}

// Returns the world position; 'normal' faces the camera.
float3 shadowReceiver(Texture2D<float> depth, uint gbufferSrv, uint2 px, float d0, out float3 normal)
{
    const float3 p0 = worldFromDepth(float2(px), d0);
    const float z0 = linearDepth(d0);
    float3 pl, pr, pu, pd;
    float dl, dr, du, dd;
    const bool hl = shadowNeighbour(depth, int2(px) + int2(-1, 0), z0, pl, dl);
    const bool hr = shadowNeighbour(depth, int2(px) + int2(1, 0), z0, pr, dr);
    const bool hu = shadowNeighbour(depth, int2(px) + int2(0, -1), z0, pu, du);
    const bool hd = shadowNeighbour(depth, int2(px) + int2(0, 1), z0, pd, dd);
    const float limit = SHADOW_RECEIVER_JUMP * z0;
    const bool okx = (hl || hr) && min(dl, dr) <= limit, oky = (hu || hd) && min(du, dd) <= limit;
    float3 n;
    if (okx && oky)
    {
        const float3 tx = dr <= dl ? pr - p0 : p0 - pl;
        const float3 ty = dd <= du ? pd - p0 : p0 - pu;
        n = normalize(cross(tx, ty));
    }
    else
    {
        Texture2D<uint2> gbuffer = ResourceDescriptorHeap[gbufferSrv];
        n = decodeGBuffer(gbuffer.Load(int3(px, 0))).normal;
    }
    normal = dot(n, g_cameraPosition - p0) < 0 ? -n : n;
    return p0;
}

// scene::InstanceNoSelfShadow (ShadowSelfSlack.hlsl): how far toward the sun a pixel's sun lookup starts - past its own
// instance's bounds, so the instance's own casters lie behind the lookup; 0 for every other pixel, and without the
// texture (0xFFFFFFFF: no such instance in the scene).
float shadowSelfSlack(uint slackSrv, uint2 px)
{
    if (slackSrv == 0xFFFFFFFFu) return 0;
    Texture2D<float> slack = ResourceDescriptorHeap[slackSrv];
    return slack.Load(int3(px, 0));
}

// Screen-space contact shadow of the sun (shadow.vsm.screen_ray_length / screen_ray_steps; the reference's
// r.Shadow.Virtual.ScreenRayLength with its ShadowRayCast): a ray of length x the pixel's view depth toward the sun,
// walked across the depth buffer in 'steps' samples. The shadow map cannot say what happens within its receiver
// tolerance (a texel of the pixel's level, plus the visible surface's LOD error), so the last centimetres under a caster
// stay lit and objects float; the depth buffer has that detail. A sample is a hit when the ray is behind the depth
// buffer's surface there by less than the depth window of two steps (a thicker gap is a surface in front the ray passes
// behind: unknown, left to the shadow map). The step phase is a fixed function of the pixel (no frame term: the first
// frame after a cut has no noise to converge; the upscaler's jitter moves the pattern across the surface).
// packed = steps | round(length x 2^20) << 8 (0: off). Returns 0 (occluded) or 1.
#define SHADOW_CONTACT_LENGTH_SCALE (1.0 / 1048576.0)
#define SHADOW_CONTACT_MAX_STEPS 16u  // structural bound of the walk
float shadowSunContact(Texture2D<float> depth, uint2 px, float3 world, float3 normal, uint packed)
{
    const uint steps = min(packed & 0xFFu, SHADOW_CONTACT_MAX_STEPS);
    if (steps == 0) return 1;
    const float3 L = normalize(g_sunDirection);
    if (dot(normal, L) <= 0) return 1;  // facing away: unlit, or lit through (transmission asks the shadow map alone)
    const float4 c0 = mul(g_viewProj, float4(world, 1));
    if (c0.w <= 1e-4) return 1;
    const float rayLength = (packed >> 8) * SHADOW_CONTACT_LENGTH_SCALE * c0.w;
    float4 c1 = mul(g_viewProj, float4(world + L * rayLength, 1));
    // a ray toward the camera plane ends in front of it
    if (c1.w < 0.05 * c0.w) c1 = lerp(c0, c1, (0.95 * c0.w) / max(c0.w - c1.w, 1e-6));
    const float2 size = float2(g_viewWidth, g_viewHeight);
    // the depth window of a hit: two steps of a ray of this length along the view axis (the reference's CompareTolerance x 2)
    const float window = 4.0 * rayLength / steps;
    // interleaved gradient noise of the pixel, in [0, 1)
    const float phase = frac(52.9829189 * frac(dot(float2(px) + 0.5, float2(0.06711056, 0.00583715))));
    [loop] for (uint i = 0; i < steps; ++i)
    {
        const float t = (i + 0.5 + phase) / steps;
        const float4 c = lerp(c0, c1, t);
        const float2 at = float2(c.x / c.w * 0.5 + 0.5, 0.5 - c.y / c.w * 0.5) * size;
        if (any(at < 0) || any(at >= size)) return 1;  // left the screen: the shadow map's answer stands
        const int2 q = int2(floor(at));
        if (all(q == int2(px))) continue;  // the ray's own surface
        const float d = depth.Load(int3(q, 0));
        if (d <= 0) continue;  // sky
        const float behind = c.w - linearDepth(d);
        if (behind > 0 && behind < window) return 0;
    }
    return 1;
}

#endif
