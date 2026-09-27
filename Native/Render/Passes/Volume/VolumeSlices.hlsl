// unx-kernel: cs_6_6 main
// Particle media on the froxel grid (request 20260925_FX_particle_render_rules 3b; VolumeCommon.hlsli). One group per froxel
// tile, one thread per depth slice (S's layout, FroxelCommon.hlsli: slice s spans nodes s .. s + 1 of the tile-centre ray).
// For every media entry of the tile whose view-depth range meets the slice, the exact line integral of its tent-mass
// density over the slice's segment of the tile-centre ray (VolumeTentLine: piecewise cubic, Simpson per piece):
//   A_i = |ray| m_i / r_i^3 int_{z0}^{z1} prod max(0, 1 - |ray z - c_i| / r_i) dz   (ray: unit view depth)
//   tau_p = sum A_i extinction_i,  S_raw = sum A_i source_i,  S_p = S_raw (1 - e^-tau_p) / tau_p
// (S_p: the slice's media source at the slice entry, self-attenuated; exact when the media are uniform along the slice,
// the froxel band's condition). Writes volumeSlices: (tile, s) = tau_p, (tile, S + s) = S_p (nit, before exposure).
// Entries past the list buffer were dropped by VolumeSetup (status bit set there).
// P[0].x constants.
#include "Passes/Volume/VolumeCommon.hlsli"
#include "Passes/Atmosphere/FroxelCommon.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID)
{
    const VolumeConstants c = volumeConstants();
    ByteAddressBuffer lists = ResourceDescriptorHeap[c.froxelLights];
    const FroxelGrid g = lists.Load<FroxelGrid>(0);
    const uint tileIndex = gid.x, s = gtid.x;
    if (tileIndex >= g.gridX * g.gridY || s >= g.slices) return;
    const uint2 tile = uint2(tileIndex % g.gridX, tileIndex / g.gridX);
    StructuredBuffer<uint> counts = ResourceDescriptorHeap[c.mediaCounts];
    StructuredBuffer<uint> starts = ResourceDescriptorHeap[c.mediaStarts];
    StructuredBuffer<uint4> entries = ResourceDescriptorHeap[c.mediaEntries];
    StructuredBuffer<VolumeRecord> records = ResourceDescriptorHeap[c.records];
    const float3 ray = froxelTileRay(g, tile);  // camera-relative, unit view depth
    const float speed = length(ray);
    const float zs0 = froxelNodeDepth(g, s), zs1 = froxelNodeDepth(g, s + 1);
    float3 tau = 0, source = 0;
    // The cell holding the tile at every level of the loose quadtree (VolumeCommon.hlsli).
    const uint2 tiles = uint2(g.gridX, g.gridY);
    const uint levels = volumeLevelCount(tiles);
    uint base = 0;
    for (uint L = 0; L < levels; ++L)
    {
        const uint2 dims = volumeLevelDims(tiles, L), cellXY = tile >> L;
        const uint cell = base + cellXY.y * dims.x + cellXY.x;
        base += dims.x * dims.y;
        const uint first = starts[cell];
        const uint count = first < c.mediaEntryCapacity ? min(counts[cell], c.mediaEntryCapacity - first) : 0u;
        for (uint i = 0; i < count; ++i)
        {
            const uint4 e = entries[first + i];
            if (!volumeEntryHolds(e, tile)) continue;
            const float z0 = max(f16tof32(e.y), zs0), z1 = min(f16tof32(e.y >> 16), zs1);
            if (!(z1 > z0)) continue;
            const VolumeRecord r = records[e.x];
            const float segment = volumeTentLine(float3(0, 0, 0), ray, z0, z1, r.centre, r.radius);
            if (!(segment > 0)) continue;
            const float A = speed * r.mass / (r.radius * r.radius * r.radius) * segment;
            tau += A * r.a;
            source += A * r.b;
        }
    }
    if (!all(isfinite(tau)) || !all(isfinite(source)))
    {
        volumeStatus(c, VOLUME_STATUS_NONFINITE);  // reported; VolumeSetup isolates non-finite records, so this is a defect here
        tau = 0;
        source = 0;
    }
    const float3 selfAttenuation = float3(tau.x > 1e-6f ? (1 - exp(-tau.x)) / tau.x : 1 - 0.5f * tau.x,
                                          tau.y > 1e-6f ? (1 - exp(-tau.y)) / tau.y : 1 - 0.5f * tau.y,
                                          tau.z > 1e-6f ? (1 - exp(-tau.z)) / tau.z : 1 - 0.5f * tau.z);
    RWTexture3D<float4> slices = ResourceDescriptorHeap[c.volumeSlices];
    slices[uint3(tile, s)] = float4(min(tau, 65504.0f), 0);
    slices[uint3(tile, g.slices + s)] = float4(min(source * selfAttenuation, 65504.0f), 0);
}
