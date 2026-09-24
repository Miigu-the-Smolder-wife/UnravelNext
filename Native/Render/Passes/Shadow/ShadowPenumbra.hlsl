// unx-kernel: cs_6_6 main
// unx-variants: PATHS=0,1
// Shadow visibility, pass 2 of 2: the pixels pass 1 left mixed (compacted list, indirect dispatch, full waves) run the
// blocker search and the penumbra filter (vsmSunPenumbra).
// PATHS=1 (diagnostics): writes each pixel's VSM_PATH_* instead of the visibility.
// P[0].x depth SRV, P[0].y G-buffer SRV, P[0].z output UAV (R32_UINT), P[0].w VSM constants CBV
// P[1].x unused, P[1].y page table SRV (raw), P[1].z pool SRV, P[1].w search bound SRV (raw)
// P[2].x penumbra list SRV (raw), P[2].y blocks SRV (raw), P[2].z statistics UAV (raw), P[2].w -
// P[3].x blocker search taps, P[3].y penumbra filter taps. Frame constants of the view.
#include "Frame.hlsli"
#include "Passes/Shadow/ShadowReceiver.hlsli"
#include "Passes/Shadow/VsmSample.hlsli"

[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    ByteAddressBuffer list = ResourceDescriptorHeap[P[2].x];
    const bool valid = i < list.Load(0);
    uint path = 0xFFu;
    if (valid)
    {
        const uint word = list.Load(4 + i * 4);
        const uint2 px = uint2(word & 0xFFFFu, word >> 16);
        Texture2D<float> depthTex = ResourceDescriptorHeap[P[0].x];
        RWTexture2D<uint> output = ResourceDescriptorHeap[P[0].z];
        VsmResources r;
        r.table = ResourceDescriptorHeap[P[1].y];
        r.pool = ResourceDescriptorHeap[P[1].z];
        r.searchBound = ResourceDescriptorHeap[P[1].w];
        r.blocks = ResourceDescriptorHeap[P[2].y];
        r.cbv = P[0].w;
        ConstantBuffer<VsmConstants> vc = ResourceDescriptorHeap[P[0].w];
        const float depth = depthTex.Load(int3(px, 0));
        float3 normal;
        const float3 world = shadowReceiver(depthTex, P[0].y, px, depth, normal);
        const float footprint = 2 * linearDepth(depth) * g_tanHalfFovY / g_viewHeight;
        const VsmReceiver rc = vsmMakeReceiver(vc, world, normal);
        const float tanSun = tan(g_sunAngularRadius);
        const uint k = vsmLevelForFootprint(vc, footprint);
        const float reach = (vsmSearchHeight(r, rc.uv, k) - rc.h) * tanSun;
        const float sun = vsmSunPenumbra(r, rc, k, reach, tanSun, P[3].x, P[3].y, path);
#if PATHS
        output[px] = path;
#else
        output[px] = (uint)round(saturate(sun) * 255.0) | 0xFFFFFF00u;
#endif
    }
    RWByteAddressBuffer stats = ResourceDescriptorHeap[P[2].z];
    [unroll] for (uint p = 3; p < 7; ++p)
    {
        const uint c = WaveActiveCountBits(path == p);
        if (WaveIsFirstLane() && c) stats.InterlockedAdd(32 + p * 4, c);
    }
}
