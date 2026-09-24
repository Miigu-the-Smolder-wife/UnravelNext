// unx-kernel: cs_6_6 main
// Screen probe placement: each probe's surface point finds or creates its cache entry, and the entries that the probe's
// trilinear gather will read are kept alive and queued for this frame's GI rays (screen-used entries first).
// P[0] = { cache UAV, depth SRV, gbuffer SRV, spacing }, P[1] = { probesX, probesY, width, height }; frame constants b1.
#include "GBuffer.hlsli"
#include "Passes/GI/GiInternal.hlsli"

[numthreads(8, 8, 1)]
void main(uint2 probe : SV_DispatchThreadID)
{
    if (probe.x >= P[1].x || probe.y >= P[1].y) return;
    RWByteAddressBuffer b = ResourceDescriptorHeap[P[0].x];
    Texture2D<float> depth = ResourceDescriptorHeap[P[0].y];
    Texture2D<uint2> gbuffer = ResourceDescriptorHeap[P[0].z];
    const GiHeader h = giHeader(b);
    uint2 offset;
    float d;
    if (!giProbePixel(depth, probe, P[0].w, P[1].zw, offset, d)) return;
    const uint2 pixel = probe * P[0].w + offset;
    const float3 p = worldFromDepth(float2(pixel), d);
    const float3 n = decodeGBuffer(gbuffer.Load(int3(pixel, 0))).normal;
    bool created;
    const uint own = giFindOrCreate(b, h, giSurfaceKey(h, p, n, 0), p, n, created);
    if (own != GI_ENTRY_PENDING)
    {
        giTouch(b, h, own);
        giRequestUpdate(b, h, own);
    }
    // The other cells of the trilinear footprint (same level and normal class) that exist.
    const uint nc = giNormalClass(n);
    const uint level = giLevel(h, p);
    const float3 f = p / giCellSize(h, level) - 0.5;
    const int3 c0 = int3(floor(f));
    [loop] for (uint k = 0; k < 8; ++k)
    {
        const uint e = giFind(b, h, giKey(level, nc, c0 + int3(k & 1, (k >> 1) & 1, k >> 2)));
        if (e == GI_ENTRY_PENDING || e == own) continue;
        giTouch(b, h, e);
        giRequestUpdate(b, h, e);
    }
}
