// unx-kernel: cs_6_6 main
// unx-variants: PATHS=0,1
// Shadow visibility, pass 1 of 2 (ARCHITECTURE 2.11: small kernels separate from shading, so the page-table and pool
// dependent loads are hidden by occupancy). Writes 4 B per pixel (INTERFACES 7.3): slot 0 = sun, slots 1-3 = the first
// three shadowed local lights of the pixel's froxel list (255 until local-light VSM lands). Settles every pixel whose
// sun visibility the page structures decide exactly (vsmSunClassify: no caster within reach, the block hierarchy over
// the reach square all below or all above the receiver's plane) and compacts the rest into a list for pass 2
// (ShadowPenumbra.hlsl, indirect).
// PATHS=1 (diagnostics): writes each pixel's VSM_PATH_* (0xFF = sky) instead of the visibility.
// P[0].x depth SRV, P[0].y G-buffer SRV (RG32_UINT), P[0].z output UAV (R32_UINT), P[0].w VSM constants CBV
// P[1].x unused, P[1].y page table SRV (raw), P[1].z pool SRV (raw), P[1].w search bound SRV (raw)
// P[2].x penumbra list UAV (raw: count, then pixel y << 16 | x), P[2].y blocks SRV (raw), P[2].z statistics UAV (raw,
// words 8.. of the VSM stats: pixels per VSM_PATH_*). Frame constants of the view.
#include "Frame.hlsli"
#include "Passes/Shadow/ShadowReceiver.hlsli"
#include "Passes/Shadow/VsmSample.hlsli"

// One pixel's classification: sky, settled (packed visibility), or mixed (goes to pass 2).
void classifyPixel(uint2 px, out uint packed, out uint path, out bool mixed)
{
    packed = 0xFFFFFFFFu;
    path = 0xFFu;  // sky
    mixed = false;
    if (px.x >= g_viewWidth || px.y >= g_viewHeight) return;
    Texture2D<float> depthTex = ResourceDescriptorHeap[P[0].x];
    const float depth = depthTex.Load(int3(px, 0));
    if (depth <= 0) return;
    VsmResources r;
    r.table = ResourceDescriptorHeap[P[1].y];
    r.pool = ResourceDescriptorHeap[P[1].z];
    r.searchBound = ResourceDescriptorHeap[P[1].w];
    r.blocks = ResourceDescriptorHeap[P[2].y];
    r.cbv = P[0].w;
    ConstantBuffer<VsmConstants> vc = ResourceDescriptorHeap[P[0].w];
    float3 normal;
    const float3 world = shadowReceiver(depthTex, P[0].y, px, depth, normal);
    const float footprint = 2 * linearDepth(depth) * g_tanHalfFovY / g_viewHeight;
    const VsmReceiver rc = vsmMakeReceiver(vc, world, normal);
    uint k;
    float reach;
    const uint cls = vsmSunClassify(r, rc, footprint, tan(g_sunAngularRadius), k, reach, path);
    mixed = cls == VSM_REGION_MIXED;
    packed = (cls == VSM_REGION_UMBRA ? 0u : 255u) | 0xFFFFFF00u;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    RWTexture2D<uint> output = ResourceDescriptorHeap[P[0].z];
    RWByteAddressBuffer list = ResourceDescriptorHeap[P[2].x];
    RWByteAddressBuffer stats = ResourceDescriptorHeap[P[2].z];
    uint packed[1], path[1];
    bool mixed[1];
    classifyPixel(id.xy, packed[0], path[0], mixed[0]);
    {
        const uint j2 = 0;
        const uint2 px = id.xy;
        // Compact the mixed pixels (one atomic per wave).
        const uint n = WaveActiveCountBits(mixed[j2]);
        uint base = 0;
        if (WaveIsFirstLane() && n) list.InterlockedAdd(0, n, base);
        base = WaveReadLaneFirst(base);
        if (mixed[j2]) list.Store(4 + (base + WavePrefixCountBits(mixed[j2])) * 4, (px.y << 16) | px.x);
        if (px.x < g_viewWidth && px.y < g_viewHeight && !mixed[j2])
        {
#if PATHS
            output[px] = path[j2];
#else
            output[px] = packed[j2];
#endif
        }
        [unroll] for (uint i = 0; i < 3; ++i)
        {
            const uint c = WaveActiveCountBits(path[j2] == i);
            if (WaveIsFirstLane() && c) stats.InterlockedAdd(32 + i * 4, c);
        }
    }
}
