// unx-kernel: cs_6_6 main
// unx-variants: PATHS=0,1 STAGE=0,1
// Shadow visibility, pass 2: the pixels pass 1 left mixed (compacted list, indirect dispatch, full waves).
//   STAGE=0: the blocker search and the classification of the penumbra disk (vsmSunPenumbraSearch); the pixels they
//            settle (search lit, disk lit, disk umbra: most of the list) are written, the others go to a second list
//            (pixel, filter radius, levels, reach) for STAGE=1;
//   STAGE=1: the penumbra filter (vsmSunPenumbraFilter, 16 taps) over that list in full waves: the pixels that settled
//            early no longer wait in their waves for the filtered ones (the same functions and inputs as the single
//            pass: vsmSunPenumbra = search then filter; the receiver is rebuilt by the same shadowReceiver).
// PATHS=1 (diagnostics): STAGE=0 writes each pixel's VSM_PATH_* instead of the visibility (no second list).
// P[0].x depth SRV, P[0].y G-buffer SRV, P[0].z output UAV (R32_UINT), P[0].w VSM constants CBV
// P[1].x unused, P[1].y page table SRV (raw), P[1].z pool SRV, P[1].w search bound SRV (raw)
// P[2].x penumbra list SRV (raw; STAGE=1: the filter list), P[2].y blocks SRV (raw), P[2].z statistics UAV (raw),
// P[2].w filter list UAV (raw: count at 0, records of 16 B from 16: pixel y << 16 | x, radius, k | kf << 8, reach)
// P[3].w the transmittance LUT (0xFFFFFFFF: none): the sun slot also x the cloud layer's sun transmittance (B5).
// P[3].x blocker search taps, P[3].y penumbra filter taps, P[3].z transmittance layer SRV (raw; 0xFFFFFFFF: none):
// the result is multiplied by the thin casters' T over the penumbra's reach (v1.26). Frame constants of the view.
#include "Frame.hlsli"
#include "Passes/Shadow/ShadowReceiver.hlsli"
#include "Passes/Shadow/ShadowVisibility.hlsli"
#include "Passes/Atmosphere/CloudShadowCommon.hlsli"

// The sun slot of a pixel: visibility x the thin casters' and the cloud layer's transmittance.
void storeSun(uint2 px, float3 world, float footprint, float reach, float sun)
{
    if (P[3].z != 0xFFFFFFFFu && sun > 0)
    {
        ShadowSrvs ts = (ShadowSrvs)0;
        ts.pageTable = P[1].y;
        ts.pool = P[1].z;
        ts.blocks = P[2].y;
        ts.searchBound = P[1].w;
        ts.constants = P[0].w;
        ts.layers = P[3].z;
        sun *= shadowSunTransmittanceAt(ts, world, footprint, max(reach, footprint));
    }
    if (P[3].w != 0xFFFFFFFFu && sun > 0) sun *= cloudSunTransmittanceFromLut(P[3].w, world);  // B5 cloud shadow
    RWTexture2D<uint> output = ResourceDescriptorHeap[P[0].z];
    output[px] = (output[px] & 0xFFFFFF00u) | (uint)round(saturate(sun) * 255.0);  // local slots from pass 1
}

[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    ByteAddressBuffer list = ResourceDescriptorHeap[P[2].x];
    const bool valid = i < list.Load(0);
    uint path = 0xFFu;
    bool filter = false;
    uint4 record = 0;
    if (valid)
    {
#if STAGE == 0
        const uint word = list.Load(4 + i * 4);
#else
        record = list.Load4(16 + i * 16);
        const uint word = record.x;
#endif
        const uint2 px = uint2(word & 0xFFFFu, word >> 16);
        Texture2D<float> depthTex = ResourceDescriptorHeap[P[0].x];
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
        const float footprint = outputPixelFootprint(linearDepth(depth));
#if STAGE == 0
        const float tanSun = tan(g_sunAngularRadius);
        const uint k = vsmLevelForFootprint(vc, footprint);
        const VsmReceiver rc = vsmReceiverAt(vc, vsmMakeReceiver(vc, world, normal, k), k);
        const float reach = (vsmSearchHeight(r, rc, k) - rc.h) * tanSun;
        float radius;
        uint kf;
        const float sun = vsmSunPenumbraSearch(r, rc, k, reach, tanSun, P[3].x, P[3].y, radius, kf, path);
#if PATHS
        RWTexture2D<uint> output = ResourceDescriptorHeap[P[0].z];
        output[px] = path;
#else
        if (sun >= 0) storeSun(px, world, footprint, reach, sun);
        else
        {
            filter = true;
            record = uint4(word, asuint(radius), k | (kf << 8), asuint(reach));
        }
#endif
#else
        const uint k = record.z & 0xFFu, kf = record.z >> 8;
        const VsmReceiver rc = vsmReceiverAt(vc, vsmMakeReceiver(vc, world, normal, k), k);
        storeSun(px, world, footprint, asfloat(record.w), vsmSunPenumbraFilter(r, rc, asfloat(record.y), kf, P[3].y));
#endif
    }
#if STAGE == 0
    // The filtered pixels to the second list (one atomic per wave).
    const uint n = WaveActiveCountBits(filter);
    if (n)
    {
        RWByteAddressBuffer next = ResourceDescriptorHeap[P[2].w];
        uint base = 0;
        if (WaveIsFirstLane()) next.InterlockedAdd(0, n, base);
        base = WaveReadLaneFirst(base);
        if (filter) next.Store4(16 + (base + WavePrefixCountBits(filter)) * 16, record);
    }
    RWByteAddressBuffer stats = ResourceDescriptorHeap[P[2].z];
    [unroll] for (uint p = 3; p < 7; ++p)
    {
        const uint c = WaveActiveCountBits(path == p);
        if (WaveIsFirstLane() && c) stats.InterlockedAdd(32 + p * 4, c);
    }
#endif
}
