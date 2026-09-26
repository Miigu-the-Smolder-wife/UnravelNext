// unx-kernel: cs_6_6 main
// Froxel light lists (ARCHITECTURE 2.4, INTERFACES 7.4). One group per screen tile, one thread per depth slice:
//  1. the tile's frustum (four planes through the camera) culls every light's bounding sphere, the group's threads taking
//     the lights in turn (512 lights x 14.4 k tiles at 4K = 7.4 M sphere tests): the scene's, then the FX particle lights
//     of the buffer's tail (froxelLightTotal, A3: + 14.4 k F tests at 4K);
//  2. each slice keeps the candidates whose bounds reach its froxel: view-depth range, bounding sphere of the froxel
//     (not for the last slice, which extends to infinity), spot cone, emitter plane of one-sided area lights;
//  3. ordered by importance at the froxel (peak intensity x distance window / squared distance), at most lights_max
//     (FROXEL_LIST_MAX compiled); ties by light index, so the lists do not depend on the candidates' order;
//  4. header words and 16-bit indices into the froxel's fixed run (FroxelCommon.hlsli).
// Truncation (more than lights_max lights reach a froxel) is counted in the header.
// P[0].x froxelLights UAV (raw; header written by FroxelBegin), P[0].y lights_max, P[0].z slot of light SRV
// (StructuredBuffer<uint>: shadow slot or VSM_LOCAL_NONE per scene light; 0xFFFFFFFF: no local shadows): entries carry
// bit 15 when their light has a shadow slot (froxelLightShadowed).
// P[0].w tile readers SRV (Texture2D<float2>, FroxelTileDepth.hlsl; 0xFFFFFFFF: every tile): a tile none of whose 3 x 3
// neighbourhood holds a read pixel (planar views: tiles without mirror pixels) gets empty lists and no culling.
// Frame constants of the view.
#include "Bindless.hlsli"
#include "Frame.hlsli"
#include "Scene.hlsli"
#include "Passes/Atmosphere/FroxelCommon.hlsli"

#define FROXEL_CANDIDATES 1024u
#define FROXEL_LIST_MAX 32u

groupshared uint gs_candidates[FROXEL_CANDIDATES];
groupshared uint gs_candidateCount;
groupshared uint gs_keys[64 * FROXEL_LIST_MAX];  // per slice, descending: importance code << 16 | (0xFFFF - light)
groupshared uint gs_listed;
groupshared uint gs_overflow, gs_dropped, gs_max;

// Monotonic 16-bit code of a positive importance: 1/256 octave steps over [2^-64, 2^192).
uint importanceCode(float importance) { return (uint)clamp((log2(max(importance, 5.4e-20)) + 64) * 256, 0.0, 65535.0); }

[numthreads(64, 1, 1)]
void main(uint3 gid : SV_GroupID, uint s : SV_GroupIndex)
{
    RWByteAddressBuffer buffer = ResourceDescriptorHeap[P[0].x];
    const FroxelGrid g = buffer.Load<FroxelGrid>(0);
    const uint listMax = min(P[0].y, FROXEL_LIST_MAX);
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
                buffer.Store(g.headerBase + froxel * 4, (froxel * g.indexStride) << 6);
            }
            return;
        }
    }
    if (s == 0)
    {
        gs_candidateCount = 0;
        gs_listed = 0;
        gs_overflow = 0;
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

    // 1. Tile frustum.
    const uint lightTotal = froxelLightTotal();
    for (uint base = 0; base < lightTotal; base += 64)
    {
        const uint li = base + s;
        if (li >= lightTotal) break;
        const GpuLight l = loadLight(li);
        const float3 c = l.position - g_cameraPosition;
        const float r = froxelLightRadius(l);
        bool inside = dot(c, forward) >= -r;
        [unroll] for (uint p = 0; p < 4; ++p) inside = inside && dot(plane[p], c) >= -r;
        if (inside)
        {
            uint slot;
            InterlockedAdd(gs_candidateCount, 1, slot);
            if (slot < FROXEL_CANDIDATES) gs_candidates[slot] = li;
        }
    }
    GroupMemoryBarrierWithGroupSync();
    const uint candidates = min(gs_candidateCount, FROXEL_CANDIDATES);

    // 2-3. This slice's list.
    uint count = 0, reached = 0;
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
        const uint keys = s * FROXEL_LIST_MAX;
        for (uint j = 0; j < candidates; ++j)
        {
            const uint li = gs_candidates[j];
            const GpuLight l = loadLight(li);
            const float r = froxelLightRadius(l);
            const float3 v = centre - l.position;
            const float lz = dot(l.position - g_cameraPosition, forward);
            if (lz + r < z0 || lz - r > z1) continue;
            const float dist = length(v);
            if (!last && dist > r + radius) continue;
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
                    if (cosA * across - along * sinA > radius || along < -radius) continue;
                }
            }
            if (!last && (type == LIGHT_RECT || type == LIGHT_DISK) && dot(v, l.forward) < -radius) continue;
            ++reached;
            const float dn = max(dist - radius, 0.0);
            const float extent = max(l.size.x, l.size.y);
            const float importance = froxelPeakIntensity(l) * froxelWindow(l, dn) / (dn * dn + 0.25 * radius * radius + extent * extent);
            const uint key = importanceCode(importance) << 16 | (0xFFFFu - li);
            // Insertion into the descending list, keeping the first listMax.
            if (count == listMax && key <= gs_keys[keys + count - 1]) continue;
            uint at = count < listMax ? count++ : listMax - 1;
            while (at > 0 && gs_keys[keys + at - 1] < key)
            {
                gs_keys[keys + at] = gs_keys[keys + at - 1];
                --at;
            }
            gs_keys[keys + at] = key;
        }
        if (reached > listMax)
        {
            InterlockedAdd(gs_overflow, 1);
            InterlockedAdd(gs_dropped, reached - listMax);
        }
        InterlockedMax(gs_max, reached);
    }

    // 4. Output and statistics.
    InterlockedAdd(gs_listed, count);
    GroupMemoryBarrierWithGroupSync();
    if (s == 0)
    {
        buffer.InterlockedAdd(44, gs_listed);  // FroxelGrid::indexCount
        if (gs_overflow != 0) buffer.InterlockedAdd(48, gs_overflow);
        if (gs_dropped != 0) buffer.InterlockedAdd(52, gs_dropped);
        buffer.InterlockedMax(56, gs_max);
        if (gs_candidateCount > FROXEL_CANDIDATES) buffer.InterlockedAdd(60, 1);
    }
    if (s >= g.slices) return;
    const uint froxel = froxelIndex(g, tile, s), first = froxel * g.indexStride;
    buffer.Store(g.headerBase + froxel * 4, first << 6 | count);
    const uint keys = s * FROXEL_LIST_MAX;
    for (uint e = 0; e < count; e += 2)
    {
        uint a = 0xFFFFu - (gs_keys[keys + e] & 0xFFFFu);
        uint b = e + 1 < count ? 0xFFFFu - (gs_keys[keys + e + 1] & 0xFFFFu) : 0u;
        if (P[0].z != 0xFFFFFFFFu)
        {
            StructuredBuffer<uint> slotOf = ResourceDescriptorHeap[P[0].z];
            a |= a < g_lightCount && slotOf[a] != 0xFFFFu ? 0x8000u : 0u;  // FX lights (index >= g_lightCount): no slot
            if (e + 1 < count) b |= b < g_lightCount && slotOf[b] != 0xFFFFu ? 0x8000u : 0u;
        }
        buffer.Store(g.indexBase + (first + e) * 2, a | b << 16);
    }
}
