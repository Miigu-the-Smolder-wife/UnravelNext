// unx-kernel: cs_6_6 main
// renderergate --frame-log (RENDERER_REDESIGN_V2 3.3 workload counter d): the share of the main view's surface pixels
// this frame that the previous frame did not see - disoccluded by the camera's motion. Per internal pixel with depth: its
// point (pixel centre, device depth) under this frame's inverse view-projection, projected with the previous one (M =
// prevViewProj x invViewProj, clip to clip; both carry their frame's jitter, as the depth buffers do); disoccluded when
// that lands outside the previous frame, behind its camera, or where the previous frame's depth at the nearest pixel
// differs by more than 2 % (reversed-Z device depth is ~ near / distance: a relative depth difference). Static scenes
// only (instance motion is not followed: the gates' interior scenes do not move). The frame's depth is copied into the
// gate's own texture for the next frame. Counts per frame slot (16 B): surface pixels, disoccluded, off-screen (a part of
// disoccluded), sky. The order of the atomics does not change the counts.
// P[0] = { depth SRV, previous depth UAV (0xFFFFFFFF: none, every surface pixel is new), depth copy UAV, counts UAV (raw) },
// P[1] = { width, height, slot, clear (1: one thread zeroes the slot) }, P[2..5] = rows of M (asfloat).
#include "Bindless.hlsli"

[numthreads(8, 8, 1)]
void main(uint2 px : SV_DispatchThreadID)
{
    RWByteAddressBuffer counts = ResourceDescriptorHeap[P[0].w];
    const uint slot = P[1].z * 16;
    if (P[1].w != 0)
    {
        if (all(px == 0)) counts.Store4(slot, 0u);
        return;
    }
    const uint2 size = P[1].xy;
    bool surface = false, disoccluded = false, offscreen = false, sky = false;
    if (all(px < size))
    {
        Texture2D<float> depth = ResourceDescriptorHeap[P[0].x];
        RWTexture2D<float> copy = ResourceDescriptorHeap[P[0].z];
        const float z = depth.Load(int3(px, 0));
        copy[px] = z;
        if (z <= 0)
            sky = true;
        else
        {
            surface = true;
            const float4 clip = float4((px.x + 0.5) / size.x * 2 - 1, 1 - (px.y + 0.5) / size.y * 2, z, 1);
            const float4 p = float4(dot(asfloat(P[2]), clip), dot(asfloat(P[3]), clip), dot(asfloat(P[4]), clip), dot(asfloat(P[5]), clip));
            if (P[0].y == 0xFFFFFFFFu || !(p.w > 1e-6))
                disoccluded = true;
            else
            {
                const float3 n = p.xyz / p.w;
                const float2 q = float2((n.x + 1) * 0.5 * size.x, (1 - n.y) * 0.5 * size.y);
                if (any(q < 0) || any(q >= float2(size)))
                {
                    disoccluded = true;
                    offscreen = true;
                }
                else
                {
                    RWTexture2D<float> prev = ResourceDescriptorHeap[P[0].y];
                    const float zp = prev[uint2(q)];
                    disoccluded = !(abs(zp - n.z) <= 0.02 * max(n.z, 1e-30));
                }
            }
        }
    }
    const uint s = WaveActiveCountBits(surface), d = WaveActiveCountBits(disoccluded), o = WaveActiveCountBits(offscreen), k = WaveActiveCountBits(sky);
    if (WaveIsFirstLane())
    {
        if (s) counts.InterlockedAdd(slot, s);
        if (d) counts.InterlockedAdd(slot + 4, d);
        if (o) counts.InterlockedAdd(slot + 8, o);
        if (k) counts.InterlockedAdd(slot + 12, k);
    }
}
