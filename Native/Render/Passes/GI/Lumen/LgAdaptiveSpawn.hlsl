// unx-kernel: cs_6_6 main
// gi.lumen, r.gi.lg.adaptive.spawn: one thread per tile. Each marked candidate (LgAdaptiveMark.hlsl) becomes a probe
// unless marked candidates with a lower index in the 4 tiles around it already cover its pixel (their adaptive weights
// sum to the minimum interpolation weight) - the decision reads the masks alone, so it does not depend on the order
// the threads run in. A probe takes the next adaptive index (count in the adaptive buffer's word 0, the probes past
// the maximum are dropped), its screen position, a slot in its tile's list and its row in the probe textures.
// P[0] = LgSurface inputs, P[1] = { probe depth UAV, probe normal UAV, probe position UAV, mask SRV },
// P[10].z = adaptive buffer UAV. b1 = the view.
#include "Passes/GI/Lumen/LgAdaptive.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= lgProbeViewSize())) return;
    Texture2D<uint> mask = ResourceDescriptorHeap[P[1].w];
    const uint own = mask[id.xy];
    if (own == 0) return;
    RWTexture2D<float> probeDepth = ResourceDescriptorHeap[P[1].x];
    RWTexture2D<float2> probeNormal = ResourceDescriptorHeap[P[1].y];
    RWTexture2D<float4> probePosition = ResourceDescriptorHeap[P[1].z];
    RWByteAddressBuffer adaptive = ResourceDescriptorHeap[P[10].z];
    [loop] for (uint i = 0; i < LG_TILE_ADAPTIVE; ++i)
    {
        if ((own & (1u << i)) == 0) continue;
        const uint2 pixel = lgAdaptivePixel(id.xy, i);
        const LgSurface s = lgSurface(pixel);
        if (!s.valid) continue;
        const float4 plane = float4(s.normal, dot(s.position, s.normal));
        const float2 full = clamp((float2)pixel - (float2)lgTileJitter(lgTemporalIndex()), 0.0, (float2)lgViewSize() - 1.0);
        const uint2 tile00 = min((uint2)(full / (float)lgTile()), lgProbeViewSize() - 2);
        float4 corner = 0;
        bool place = true;
        [loop] for (uint c = 0; c < 4 && place; ++c)
        {
            const uint2 tile = tile00 + uint2(c & 1u, c >> 1);
            uint bits = mask[tile];
            [loop] while (bits != 0)
            {
                const uint k = firstbitlow(bits);
                bits &= bits - 1;
                if (k >= i) break;
                const uint2 other = lgAdaptivePixel(tile, k);
                const LgSurface so = lgSurface(other);
                if (!so.valid) continue;
                corner[c] = max(corner[c], lgAdaptiveWeight((float2)pixel, plane, s.depth, false, (float2)other, so.position).x);
                if (dot(corner, 1) >= LG_MIN_INTERPOLATION_WEIGHT)
                {
                    place = false;
                    break;
                }
            }
        }
        if (!place) continue;
        uint index;
        adaptive.InterlockedAdd(0, 1u, index);
        if (index >= lgMaxAdaptive()) continue;
        adaptive.Store(16 + index * 4, lgPackScreen(pixel));
        const uint header = lgTileHeaderAddress(id.xy);
        uint slot;
        adaptive.InterlockedAdd(header, 1u, slot);
        if (slot < LG_TILE_ADAPTIVE) adaptive.Store(header + 4 + slot * 4, index);
        const uint2 atlas = lgAtlasCoord(lgUniformProbes() + index);
        probeDepth[atlas] = s.depth;
        probeNormal[atlas] = lgEncodeNormal(s.normal);
        probePosition[atlas] = float4(s.position, distance(s.position, lgPreviousPosition(pixel, s)));
    }
}
