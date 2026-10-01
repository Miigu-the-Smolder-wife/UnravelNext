// unx-kernel: cs_6_6 main
// Local invalidation after scene edits (B3; RayScene::changes(): world AABBs of geometry added, removed, re-meshed or moved
// while static, at an incremental rebuild or in this frame). An instance edit no longer starts a new lighting epoch for
// the whole cache: only the entries whose texel rays can see a changed box restart their history (history word 0:
// young, so the next update replaces instead of averaging with the stale light) and are requested for update this frame.
// One group per entry, thread = texel: the texel's rays over its updates spread over its octahedral texel (a cone of
// half-angle GI_TEXEL_CONE around its centre direction) up to the stored hit distance (65000 = sky); a box's bounding
// sphere tested against that cone segment (conservative), and the anchor inside a sphere counts.
// P[0] = { cache UAV, boxes SRV (raw: count, then per box min xyz, 0, max xyz, 0), 0, 0 }; dispatch 512 x ceil(capacity / 512).
#include "Passes/GI/GiInternal.hlsli"

#define GI_TEXEL_CONE_TAN 0.31  // tan(~17 deg): a texel of the 8 x 8 hemisphere map around its centre, corners included
#define GI_INVALIDATE_MIN_SOLID 0.01  // (r / distance)^2: the box's solid angle over pi, the irradiance share it can change

groupshared uint gs_seen;

[numthreads(64, 1, 1)]
void main(uint lane : SV_GroupIndex, uint2 group : SV_GroupID)
{
    RWByteAddressBuffer b = ResourceDescriptorHeap[P[0].x];
    const GiHeader h = giHeader(b);
    const uint entry = group.y * 512 + group.x;
    if (entry >= h.capacity) return;                      // uniform over the group
    if (b.Load(h.offMeta + entry * 16 + 4) == 0) return;  // free (keys always have bit 63 set)
    if (lane == 0) gs_seen = 0;
    GroupMemoryBarrierWithGroupSync();
    ByteAddressBuffer boxes = ResourceDescriptorHeap[P[0].y];
    const uint count = boxes.Load(0);
    const float3 anchor = giAnchorPosition(b, h, entry);
    const float3 n = giAnchorNormal(b, h, entry);
    float3 tb, bb;
    giBasis(n, tb, bb);
    const float3 local = giHemiOctDecode((float2(lane % GI_TEXELS, lane / GI_TEXELS) + 0.5) / GI_TEXELS);
    const float3 dir = normalize(tb * local.x + bb * local.y + n * local.z);
    const float reach = f16tof32(b.Load(h.offTexels + (entry * GI_TEXEL_COUNT + lane) * 8 + 4) >> 16);
    bool seen = false;
    [loop] for (uint i = 0; i < count && !seen; ++i)
    {
        const float3 lo = asfloat(boxes.Load3(16 + i * 32)), hi = asfloat(boxes.Load3(16 + i * 32 + 16));
        const float3 d = (lo + hi) * 0.5 - anchor;
        const float r = length(hi - lo) * 0.5;
        const float t = dot(d, dir);
        const float perp = length(d - dir * t);
        const float dd = dot(d, d);
        // A box that subtends less than GI_INVALIDATE_MIN_SOLID of the hemisphere's pi (cosine-weighted) changes the
        // entry's irradiance by less than that share: the running mean absorbs it (history_updates_max) instead of a
        // restart of every entry that can see it (an open ground sees every new object; D0-sized scenes would restart all).
        const bool sizeable = r * r >= GI_INVALIDATE_MIN_SOLID * dd;
        seen = dd <= r * r || (sizeable && t > -r && t - r < reach && perp < r + max(t, 0.0) * GI_TEXEL_CONE_TAN);
    }
    if (seen) InterlockedOr(gs_seen, 1u);
    GroupMemoryBarrierWithGroupSync();
    if (lane != 0 || gs_seen == 0) return;
    b.Store(h.offSh + entry * GI_SH_STRIDE + GI_SH_HISTORY, 0u);
    giAccClear(b, entry);  // gi.hit_accumulator: its direct light changed
    giRequestUpdate(b, h, entry, 0);
}
