// unx-kernel: cs_6_6 main
// Splits this frame's fixed ray budget into whole-hemisphere updates (64 rays each). Two tiers: entries the screen probes
// read (tier 0) and entries last frame's GI rays read for bounces (tier 1). Tier 1 is guaranteed a share of the updates
// (gi.hit_update_share) when it needs them, tier 0 takes the rest; a tier's unused budget goes to the other. Within a
// tier the stalest entries go first: the tier's age histogram is accumulated from the oldest bucket, every bucket above
// the threshold is taken and the threshold bucket up to its quota. Updates left over go to background entries.
// One group of 128 threads = 2 tiers x 64 buckets (suffix sums in groupshared memory).
// P[0] = { cache UAV, updates per frame (budget / 64), hit share (float bits), 0 }
#include "Passes/GI/GiInternal.hlsli"

groupshared uint g_count[2 * GI_AGE_BUCKETS];
groupshared uint g_suffix[2 * GI_AGE_BUCKETS];  // entries in buckets >= k of the tier
groupshared uint g_budget[2];

[numthreads(128, 1, 1)]
void main(uint lane : SV_GroupIndex)
{
    RWByteAddressBuffer b = ResourceDescriptorHeap[P[0].x];
    const uint tier = lane / GI_AGE_BUCKETS, k = lane % GI_AGE_BUCKETS;
    g_count[lane] = b.Load(GI_HISTOGRAM + lane * 4);
    GroupMemoryBarrierWithGroupSync();
    uint suffix = 0;
    [loop] for (uint j = k; j < GI_AGE_BUCKETS; ++j) suffix += g_count[tier * GI_AGE_BUCKETS + j];
    g_suffix[lane] = suffix;
    GroupMemoryBarrierWithGroupSync();
    if (lane == 0)
    {
        const GiHeader h = giHeader(b);
        const uint updates = P[0].y;
        const uint screen = g_suffix[0], hit = g_suffix[GI_AGE_BUCKETS];
        const uint hitUpdates = min(hit, max((uint)(updates * asfloat(P[0].z)), updates - min(screen, updates)));
        const uint screenUpdates = min(screen, updates - hitUpdates);
        g_budget[0] = screenUpdates;
        g_budget[1] = hitUpdates;
        b.Store(GI_H_BG_COUNT, updates - screenUpdates - hitUpdates);
        b.Store(GI_H_LIVE_COUNT, h.capacity - h.freeCount);
    }
    GroupMemoryBarrierWithGroupSync();
    // The threshold bucket of a range is the k whose suffix reaches the range's budget while suffix(k + 1) does not.
    // Redesign V2 P1 (gi.update_tiers, 1.1a): the young range (buckets >= GI_T0_BUCKET: fewer than 4 updates since the entry's
    // creation or restart) takes at most gi.young_update_share of the tier's budget (the rest keeps converging and rotating:
    // no starvation after a cut), with its own threshold (GI_H_SELECT_T0); the buckets below take what is left.
    const uint budget = g_budget[tier];
    const bool tiers = (b.Load(GI_P1_FLAGS) & 1u) != 0;
    const uint young = tiers ? g_suffix[tier * GI_AGE_BUCKETS + GI_T0_BUCKET] : 0u;
    const uint youngQuota = tiers ? min(young, (uint)(budget * asfloat(b.Load(GI_P1_T0_SHARE)))) : 0u;
    const uint rest = budget - youngQuota;
    const uint top = tiers ? GI_T0_BUCKET : GI_AGE_BUCKETS;
    if (tiers && k >= GI_T0_BUCKET)
    {
        const uint above = k + 1 < GI_AGE_BUCKETS ? g_suffix[lane + 1] : 0;
        const bool threshold = youngQuota > 0 ? (suffix >= youngQuota && above < youngQuota) || (k == GI_T0_BUCKET && suffix < youngQuota) : k == GI_AGE_BUCKETS - 1;
        if (threshold) b.Store4(GI_H_SELECT_T0 + tier * 16, uint4(k, youngQuota > 0 ? min(youngQuota - above, g_count[lane]) : 0, 0, 0));
    }
    if (k < top)
    {
        const uint low = suffix - young;  // entries in buckets k .. top - 1
        const uint above = k + 1 < top ? g_suffix[lane + 1] - young : 0;
        const bool threshold = rest > 0 ? (low >= rest && above < rest) || (k == 0 && low < rest) : k == top - 1;
        if (threshold) b.Store4(GI_H_SELECT + tier * 16, uint4(k, rest > 0 ? min(rest - above, g_count[lane]) : 0, 0, 0));
    }
}
