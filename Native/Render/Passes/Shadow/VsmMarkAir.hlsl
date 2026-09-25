// unx-kernel: cs_6_6 main
// Page requests of the air (froxel integration, VsmAir.hlsli): every froxel's tile-centre segment requests, at its air
// level, the pages of the square its classification reads. One group per tile, one thread per depth slice; a thread
// skips the pages the previous slice already requested. Written with InterlockedOr of VSM_REQ_PROPAGATED (the pixel
// marks' VSM_REQ_PIXEL stay; air pages need no neighbourhood propagation).
// P[0].x requests UAV (raw), P[0].y VSM constants CBV, P[0].z gridX | gridY << 16, P[0].w slices | tilePx << 16
// P[1].x nearM (float bits), P[1].y farM (float bits), P[1].z shadow texels per tile (float bits). Frame constants of the
// main view.
#include "Bindless.hlsli"
#include "Frame.hlsli"
#include "Passes/Atmosphere/FroxelCommon.hlsli"
#include "Passes/Shadow/VsmAir.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 gid : SV_GroupID, uint s : SV_GroupIndex)
{
    FroxelGrid g = (FroxelGrid)0;
    g.gridX = P[0].z & 0xFFFFu;
    g.gridY = P[0].z >> 16;
    g.slices = P[0].w & 0xFFFFu;
    g.tilePx = P[0].w >> 16;
    g.nearM = asfloat(P[1].x);
    g.farM = asfloat(P[1].y);
    g.logRatio = log2(g.farM / g.nearM);
    if (s >= g.slices) return;
    ConstantBuffer<VsmConstants> c = ResourceDescriptorHeap[P[0].y];
    RWByteAddressBuffer requests = ResourceDescriptorHeap[P[0].x];
    const float3 ray = froxelTileRay(g, gid.xy);
    // Pages of this slice and of the previous one (the same rule), to skip repeats.
    int4 previous = int4(1, 1, 0, 0);  // empty box
    uint previousLevel = 0xFFFFFFFFu;
    [unroll] for (int j = 1; j >= 0; --j)
    {
        const int n = (int)s - j;
        if (n < 0) continue;
        const float z0 = froxelNodeDepth(g, (uint)n), z1 = froxelNodeDepth(g, (uint)n + 1);
        const uint k = vsmAirLevel(c, froxelTileWidth(g, 0.5 * (z0 + z1)), asfloat(P[1].z));
        const float3 p0 = vsmLightSpace(c, g_cameraPosition + ray * z0), p1 = vsmLightSpace(c, g_cameraPosition + ray * z1);
        float2 centre;
        float radius;
        vsmAirSquare(p0, p1, centre, radius);
        const int2 lo = vsmAbsPage(vsmAbsTexel(c, centre - radius, k)), hi = vsmAbsPage(vsmAbsTexel(c, centre + radius, k));
        if (j == 1)
        {
            previous = int4(lo, hi);
            previousLevel = k;
            continue;
        }
        // A few pages at the air level (the square is shorter than a page or two); clamp against pathological input.
        const int2 top = min(hi, lo + 3);
        for (int y = lo.y; y <= top.y; ++y)
            for (int x = lo.x; x <= top.x; ++x)
            {
                const int2 page = int2(x, y);
                if (k == previousLevel && all(page >= previous.xy) && all(page <= previous.zw)) continue;
                if (!vsmInWindow(c, page, k)) continue;
                requests.InterlockedOr(vsmSlot(page, k) * 4, VSM_REQ_PROPAGATED);
            }
    }
}
