// unx-kernel: cs_6_6 main
// Heat haze deflection field (FEATURES_GAME 0.A-8; VolumeCommon.hlsli). One group per 8 x 8 haze tile of 1/4-resolution
// texels, one thread per texel. The view ray through the texel's centre (full-resolution pixel 4 q + 2) passes each haze
// particle of the tile at impact parameter b (the closest point's offset q_hat b from the particle centre); the ray bends
// by theta = sum G_i(b_i) q_hat_i (small angles: the gradients of the integrated index add linearly), and
//   D = pixel(normalise(dir + theta)) - pixel(dir)          (full-resolution pixels, the far-field limit of the bent ray)
// M displaces the HDR lookup of pixels behind the haze by D (1 - z_p / z_b) (FrameResources.h). distortionDepth = the
// nearest haze particle's front (reversed-Z device depth; 0 = none). |D| >= 8 px (outside the linear condition) sets
// VOLUME_STATUS_HAZE_LARGE.
// P[0].x constants.
#include "Passes/Volume/VolumeCommon.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID)
{
    const VolumeConstants c = volumeConstants();
    const uint2 q = gid.xy * VOLUME_HAZE_TILE + gtid.xy;
    if (q.x >= c.hazeWidth || q.y >= c.hazeHeight) return;
    const uint tileIndex = gid.y * c.hazeTilesX + gid.x;
    StructuredBuffer<uint> counts = ResourceDescriptorHeap[c.hazeCounts];
    StructuredBuffer<uint> starts = ResourceDescriptorHeap[c.hazeStarts];
    StructuredBuffer<uint2> entries = ResourceDescriptorHeap[c.hazeEntries];
    StructuredBuffer<VolumeRecord> records = ResourceDescriptorHeap[c.records];
    const uint first = starts[tileIndex];
    const uint count = first < c.hazeEntryCapacity ? min(counts[tileIndex], c.hazeEntryCapacity - first) : 0u;
    const float2 pixel = float2(q) * VOLUME_HAZE_SCALE + 0.5f * VOLUME_HAZE_SCALE;
    const float3 dir = normalize(volumeRayAt(pixel));
    float3 theta = 0;
    float front = 0;
    for (uint i = 0; i < count; ++i)
    {
        const VolumeRecord r = records[entries[first + i].x];
        const float t = dot(r.centre, dir);
        const float3 qv = dir * t - r.centre;
        const float b = length(qv);
        if (!(t > 0) || b >= r.mass) continue;
        if (b > 0) theta += volumeHazeGradient(b, r.radius, r.a) * (qv / b);
        const float zc = -mul((float3x3)g_view, r.centre).z;
        front = max(front, g_nearPlane / max(zc - r.mass, g_nearPlane));
    }
    float2 D = 0;
    if (any(theta != 0)) D = volumePixelOf(normalize(dir + theta)) - volumePixelOf(dir);
    if (dot(D, D) >= 64.0f) volumeStatus(c, VOLUME_STATUS_HAZE_LARGE);
    RWTexture2D<float2> offset = ResourceDescriptorHeap[c.distortionOffset];
    RWTexture2D<float> depth = ResourceDescriptorHeap[c.distortionDepth];
    offset[q] = D;
    depth[q] = front;
}
