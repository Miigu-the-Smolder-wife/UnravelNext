// unx-kernel: cs_6_6 main
// gi.lumen, r.gi.lg.rays: the 64 ray directions of each probe by structured importance sampling. One group per probe,
// thread = direction texel of the 8 x 8 map. A texel's density = the BRDF density of the probe's pixels in its direction
// (LgScreenData.hlsl, SH) x the lighting density of last frame (LgLightingPdf.hlsl, x 64 so that uniform lighting is 1);
// a texel the BRDF does not cull (its BRDF density >= the minimum) keeps at least the minimum. The texels are sorted by
// density; while the 3 lowest remaining are under the minimum, they give their rays to the highest remaining one,
// which splits into the 4 texels of the next finer level (16 x 16) in its place: 64 rays, none toward directions no
// pixel needs, 4 where the density is highest. The trace's solid angle follows its level (LgComposite.hlsl weighs it).
// Output per trace: the texel and its level (lgPackRay; level 1 = 8 x 8, level 0 = 16 x 16).
// P[1] = { ray info UAV (R16_UINT, atlas x 8), BRDF SH SRV (raw, 40 B per probe), lighting density SRV (R16F, atlas x 8;
// 0xFFFFFFFF: BRDF alone), minimum density (float) }, P[10].z = adaptive SRV, P[10].w = probe depth SRV.
#include "Passes/GI/Lumen/LgCommon.hlsli"

groupshared uint2 gs_rays[128];
groupshared uint gs_subdivide;

uint2 lgSortInfo(uint2 texel, uint level, float density) { return uint2((texel.x & 0xFFu) | ((texel.y & 0xFFu) << 8) | ((level & 0xFFu) << 16), asuint(density)); }
void lgSortInfoUnpack(uint2 v, out uint2 texel, out uint level, out float density)
{
    texel = uint2(v.x & 0xFFu, (v.x >> 8) & 0xFFu);
    level = (v.x >> 16) & 0xFFu;
    density = asfloat(v.y);
}

[numthreads(8, 8, 1)]
void main(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
    const uint2 atlas = group.xy;
    const uint probe = lgProbeIndex(atlas);
    ByteAddressBuffer adaptive = ResourceDescriptorHeap[P[10].z];
    Texture2D<float> probeDepth = ResourceDescriptorHeap[P[10].w];
    if (!(atlas.x < lgProbeViewSize().x && probe < lgProbeCount(adaptive) && probeDepth[atlas] > 0)) return;
    const float minimum = asfloat(P[1].w);
    const uint index = thread.y * 8 + thread.x, count = 64;
    {
        ByteAddressBuffer brdf = ResourceDescriptorHeap[P[1].y];
        LgSh sh;
        sh.a = asfloat(brdf.Load4(probe * 40));
        sh.b = asfloat(brdf.Load4(probe * 40 + 16));
        sh.c = asfloat(brdf.Load(probe * 40 + 32));
        const float3 direction = lgSphere((float2(thread.xy) + 0.5) / 8.0);
        float density = max(lgShDot(sh, lgShBasis(direction)), 0.0);
        if (P[1].z != 0xFFFFFFFFu)
        {
            Texture2D<float> lighting = ResourceDescriptorHeap[P[1].z];
            const bool kept = density >= minimum;
            density *= lighting[atlas * LG_TRACE_RES + thread.xy] * 64.0;
            if (kept) density = max(density, minimum);
        }
        gs_rays[index] = lgSortInfo(thread.xy, 1, density);
    }
    GroupMemoryBarrierWithGroupSync();
    // Ascending by density (ties by index, later first): each thread counts the rays that go before its own.
    {
        uint2 texel;
        uint level;
        float key;
        lgSortInfoUnpack(gs_rays[index], texel, level, key);
        uint before = 0;
        [loop] for (uint i = 0; i < count; ++i)
        {
            const float other = asfloat(gs_rays[i].y);
            if (other < key || (other == key && i > index)) ++before;
        }
        gs_rays[count + before] = gs_rays[index];
    }
    gs_subdivide = 0;
    GroupMemoryBarrierWithGroupSync();
    // Threads 3 m, 3 m + 1, 3 m + 2 hold the m-th lowest triple; ray count - 1 - m is the m-th highest. While the triple is
    // under the minimum and below that ray, the triple becomes three of its children; the ray itself the fourth (below).
    {
        const uint part = index % 3, merge = index / 3;
        const uint refine = (uint)max((int)count - (int)merge - 1, 0);
        const uint last = merge * 3 + 2;
        if (last < refine && asfloat(gs_rays[count + last].y) < minimum)
        {
            uint2 texel;
            uint level;
            float density;
            lgSortInfoUnpack(gs_rays[count + refine], texel, level, density);
            if (level > 0)
            {
                gs_rays[count + index] = lgSortInfo(texel * 2 + uint2((part + 1) % 2, (part + 1) / 2), level - 1, 0.0);
                if (part == 0) InterlockedAdd(gs_subdivide, 1u);
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (index < gs_subdivide)
    {
        const uint target = count - index - 1;
        uint2 texel;
        uint level;
        float density;
        lgSortInfoUnpack(gs_rays[count + target], texel, level, density);
        gs_rays[count + target] = lgSortInfo(texel * 2, level - 1, 0.0);
    }
    GroupMemoryBarrierWithGroupSync();
    uint2 texel;
    uint level;
    float density;
    lgSortInfoUnpack(gs_rays[count + index], texel, level, density);
    RWTexture2D<uint> rays = ResourceDescriptorHeap[P[1].x];
    rays[atlas * LG_TRACE_RES + thread.xy] = lgPackRay(texel, level);
}
