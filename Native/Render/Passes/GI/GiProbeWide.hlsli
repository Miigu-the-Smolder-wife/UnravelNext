// The wide layer of L_gi (redesign V2 1.2, P2; gi.screen_wide_filter): the screen probes' irradiance SH after
// r.gi.probe.filter (GiProbeFilter.hlsl), as a record source of ScreenProbes.hlsli's templates (giLoadProbeRecord,
// giFootprintIrradiance: the same four probes and weights as M's gather, the filtered SH in place of the probe's own).
// Texture: RGBA32_UINT, (probesX * 4) x probesY; plane k = 1..4 of probe (i, j) at texel (i + (k - 1) probesX, j), the
// words of ScreenProbes.hlsli's planes 1-4 (27 fp16 SH coefficients x GI_STORE_SCALE, unorm16 occlusion in word 13's
// high half: the probe's own, copied), then plane 4's .z = the probe's relative standard deviation (float bits; the
// lookup's sigma at the probe's surface point, GiCache.hlsli) and .w = 0.
// Include before ScreenProbes.hlsli (its templates call giProbePlane on the source).
#ifndef UNX_GI_PROBEWIDE_HLSLI
#define UNX_GI_PROBEWIDE_HLSLI

struct GiProbeWide
{
    uint srv;
};
uint4 giProbePlane(GiProbeWide s, uint2 probe, uint plane, int2 count)
{
    Texture2D<uint4> t = ResourceDescriptorHeap[s.srv];
    return t.Load(int3(probe.x + (plane - 1) * count.x, probe.y, 0));
}
#endif
