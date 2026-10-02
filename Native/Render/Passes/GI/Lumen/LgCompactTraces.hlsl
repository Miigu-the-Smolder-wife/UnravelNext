// unx-kernel: cs_6_6 main
// gi.lumen, r.gi.lg.compact (gi.lumen_compact_traces; the reference's CompactTraces before its hardware ray tracing):
// the trace texels that need a world ray, as a list (RayTracing/CompactDispatch.hlsl), so that r.gi.lg.trace launches a
// thread per ray it traces. Over the whole trace atlas its threads of dead slots (the adaptive rows not in use, probes
// on the sky) and of the texels the screen trace finished return at once and leave their waves partly idle.
// Thread = trace texel (a group: one probe's 8 x 8 map). A live probe's texel whose trace word holds no final screen hit
// - every live texel without screen traces - is appended as x | y << 16; a dead slot's texel takes radiance 0 and the
// empty trace word (what LgTrace's thread wrote there). One atomic a wave.
// P[0] = { trace word UAV (R32_UINT), trace radiance UAV, list UAV (raw: word 0 the count, entries from byte 16),
//          capacity (entries) }, P[1].x = flags (bit 0: LgScreenTrace ran before - the trace words hold its results),
// P[8..11] = the common block (P[10].z adaptive SRV, P[10].w probe depth SRV).
#include "Passes/GI/Lumen/LgCommon.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint2 coord = id.xy;
    const uint2 atlas = coord / LG_TRACE_RES;
    const uint probe = lgProbeIndex(atlas);
    ByteAddressBuffer adaptive = ResourceDescriptorHeap[P[10].z];
    Texture2D<float> probeDepth = ResourceDescriptorHeap[P[10].w];
    RWTexture2D<uint> traceWord = ResourceDescriptorHeap[P[0].x];
    const bool inAtlas = atlas.x < lgProbeViewSize().x && atlas.y < lgAtlasRows();
    const bool live = inAtlas && probe < lgProbeCount(adaptive) && probeDepth[atlas] > 0;
    bool need = live;
    if (live && (P[1].x & 1u) != 0) need = !lgTraceHit(traceWord[coord]);
    if (inAtlas && !live)
    {
        RWTexture2D<float4> traceRadiance = ResourceDescriptorHeap[P[0].y];
        traceRadiance[coord] = 0;
        traceWord[coord] = lgEncodeTrace(0, false, false, false);
    }
    const uint count = WaveActiveCountBits(need);
    uint base = 0;
    RWByteAddressBuffer list = ResourceDescriptorHeap[P[0].z];
    if (WaveIsFirstLane() && count > 0) list.InterlockedAdd(0, count, base);
    base = WaveReadLaneFirst(base);
    const uint slot = base + WavePrefixCountBits(need);
    if (need && slot < P[0].w) list.Store(16 + 4 * slot, coord.x | (coord.y << 16));
}
