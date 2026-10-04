// unx-kernel: cs_6_6 main
// unx-variants: MODE=0,1
// Froxel light lists (ARCHITECTURE 2.4, INTERFACES 7.4; RENDERER_REDESIGN_V2 14.1: variable-length lists, no cap). One group
// per screen tile, one thread per depth slice:
//  1. the tile's frustum (four planes through the camera) culls every light's bounding sphere, the group's threads taking
//     the lights in turn (512 lights x 14.4 k tiles at 4K = 7.4 M sphere tests): the scene's, then the FX particle lights
//     of the buffer's tail (froxelLightTotal, A3: + 14.4 k F tests at 4K). Candidates are compacted in light-index order
//     (a ballot per batch of 64), so the lists never depend on which thread finished first. MODE=0 keeps a tile's
//     candidates (P[1].w: FROXEL_STORED of them at most, 16 bits each after their count) and MODE=1 takes them from
//     there: the lights x tiles test runs once a frame. A tile with more candidates than are kept is culled again by
//     MODE=1 (the same test on the same lights: the same candidates);
//  2. each slice keeps the candidates whose bounds reach its froxel: view-depth range, bounding sphere of the froxel
//     (not for the last slice, which extends to infinity), spot cone, emitter plane of one-sided area lights;
//  3. MODE=0 (count): the number of lights reaching the froxel goes to the header (first 0, scene lights | all << 16). FroxelScan.hlsl then
//     turns the counts (rounded up to even: every run starts at an even entry, froxelLightBuffered) into each froxel's
//     first entry: an exclusive prefix within its block of 2048 froxels in the header's first word, the block totals
//     scanned separately.
//     MODE=1 (fill): the same culling; the strongest lights_max lights (importance at the froxel: peak intensity x
//     distance window / squared distance; ties by light index) are the ordered head of the list, every other light
//     reaching the froxel follows them (deterministic: the order the insertion sort evicts or rejects them in). Nothing
//     is truncated or merged: a froxel lists every light that reaches it. A list is cut only by the buffer's capacity
//     (FroxelGrid::capacity, a size: entries that would lie past it are lost and counted in the header statistics; the
//     gates require 0).
//     The head's order has one reader: S's visibility slots 1-3 go to the first three shadow-casting lights of a list, so
//     the strongest take them. In a frame without local shadow slots (P[2].x bit 0: shading.mega_lights takes the local
//     lights' shadows with its samples' rays, or no light casts) no reader looks at the order, and the list is written
//     in light-index order without the head's insertion sort: the same lights in every list.
//  4. header words (first entry, count) and 16-bit indices into the froxel's run (FroxelCommon.hlsli).
// P[0].x froxelLights UAV (raw; header written by FroxelBegin), P[0].y lights_max (the ordered head, even, <= 96),
// P[0].z slot of light SRV (StructuredBuffer<uint>: shadow slot or VSM_LOCAL_NONE per scene light; 0xFFFFFFFF: no local
// shadows): entries carry bit 15 when their light has a shadow slot (froxelLightShadowed).
// P[0].w tile readers SRV (Texture2D<float2>, FroxelTileDepth.hlsl; 0xFFFFFFFF: every tile): a tile none of whose 3 x 3
// neighbourhood holds a read pixel (planar views: tiles without mirror pixels) gets empty lists and no culling.
// P[1].x block offsets SRV (raw, FroxelScan MODE=1's exclusive block sums: all lights at 0, scene lights at 8192; MODE=1
// only), P[1].y tests: 1 forces the fallback below, P[1].z scene allocation SRV (raw, FroxelScan MODE=0: each froxel's
// exclusive prefix over the scene lights alone; MODE=1 only), P[1].w tile candidates (raw, FROXEL_STORED_BYTES per tile:
// MODE=0 UAV, MODE=1 SRV; 0xFFFFFFFF: each mode culls for itself).
// P[2].x bit 0: no reader of the head's order this frame (MODE=1: light-index order, no sort).
// Frame constants of the view.
#include "Bindless.hlsli"
#include "Frame.hlsli"
#include "Scene.hlsli"
#include "Passes/Atmosphere/FroxelCommon.hlsli"

#define FROXEL_CANDIDATES 1024u
#define FROXEL_SORTED_MAX 96u   // ordered head kept in groupshared memory per slice (64 x 96 x 4 B = 24 KB)
#define FROXEL_STORED 254u      // a tile's candidates kept from MODE=0 for MODE=1: a count word + 127 words of two
#define FROXEL_STORED_BYTES 512u  // (FroxelSystem.cpp kTileCandidateBytes)

groupshared uint gs_candidates[FROXEL_CANDIDATES];
groupshared uint gs_candidateCount;
groupshared uint gs_batchBits[2];
#if MODE == 1
groupshared uint gs_keys[64 * FROXEL_SORTED_MAX];  // per slice, descending: importance code << 16 | (0xFFFF - light)
#endif
groupshared uint gs_listed;
groupshared uint gs_cut, gs_dropped, gs_max;

// Monotonic 16-bit code of a positive importance: 1/256 octave steps over [2^-64, 2^192).
uint importanceCode(float importance) { return (uint)clamp((log2(max(importance, 5.4e-20)) + 64) * 256, 0.0, 65535.0); }

// Whether light li reaches the slice's froxel (view-depth range, bounding sphere, spot cone, emitter plane), and its
// importance key there.
bool froxelReaches(GpuLight l, uint li, bool last, float z0, float z1, float3 centre, float radius, float3 forward, out uint key)
{
    key = 0;
    const float r = froxelLightRadius(l);
    const float3 v = centre - l.position;
    const float lz = dot(l.position - g_cameraPosition, forward);
    if (lz + r < z0 || lz - r > z1) return false;
    const float dist = length(v);
    if (!last && dist > r + radius) return false;
    const uint type = lightType(l);
    if (!last && type == LIGHT_SPOT && l.spotScale > 0)
    {
        // Sphere against the cone of half-angle acos(-spotOffset / spotScale).
        const float cosA = -l.spotOffset / l.spotScale;
        if (cosA > -1)
        {
            const float sinA = sqrt(saturate(1 - cosA * cosA));
            const float along = dot(v, l.forward);
            const float across = sqrt(max(dist * dist - along * along, 0.0));
            if (cosA * across - along * sinA > radius || along < -radius) return false;
        }
    }
    if (!last && (type == LIGHT_RECT || type == LIGHT_DISK) && dot(v, l.forward) < -radius) return false;
    const float dn = max(dist - radius, 0.0);
    const float extent = max(l.size.x, l.size.y);
    const float importance = froxelPeakIntensity(l) * froxelWindow(l, dn) / (dn * dn + 0.25 * radius * radius + extent * extent);
    key = importanceCode(importance) << 16 | (0xFFFFu - li);
    return true;
}

// Entry of light li with its shadow-slot bit.
uint froxelEntryOf(uint li)
{
    uint e = li;
    if (P[0].z != 0xFFFFFFFFu)
    {
        StructuredBuffer<uint> slotOf = ResourceDescriptorHeap[P[0].z];
        e |= li < g_lightCount && slotOf[li] != 0xFFFFu ? 0x8000u : 0u;  // FX lights (index >= g_lightCount): no slot
    }
    return e;
}

[numthreads(64, 1, 1)]
void main(uint3 gid : SV_GroupID, uint s : SV_GroupIndex)
{
    RWByteAddressBuffer buffer = ResourceDescriptorHeap[P[0].x];
    const FroxelGrid g = buffer.Load<FroxelGrid>(0);
    const uint listMax = min(P[0].y, FROXEL_SORTED_MAX);
    const uint2 tile = gid.xy;
    if (P[0].w != 0xFFFFFFFFu)
    {
        Texture2D<float2> readers = ResourceDescriptorHeap[P[0].w];
        bool read = false;
        [unroll] for (int dy = -1; dy <= 1; ++dy)
            [unroll] for (int dx = -1; dx <= 1; ++dx)
            {
                const float2 r = readers[clamp(int2(tile) + int2(dx, dy), 0, int2(g.gridX, g.gridY) - 1)];
                read = read || r.x > 0 || r.y > 0;
            }
        if (!read)
        {
            if (s < g.slices)
            {
                const uint froxel = froxelIndex(g, tile, s);
                buffer.Store2(g.headerBase + froxel * 8, uint2(0, 0));
            }
            return;
        }
    }
    if (s == 0)
    {
        gs_candidateCount = 0;
        gs_listed = 0;
        gs_cut = 0;
        gs_dropped = 0;
        gs_max = 0;
    }
    // Tile corners scaled to unit view depth, counter-clockwise on screen (x right, y down).
    const float2 p0 = float2(tile) * g.tilePx, p1 = p0 + g.tilePx;
    float3 corner[4];
    corner[0] = froxelRayAt(p0);
    corner[1] = froxelRayAt(float2(p0.x, p1.y));
    corner[2] = froxelRayAt(p1);
    corner[3] = froxelRayAt(float2(p1.x, p0.y));
    const float3 axis = froxelRayAt(0.5 * (p0 + p1));
    float3 plane[4];
    [unroll] for (uint i = 0; i < 4; ++i)
    {
        const float3 n = normalize(cross(corner[i], corner[(i + 1) & 3]));
        plane[i] = dot(n, axis) >= 0 ? n : -n;
    }
    const float3 forward = froxelForward();
    GroupMemoryBarrierWithGroupSync();

    // 1. Tile frustum; candidates compacted in light-index order (one ballot per batch of 64 lights).
    const uint lightTotal = froxelLightTotal();
    const uint tileBytes = (tile.y * g.gridX + tile.x) * FROXEL_STORED_BYTES;
    uint kept = 0xFFFFFFFFu;  // the candidates MODE=0 kept for this tile (their count; above FROXEL_STORED: not kept)
#if MODE == 1
    if (P[1].w != 0xFFFFFFFFu)
    {
        ByteAddressBuffer stored = ResourceDescriptorHeap[P[1].w];
        kept = stored.Load(tileBytes);
        if (kept <= FROXEL_STORED)
        {
            for (uint w = s; 2 * w < kept; w += 64)
            {
                const uint pair = stored.Load(tileBytes + 4 + w * 4);
                gs_candidates[2 * w] = pair & 0xFFFFu;
                if (2 * w + 1 < kept) gs_candidates[2 * w + 1] = pair >> 16;
            }
            if (s == 0) gs_candidateCount = kept;
            GroupMemoryBarrierWithGroupSync();
        }
    }
#endif
    for (uint base = 0; base < (kept <= FROXEL_STORED ? 0u : lightTotal); base += 64)  // (uniform over the group)
    {
        if (s == 0) { gs_batchBits[0] = 0; gs_batchBits[1] = 0; }
        GroupMemoryBarrierWithGroupSync();
        const uint li = base + s;
        bool inside = false;
        if (li < lightTotal)
        {
            const GpuLight l = loadLight(li);
            const float3 c = l.position - g_cameraPosition;
            const float r = froxelLightRadius(l);
            inside = dot(c, forward) >= -r;
            [unroll] for (uint p = 0; p < 4; ++p) inside = inside && dot(plane[p], c) >= -r;
            if (inside) InterlockedOr(gs_batchBits[s >> 5], 1u << (s & 31));
        }
        GroupMemoryBarrierWithGroupSync();
        const uint b0 = gs_batchBits[0], b1 = gs_batchBits[1];
        const uint below = s < 32 ? countbits(b0 & ((1u << s) - 1u)) : countbits(b0) + countbits(b1 & ((1u << (s - 32)) - 1u));
        const uint total = countbits(b0) + countbits(b1);
        const uint slot = gs_candidateCount + below;
        if (inside && slot < FROXEL_CANDIDATES) gs_candidates[slot] = li;
        GroupMemoryBarrierWithGroupSync();
        if (s == 0) gs_candidateCount += total;
        GroupMemoryBarrierWithGroupSync();
    }
    const uint candidates = min(gs_candidateCount, FROXEL_CANDIDATES);
#if MODE == 0
    if (P[1].w != 0xFFFFFFFFu)
    {
        RWByteAddressBuffer stored = ResourceDescriptorHeap[P[1].w];
        if (s == 0) stored.Store(tileBytes, gs_candidateCount);
        if (gs_candidateCount <= FROXEL_STORED)
            for (uint w = s; 2 * w < candidates; w += 64)
                stored.Store(tileBytes + 4 + w * 4, gs_candidates[2 * w] | (2 * w + 1 < candidates ? gs_candidates[2 * w + 1] : 0u) << 16);
    }
#endif

    // 2-3. This slice's list.
    uint count = 0, sceneCount = 0;
#if MODE == 1
    uint sorted = 0, stored = 0, pendingEntry = 0, droppedFx = 0, listed = 0;
    bool fallback = false, unordered = false;
    uint first = 0, limit = 0;  // the run [first, first + count) cut at the capacity: limit = entries this run may store
#endif
    if (s < g.slices)
    {
        const bool last = s + 1 == g.slices;
        const float z0 = froxelNodeDepth(g, s), z1 = last ? 3.0e38 : froxelNodeDepth(g, s + 1);
        // Bounding sphere of the froxel (the last slice: of its part up to farM, for the importance only).
        const float zb = last ? max(g.farM, 2 * z0) : z1;
        float3 lo = g_cameraPosition + corner[0] * z0, hi = lo;
        [unroll] for (uint q = 0; q < 8; ++q)
        {
            const float3 p = g_cameraPosition + corner[q & 3] * ((q & 4) ? zb : z0);
            lo = min(lo, p);
            hi = max(hi, p);
        }
        const float3 centre = 0.5 * (lo + hi);
        const float radius = 0.5 * length(hi - lo);
#if MODE == 1
        const uint froxel = froxelIndex(g, tile, s);
        {
            ByteAddressBuffer blocks = ResourceDescriptorHeap[P[1].x];
            first = buffer.Load(g.headerBase + froxel * 8) + blocks.Load((froxel >> 11) * 4);
        }
        unordered = (P[2].x & 1u) != 0;
        fallback = buffer.Load(28) > g.capacity || P[1].y != 0;  // P[1].y: tests force the fallback
        if (fallback)
        {
            // The frame's lists need more entries than the buffer holds (FroxelGrid::needed, FroxelScan.hlsl). The scene's
            // lights cannot bring this about (FroxelSystem.cpp sizes the buffer from an upper bound of their entries); the
            // FX particle lights, whose reach is computed on the GPU, can when they exceed their allowance. This frame the
            // runs are the scene lights' own allocation (FroxelScan's second prefix sum, inside the bound), every scene
            // light is listed as usual (head and tail) and the FX lights are left out: counted as lost entries in cut
            // lists, so the gate fails on the frame and the allowance grows (FroxelSystem.cpp).
            ByteAddressBuffer sceneAlloc = ResourceDescriptorHeap[P[1].z];
            ByteAddressBuffer blocks = ResourceDescriptorHeap[P[1].x];
            first = sceneAlloc.Load(froxel * 4) + blocks.Load(8192 + (froxel >> 11) * 4);
        }
        limit = first < g.capacity ? g.capacity - first : 0u;
        const uint keys = s * FROXEL_SORTED_MAX;
#endif
        for (uint j = 0; j < candidates; ++j)
        {
            const uint li = gs_candidates[j];
            const GpuLight l = loadLight(li);
            uint key;
            if (!froxelReaches(l, li, last, z0, z1, centre, radius, forward, key)) continue;
            ++count;
            if (li < g_lightCount) ++sceneCount;
#if MODE == 1
            if (fallback && li >= g_lightCount) { ++droppedFx; continue; }
            // Insertion into the descending head of listMax; the key the head rejects or evicts goes to the tail,
            // written straight into the run (this thread owns every word of its run: the head starts at the even entry
            // 'first' and listMax is even, so no word is shared with another thread or with the head).
            uint leaving = 0xFFFFFFFFu;  // key leaving the head this step (none)
            if (unordered) leaving = key & 0xFFFFu;  // no head: every light goes straight into the run, in candidate order
            else if (sorted == listMax && key <= gs_keys[keys + sorted - 1]) leaving = key;
            else
            {
                if (sorted == listMax) leaving = gs_keys[keys + sorted - 1];
                uint at = sorted < listMax ? sorted++ : listMax - 1;
                while (at > 0 && gs_keys[keys + at - 1] < key)
                {
                    gs_keys[keys + at] = gs_keys[keys + at - 1];
                    --at;
                }
                gs_keys[keys + at] = key;
            }
            if (leaving != 0xFFFFFFFFu)
            {
                const uint pos = sorted + stored;  // tail position within the run (sorted == listMax here; no head: 0)
                if (pos < limit)
                {
                    const uint e = froxelEntryOf(0xFFFFu - (leaving & 0xFFFFu));
                    if ((pos & 1) == 0) pendingEntry = e;
                    else buffer.Store(g.indexBase + (first + pos - 1) * 2, pendingEntry | e << 16);
                    ++stored;
                }
            }
#endif
        }
        InterlockedMax(gs_max, count);
#if MODE == 1
        listed = count - droppedFx;
        if (listed > limit || droppedFx != 0)
        {
            InterlockedAdd(gs_cut, 1);
            InterlockedAdd(gs_dropped, droppedFx + (listed > limit ? listed - limit : 0u));
        }
#endif
    }

    // 4. Output and statistics.
#if MODE == 0
    InterlockedAdd(gs_listed, count);
#else
    InterlockedAdd(gs_listed, min(listed, limit));
#endif
    GroupMemoryBarrierWithGroupSync();
    if (s == 0)
    {
#if MODE == 1
        buffer.InterlockedAdd(44, gs_listed);  // FroxelGrid::indexCount (entries stored)
        if (gs_cut != 0) buffer.InterlockedAdd(48, gs_cut);
        if (gs_dropped != 0) buffer.InterlockedAdd(52, gs_dropped);
        if (gs_candidateCount > FROXEL_CANDIDATES) buffer.InterlockedAdd(60, 1);
#endif
        buffer.InterlockedMax(56, gs_max);
    }
    if (s >= g.slices) return;
    const uint froxel = froxelIndex(g, tile, s);
#if MODE == 0
    buffer.Store2(g.headerBase + froxel * 8, uint2(0, sceneCount | count << 16));  // counts < 32769 (FroxelScan reads both)
#else
    const uint storedCount = min(listed, limit);
    buffer.Store2(g.headerBase + froxel * 8, uint2(first, storedCount));
    // The head from groupshared memory (its words are not shared with the tail: listMax is even).
    const uint headCount = min(sorted, limit), keys = s * FROXEL_SORTED_MAX;
    for (uint e = 0; e < headCount; e += 2)
    {
        const uint a = froxelEntryOf(0xFFFFu - (gs_keys[keys + e] & 0xFFFFu));
        const uint b = e + 1 < headCount ? froxelEntryOf(0xFFFFu - (gs_keys[keys + e + 1] & 0xFFFFu)) : 0u;
        buffer.Store(g.indexBase + (first + e) * 2, a | b << 16);
    }
    // The tail's last entry when its word is half full.
    if (stored != 0 && (stored & 1) != 0) buffer.Store(g.indexBase + (first + sorted + stored - 1) * 2, pendingEntry);
#endif
}
