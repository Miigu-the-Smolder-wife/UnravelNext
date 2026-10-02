// unx-kernel: cs_6_6 main
// unx-variants: STAGE=0,1,2
// r.card.select (CardLighting.hlsli): which card pages get their direct light and their radiosity updated this frame -
// the reference's update selection (priority histogram, max bucket, lists), for both contexts (0 direct, 1 radiosity).
//   STAGE 0  one thread per card page: its priority bucket per context (0 = most urgent) = 15 - log2(4 x frames since
//            its last update x update speed); a page never updated counts as 2048 frames. Update speed = 1 / (1 +
//            distance from the camera to the page's box / P[4].z), doubled when the box is within P[4].w of the view
//            frustum. The histograms get the page's tiles.
//   STAGE 1  one thread: per context the max bucket - the first whose running tile count reaches the budget - and the
//            tiles the budget leaves for that bucket.
//   STAGE 2  one thread per card page: pages under the max bucket are listed, pages in it while the budget lasts; a
//            listed page's tiles go to the context's tile list, its page-light record gets this frame and the next
//            temporal index.
// P[0] = { card frame SRV, select UAV, frame index, 0 }
// P[4] = { page light UAV (raw), page capacity, asuint(update distance, m), asuint(frustum margin, m) }
// P[5] = { direct tile budget, radiosity tile budget, direct list capacity, radiosity list capacity }
#include "Frame.hlsli"
#include "Passes/SurfaceCache/CardLighting.hlsli"

uint2 pageTiles(McCardPage page) { return (uint2(page.sizeInTexels) + MC_TILE - 1) / MC_TILE; }

#if STAGE == 0
float pointBoxDistance(float3 p, float3 extent)
{
    const float3 d = max(abs(p) - extent, 0);
    return length(d);
}

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const McFrame f = mcFrame(P[0].x);
    const uint index = id.x;
    if (index >= f.pageCount || index >= P[4].y) return;
    RWByteAddressBuffer select = ResourceDescriptorHeap[P[0].y];
    select.Store(clPageBucketOffset(index), 0xFFFFFFFFu);
    const McCardPage page = mcLoadCardPage(f, index);
    if (!(page.sizeInTexels.x > 0)) return;
    const McCard card = mcLoadCard(f, page.card);
    if (!mcVisible(card)) return;
    // the page's box in card space (its uv rectangle over the card's face, the card's whole depth)
    const float2 uvCentre = 0.5 * (page.cardUvRect.xy + page.cardUvRect.zw), uvHalf = 0.5 * (page.cardUvRect.zw - page.cardUvRect.xy);
    const float3 centre = float3((uvCentre * 2 - 1) * card.extent.xy, 0), extent = float3(uvHalf * 2 * card.extent.xy, card.extent.z);
    const float distance = pointBoxDistance(mcWorldToCard(card, g_cameraPosition) - centre, extent);
    float speed = 1 / (1 + distance / max(asfloat(P[4].z), 1e-3));
    {
        const float3 worldCentre = mcCardToWorld(card, centre);
        const float3 ax = mcCardToWorldVector(card, float3(extent.x, 0, 0)), ay = mcCardToWorldVector(card, float3(0, extent.y, 0)), az = mcCardToWorldVector(card, float3(0, 0, extent.z));
        const float3 worldExtent = abs(ax) + abs(ay) + abs(az);
        // the view's planes (inside positive): left, right, bottom, top, near
        const float4 r0 = g_viewProj[0], r1 = g_viewProj[1], r2 = g_viewProj[2], r3 = g_viewProj[3];
        const float4 planes[5] = { r3 + r0, r3 - r0, r3 + r1, r3 - r1, r3 - r2 };
        bool nearFrustum = true;
        for (uint k = 0; k < 5; ++k)
        {
            const float scale = 1 / max(length(planes[k].xyz), 1e-12);
            const float inside = (dot(planes[k].xyz, worldCentre) + planes[k].w) * scale;
            const float radius = dot(worldExtent, abs(planes[k].xyz)) * scale;
            nearFrustum = nearFrustum && inside >= -(radius + asfloat(P[4].w));
        }
        if (nearFrustum) speed *= 2;
    }
    RWByteAddressBuffer light = ResourceDescriptorHeap[P[4].x];
    const ClPageLight pl = clPageLight(light.Load4(index * CL_PAGE_LIGHT_BYTES));
    const uint2 tiles2 = pageTiles(page);
    const uint tiles = tiles2.x * tiles2.y;
    uint buckets = 0;
    for (uint context = 0; context < 2; ++context)
    {
        const uint last = context ? pl.indirectFrame : pl.directFrame;
        const float frames = last == 0 ? CL_NEVER_FRAMES : (float)(P[0].z + 1 - last);
        const uint bucket = CL_BUCKETS - 1 - (uint)clamp(log2(max(4.0 * frames * speed, 1.0)), 0.0, CL_BUCKETS - 1.0);
        select.InterlockedAdd(clHistogramOffset(context, bucket), tiles);
        buckets |= bucket << (8 * context);
    }
    select.Store(clPageBucketOffset(index), buckets);
}
#elif STAGE == 1
[numthreads(1, 1, 1)]
void main()
{
    RWByteAddressBuffer select = ResourceDescriptorHeap[P[0].y];
    for (uint context = 0; context < 2; ++context)
    {
        const uint budget = context ? P[5].y : P[5].x;
        uint sum = 0, bucket = 0;
        for (; bucket < CL_BUCKETS; ++bucket)
        {
            const uint tiles = select.Load(clHistogramOffset(context, bucket));
            if (sum + tiles >= budget) break;
            sum += tiles;
        }
        // (every bucket fits: bucket = CL_BUCKETS, all pages listed)
        select.Store(clSelectContext(context) + CL_SELECT_MAX_BUCKET, bucket);
        select.Store(clSelectContext(context) + CL_SELECT_ALLOWED, budget - min(sum, budget));
    }
}
#else
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const McFrame f = mcFrame(P[0].x);
    const uint index = id.x;
    if (index >= f.pageCount || index >= P[4].y) return;
    RWByteAddressBuffer select = ResourceDescriptorHeap[P[0].y];
    const uint buckets = select.Load(clPageBucketOffset(index));
    if (buckets == 0xFFFFFFFFu) return;
    const McCardPage page = mcLoadCardPage(f, index);
    const uint2 tiles2 = pageTiles(page);
    const uint tiles = tiles2.x * tiles2.y;
    RWByteAddressBuffer light = ResourceDescriptorHeap[P[4].x];
    for (uint context = 0; context < 2; ++context)
    {
        const uint head = clSelectContext(context);
        const uint bucket = (buckets >> (8 * context)) & 0xFFu, maxBucket = select.Load(head + CL_SELECT_MAX_BUCKET);
        if (bucket > maxBucket) continue;
        if (bucket == maxBucket)
        {
            // the budget's rest, in the order the pages arrive (a page is taken whole)
            uint reserved;
            select.InterlockedAdd(head + CL_SELECT_RESERVED, tiles, reserved);
            if (reserved >= select.Load(head + CL_SELECT_ALLOWED)) continue;
        }
        uint first;
        select.InterlockedAdd(head + CL_SELECT_TILES, tiles, first);
        const uint capacity = context ? P[5].w : P[5].z;
        if (first + tiles > capacity) continue;  // (cannot happen: capacity = budget + a page's tiles; the list stays inside its buffer)
        for (uint t = 0; t < tiles; ++t)
            select.Store(clTileListOffset(P[4].y, P[5].z, context, first + t), clPackTile(index, uint2(t % tiles2.x, t / tiles2.x)));
        uint before;
        select.InterlockedAdd(head + CL_SELECT_PAGES, 1u, before);
        const uint at = index * CL_PAGE_LIGHT_BYTES + context * 4u;
        light.Store(at, P[0].z + 1);
        light.Store(at + 8, light.Load(at + 8) + 1);
    }
}
#endif
