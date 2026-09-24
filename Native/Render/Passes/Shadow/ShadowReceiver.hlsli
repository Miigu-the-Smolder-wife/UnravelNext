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

#endif
