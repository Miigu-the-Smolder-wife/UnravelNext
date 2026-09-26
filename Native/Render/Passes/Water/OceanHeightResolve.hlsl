// unx-kernel: cs_6_6 main
// Water height clipmap, step 2 of the build (OceanHeight.hlsli, FEATURES_GAME 1.8): one thread per lattice point
// (x, z, level) turns the scatter's winning key (OceanHeightScatter.hlsl: the upper sheet's mesh height and rest
// position x0) into the stored texel, polished on the continuous field: Newton steps on F(x0) = x0 + D(x0) - w with the
// field's Jacobian I + dD/dx0 from the mesh's x0 (OH_ITERATIONS). The polished solution is kept when its residual is
// within 0.02 s_l and it stays within 1.5 s_l of the mesh's x0 (the same sheet); otherwise the mesh value is kept (a
// fold edge, where the sheet's Jacobian vanishes). A point no triangle covered (only if the displacement exceeded R or a
// triangle's loop bound) is solved by Newton from w - D(w) and counted when that fails.
// Flags (texel .w, a float value): 0 polished, 1 mesh value, 2 uncovered and solved, 3 uncovered and unsolved.
// Root constants: P[0] displacement SRV, height UAV, key UAV (raw), levels; P[1], P[2] as OceanHeight.hlsli; P[3]
// counter UAV (raw: +0 = points flagged 3), 0, 0, slopes SRV.
#include "OceanHeight.hlsli"

struct Solve { float2 x0; float3 d; float residual; };
Solve solveFrom(float2 w, float2 start, float s)
{
    Solve r;
    r.x0 = start;
    [unroll] for (int i = 0; i < OH_ITERATIONS; ++i)
    {
        float3 d, j;
        ohDisplacementJacobian(r.x0, s, d, j);
        const float2 f = r.x0 + d.xz - w;
        const float a = 1 + j.x, b = j.y, c = 1 + j.z, det = a * c - b * b;
        r.x0 = det > 0.05 ? r.x0 - float2(c * f.x - b * f.y, a * f.y - b * f.x) / det : w - d.xz;
    }
    r.d = ohDisplacement(r.x0, s);
    r.residual = length(r.x0 + r.d.xz - w);
    return r;
}

float ohHeightFromOrdered(uint u) { return asfloat((u & 0x80000000u) ? (u & 0x7FFFFFFFu) : ~u); }

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint level = id.z;
    if (id.x >= OH_N || id.y >= OH_N || level >= P[0].w) return;
    const float s = ohLevelSpacing(level);
    const int2 origin = ohOrigin(level, asfloat(P[1].xy));
    const float2 w = float2(origin + int2(id.xy)) * s;
    const float tolerance = OH_RESIDUAL * s;
    RWByteAddressBuffer keys = ResourceDescriptorHeap[P[0].z];
    const uint2 key = keys.Load2(((level * OH_N + id.y) * OH_N + id.x) * 8);  // little-endian: .x low, .y high
    float4 texel;
    if (key.y != 0)
    {
        const float2 meshX0 = w + (float2(key.x >> 16, key.x & 0xFFFFu) - 32768.0) / 64.0 * s;
        const float meshH = ohHeightFromOrdered(key.y);
        const Solve polished = solveFrom(w, meshX0, s);
        if (polished.residual <= tolerance && length(polished.x0 - meshX0) <= 1.5 * s)
            texel = float4(asfloat(P[1].w) + polished.d.y, polished.x0 - w, 0);
        else
            texel = float4(meshH, meshX0 - w, 1);
    }
    else
    {
        const Solve fallback = solveFrom(w, w - ohDisplacement(w, s).xz, s);
        const bool solved = fallback.residual <= tolerance;
        texel = float4(asfloat(P[1].w) + fallback.d.y, fallback.x0 - w, solved ? 2 : 3);
        if (!solved)
        {
            RWByteAddressBuffer counter = ResourceDescriptorHeap[P[3].x];
            counter.InterlockedAdd(0, 1u);
        }
    }
    RWTexture2DArray<float4> height = ResourceDescriptorHeap[P[0].y];
    height[uint3(id.xy, level)] = texel;
    keys.Store2(((level * OH_N + id.y) * OH_N + id.x) * 8, uint2(0, 0));  // cleared for the next frame
}
