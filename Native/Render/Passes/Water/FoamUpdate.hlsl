// unx-kernel: cs_6_6 main
// Water foam F (Foam.hlsli), one level's update: per texel of the window, F = max(F exp(-elapsed / tau), C) with the
// breaking fraction C = Phi((J_t - Jbar) / sigma_J) (Jbar from OceanSample.hlsli at the texel's rest centre, low-passed to
// the texel; sigma_J^2 from FoamVariance). A texel that entered the window since the level's last update starts from
// the next coarser level's foam there (coarser levels update first), the coarsest from zero.
// P[0] foam UAV (Texture2DArray R16F, one slice per level), parameter SRV (raw), accumulator SRV (raw, uint64 per
// level), level; P[1] displacement SRV, slopes SRV; P[2] cascade lengths
#include "Foam.hlsli"
#include "OceanSample.hlsli"

float foamCoarser(RWTexture2DArray<float> foam, FoamParams p, uint level, float2 x0)
{
    const FoamLevel l = foamLevel(p, level);
    const float2 u = x0 / foamSpacing(p, level) - 0.5;
    const int2 i0 = int2(floor(u));
    const float2 f = u - float2(i0);
    if (any(i0 < l.origin) || any(i0 + 1 >= l.origin + FOAM_N)) return 0;
    const float a = foam[uint3(foamStorage(i0), level)], b = foam[uint3(foamStorage(i0 + int2(1, 0)), level)];
    const float c = foam[uint3(foamStorage(i0 + int2(0, 1)), level)], d = foam[uint3(foamStorage(i0 + 1), level)];
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    if (any(id >= FOAM_N)) return;
    const FoamParams p = foamParams(P[0].y);
    const uint level = P[0].w;
    const FoamLevel l = foamLevel(p, level);
    const float s = foamSpacing(p, level);
    const int2 texel = l.origin + int2(id);
    const float2 x0 = (float2(texel) + 0.5) * s;
    RWTexture2DArray<float> foam = ResourceDescriptorHeap[P[0].x];
    const uint3 at = uint3(foamStorage(texel), level);
    const bool entered = any(texel < l.previous) || any(texel >= l.previous + FOAM_N);
    const float previous = entered ? (level + 1 < p.levels ? foamCoarser(foam, p, level + 1, x0) : 0.0) : foam[at] * exp(-l.elapsed / p.tau);
    const OceanPoint o = oceanSample(P[1].x, P[1].y, asfloat(P[2].xyz), x0, s);
    const float jacobian = (1 + o.dDdx.x) * (1 + o.dDdz.z) - o.dDdz.x * o.dDdx.z;
    ByteAddressBuffer accumulator = ResourceDescriptorHeap[P[0].z];
    const float sigma = sqrt(float(accumulator.Load<uint64_t>(8 * level)) * (1.0 / 1099511627776.0));
    const float breaking = sigma > 0 ? foamPhi((p.threshold - jacobian) / sigma) : (jacobian < p.threshold ? 1.0 : 0.0);
    foam[at] = max(previous, breaking);
}
