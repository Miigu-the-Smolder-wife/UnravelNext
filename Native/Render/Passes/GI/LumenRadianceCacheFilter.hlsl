// unx-kernel: cs_6_6 main
// unx-variants: MODE=0,1
// r.gi.rc.filter / r.gi.rc.store (LumenRadianceCache.hlsli): one group layer per traced probe (SV_GroupID.z = the trace
// record), 8 x 8 threads over the probe's texels.
// MODE 0 (filter): the traced texel is averaged with the same texel of the 6 neighbour probes of its clipmap level, each
//   weighed by (i) the angle, seen from this probe, between this direction and the neighbour ray's hit point (the hit
//   distance clamped to this texel's: beyond 0.2 rad the neighbour saw something else), and (ii) mutual visibility:
//   this probe must see the point 2 TMin along the neighbour's ray and the neighbour the point 2 TMin along this
//   probe's ray (each test against the looking probe's own depth map), and a neighbour ray that hit a back face is
//   dropped. Neighbours' radiance is their stored (already filtered) value from earlier frames; a neighbour first traced
//   this frame has none yet and is skipped. Result into the second temporary atlas.
// MODE 1 (store): the filtered probe into its place of the cache's atlas with a 1-texel border (the equal-area map's
//   neighbour across each edge: the same edge mirrored about its centre), for bilinear lookups.
// P[0] = { parameters SRV, state SRV, trace records SRV, indirection SRV }
// P[1] = { MODE 0: traced radiance SRV / MODE 1: filtered SRV, depth atlas SRV, probe slots SRV, MODE 0: filtered UAV /
//          MODE 1: cache atlas UAV (R11G11B10F) }
// P[2] = { cache atlas SRV (MODE 0), max radiance hit angle (float, rad), 0, 0 }
#include "Bindless.hlsli"
#include "Frame.hlsli"
#include "Passes/GI/LumenRadianceCache.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 gid : SV_GroupID, uint3 id : SV_DispatchThreadID)
{
    const LrcParams p = lrcParams(P[0].x);
    ByteAddressBuffer state = ResourceDescriptorHeap[P[0].y];
    const uint trace = gid.z;
    if (trace >= state.Load(8)) return;
    ByteAddressBuffer traces = ResourceDescriptorHeap[P[0].z];
    const uint4 record = traces.Load4(trace * 16);
    const uint clipmap = (record.w >> 24) & 0x7Fu, slot = record.w & 0xFFFFFFu;
    const uint res = p.probeResolution;
    const uint2 temporaryBase = uint2(trace % p.tempProbes, trace / p.tempProbes) * res;
#if MODE == 0
    const uint2 texel = id.xy;
    if (any(texel >= res)) return;
    Texture2D<float4> traced = ResourceDescriptorHeap[P[1].x];
    Texture2D<uint> depthAtlas = ResourceDescriptorHeap[P[1].y];
    ByteAddressBuffer slots = ResourceDescriptorHeap[P[1].z];
    RWTexture2D<float4> filtered = ResourceDescriptorHeap[P[1].w];
    Texture3D<uint> indirection = ResourceDescriptorHeap[P[0].w];
    Texture2D<float4> atlas = ResourceDescriptorHeap[P[2].x];
    const float3 centre = asfloat(record.xyz);
    const uint2 depthBase = lrcAtlasCoord(p, slot) * res;
    float3 sum = traced[temporaryBase + texel].rgb;
    float weight = 1;
    const float hitDistance = lrcDepthDistance(depthAtlas[depthBase + texel]);
    const float3 direction = lrcUvToDirection((float2(texel) + 0.5) / res);
    const float testOffset = 2 * lrcTMin(p, clipmap), maxAngle = asfloat(P[2].y);
    const int3 coord = int3(floor(lrcCoordFloat(p, centre, clipmap)));
    static const int3 offsets[6] = { int3(-1, 0, 0), int3(1, 0, 0), int3(0, -1, 0), int3(0, 1, 0), int3(0, 0, -1), int3(0, 0, 1) };
    for (uint k = 0; k < 6; ++k)
    {
        const int3 nc = coord + offsets[k];
        const uint neighbour = lrcIndirection(indirection, p, nc, clipmap);
        if (neighbour >= LRC_USED) continue;
        if ((slots.Load(neighbour * 16 + 12) & 0xFFu) == 0) continue;  // (bucket 0: never traced before this frame, nothing stored yet)
        const uint2 neighbourDepthBase = lrcAtlasCoord(p, neighbour) * res;
        const uint neighbourDepth = depthAtlas[neighbourDepthBase + texel];
        if (!lrcDepthSeesFront(neighbourDepth)) continue;
        const float3 neighbourCentre = lrcProbePosition(p, uint3(nc), clipmap);
        // this probe sees the start of the neighbour's ray?
        {
            const float3 to = neighbourCentre + testOffset * direction - centre;
            const uint2 t = min(uint2(lrcDirectionToUv(to) * res), res - 1);
            const float d = lrcDepthDistance(depthAtlas[depthBase + t]);
            if (d * d < dot(to, to)) continue;
        }
        // the neighbour sees the start of this probe's ray?
        {
            const float3 to = centre + testOffset * direction - neighbourCentre;
            const uint2 t = min(uint2(lrcDirectionToUv(to) * res), res - 1);
            const float d = lrcDepthDistance(depthAtlas[neighbourDepthBase + t]);
            if (d * d < dot(to, to)) continue;
        }
        // (a long neighbour hit distance would make the angle small and favour distant light: no farther than our own)
        const float neighbourDistance = lrcDepthHit(neighbourDepth) ? min(lrcDepthDistance(neighbourDepth), hitDistance) : lrcDepthDistance(neighbourDepth);
        const float3 toHit = neighbourCentre + direction * neighbourDistance - centre;
        const float angle = acos(clamp(dot(toHit, direction) / max(length(toHit), 1e-6), -1.0, 1.0));
        const float w = 1 - saturate(angle / maxAngle);
        if (!(w > 0)) continue;
        sum += atlas[lrcAtlasCoord(p, neighbour) * p.finalResolution + 1 + texel].rgb * w;
        weight += w;
    }
    filtered[temporaryBase + texel] = float4(sum / weight, 1);
#else
    const uint2 texel = id.xy;  // of the bordered probe
    if (any(texel >= p.finalResolution)) return;
    Texture2D<float4> filtered = ResourceDescriptorHeap[P[1].x];
    RWTexture2D<float3> atlas = ResourceDescriptorHeap[P[1].w];
    // the interior texel a bordered texel shows: across an edge of the octahedral map the neighbour is the same edge's
    // texel mirrored about the edge's centre
    int2 s = int2(texel) - 1;
    const int n = int(res);
    if (s.x < 0 || s.x >= n)
    {
        s.x = clamp(s.x, 0, n - 1);
        s.y = n - 1 - s.y;
    }
    if (s.y < 0 || s.y >= n)
    {
        s.y = clamp(s.y, 0, n - 1);
        s.x = n - 1 - s.x;
    }
    s = clamp(s, 0, n - 1);
    atlas[lrcAtlasCoord(p, slot) * p.finalResolution + texel] = filtered[temporaryBase + uint2(s)].rgb;
#endif
}
