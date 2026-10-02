// unx-kernel: cs_6_6 main
// m.ml.compact (shading.mega_lights_compact_traces; the reference's CompactLightSampleTraces): the light samples that ask
// for a ray, as a list (RayTracing/CompactDispatch.hlsl), so that m.ml.trace launches a thread per sample it traces. Over
// the whole sample texture its threads of the samples that ask for none - no light, a light that casts no shadow, a
// sample merged into its neighbour's ray, a downsampled pixel without a surface - return at once and leave their waves
// partly idle.
// Thread = sample texel. A sample that needs a ray (MegaLightsTrace.hlsl's own test) is appended as x | y << 16. One
// atomic a wave.
// P[0] = { samples SRV (R32G32_UINT), downsampled key SRV, list UAV (raw: word 0 the count, entries from byte 16),
//          capacity (entries) }, P[1] = { sample texture width, height, factor | N << 8, 0 }
#include "Passes/Shading/MegaLights.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint2 texel = id.xy;
    bool need = false;
    if (all(texel < P[1].xy))
    {
        Texture2D<uint2> samples = ResourceDescriptorHeap[P[0].x];
        const MlSample s = mlUnpack(samples[texel]);
        if (s.needsRay && s.light != ML_LIGHT_NONE)
        {
            Texture2D<uint2> keys = ResourceDescriptorHeap[P[0].y];
            need = asfloat(keys[texel / mlSampleGrid((P[1].z >> 8) & 0xFFu)].x) > 0;
        }
    }
    const uint count = WaveActiveCountBits(need);
    uint base = 0;
    RWByteAddressBuffer list = ResourceDescriptorHeap[P[0].z];
    if (WaveIsFirstLane() && count > 0) list.InterlockedAdd(0, count, base);
    base = WaveReadLaneFirst(base);
    const uint slot = base + WavePrefixCountBits(need);
    if (need && slot < P[0].w) list.Store(16 + 4 * slot, texel.x | (texel.y << 16));
}
