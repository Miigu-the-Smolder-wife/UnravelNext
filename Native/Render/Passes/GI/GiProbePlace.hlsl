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
    uint2 pixel;
    float d;
    if (!giProbePixel(depth, probe, P[0].w, P[1].zw, pixel, d)) return;
    const float3 p = worldFromDepth(float2(pixel), d);
    const float3 n = decodeGBuffer(gbuffer.Load(int3(pixel, 0))).normal;
    bool created;
    const uint own = giFindOrCreate(b, h, giSurfaceKey(h, p, n, 0), giAnchorAtHit(h, p, normalize(p - g_cameraPosition), distance(p, g_cameraPosition)), n, created);
    if (own != GI_ENTRY_PENDING)
    {
        giTouch(b, h, own);
        giRequestUpdate(b, h, own, 0);
    }
    // The other cells of the trilinear footprint (same level and normal class) that exist; in the level band
    // (GiCache.hlsli giLevelBand) also the next coarser level's cell here (created) and footprint, which the pixels blend in.
    const uint nc = giNormalClass(n);
    uint level;
    const float beta = giLevelBand(h, p, level);
    [loop] for (uint l = level; l <= level + (beta > 0 ? 1u : 0u); ++l)
    {
        uint centre = own;
        if (l != level)
        {
            centre = giFindOrCreate(b, h, giSurfaceKey(h, p, n, l), giAnchorAtHit(h, p, normalize(p - g_cameraPosition), distance(p, g_cameraPosition)), n, created);
            if (centre != GI_ENTRY_PENDING)
            {
                giTouch(b, h, centre);
                giRequestUpdate(b, h, centre, 0);
            }
        }
        const float3 f = p / giCellSize(h, l) - 0.5;
        const int3 c0 = int3(floor(f));
        [loop] for (uint k = 0; k < 8; ++k)
        {
            const uint e = giFind(b, h, giKey(l, nc, c0 + int3(k & 1, (k >> 1) & 1, k >> 2)));
            if (e == GI_ENTRY_PENDING || e == centre) continue;
            giTouch(b, h, e);
            giRequestUpdate(b, h, e, 0);
        }
    }
}
