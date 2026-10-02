// unx-kernel: cs_6_6 main
// unx-variants: STEP=0,1,2,3
// Decal frame setup and tile lists (E, A7; Decal.hlsli).
//   STEP 0: clear the tile lists (one thread per tile; thread 0 writes the header)
//   STEP 1: per decal, its frame record: the box in camera-relative world space (through the instance's transform
//           when attached) and the inverse (unit cube coordinates of a camera-relative point); the frame's opacity =
//           the record's x the lifetime fade (the frame's clock g_time against the record's fade-in and fade-out
//           spans) x the screen-size fade (Unreal's FadeScreenSize: with screen = the box's largest half extent over
//           its distance and k = fadeScreenSize x 2 tan(half fov x) / view width x 600, saturate((screen - k) / (k / 2)));
//           a decal faded to 0 enters no tile list (STEP 3)
//   STEP 2: per 16 x 16 tile (one group), the device depth range of its geometry pixels (sky pixels left out)
//   STEP 3: per decal (one group of 64), every tile of its screen rectangle whose frustum slice (the tile's side planes
//           through the camera, its depth range) meets the box: separating-plane tests with the 4 side planes and the
//           view-depth slab, no false negatives. Appends the decal to the tile's list (<= DECAL_PER_TILE; more: counted,
//           DECAL_STATUS_TILE_FULL).
// P[0] = { records SRV, frames UAV, tiles UAV (raw), decal count }, P[1] = { depth SRV, tile depth UAV (float2 per tile),
// tilesX, tilesY }; frame constants b1 = the view.
#include "Passes/Decal/Decal.hlsli"

float3 decalRayAt(float2 pixel)  // camera-relative ray through a pixel, unit view depth
{
    const float2 ndc = float2(pixel.x / g_viewWidth * 2 - 1, 1 - pixel.y / g_viewHeight * 2);
    const float4 p = mul(g_invViewProj, float4(ndc, 1, 1));  // device depth 1 = view depth g_nearPlane
    return (p.xyz / p.w - g_cameraPosition) / g_nearPlane;
}

#if STEP == 2
groupshared uint gs_min, gs_max;
[numthreads(16, 16, 1)]
void main(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
    if (gi == 0) { gs_min = 0x7F800000u; gs_max = 0u; }
    GroupMemoryBarrierWithGroupSync();
    const uint2 pixel = gid.xy * DECAL_TILE_PX + gtid.xy;
    if (all(pixel < uint2(g_viewWidth, g_viewHeight)))
    {
        Texture2D<float> depth = ResourceDescriptorHeap[P[1].x];
        const float z = depth.Load(int3(pixel, 0));
        if (z > 0)  // positive floats order like their bits
        {
            InterlockedMin(gs_min, asuint(z));
            InterlockedMax(gs_max, asuint(z));
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0)
    {
        RWStructuredBuffer<float2> tileDepth = ResourceDescriptorHeap[P[1].y];
        tileDepth[gid.y * P[1].z + gid.x] = gs_max == 0u ? float2(0, 0) : float2(asfloat(gs_min), asfloat(gs_max));
    }
}
#else
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
    RWByteAddressBuffer tiles = ResourceDescriptorHeap[P[0].z];
    const uint tilesX = P[1].z, tilesY = P[1].w;
#if STEP == 0
    if (id.x == 0) tiles.Store4(0, uint4(tilesX, tilesY, 0, 0));
    if (id.x >= tilesX * tilesY) return;
    const uint base = DECAL_TILES_HEADER_BYTES + id.x * DECAL_TILE_WORDS * 4u;
    [unroll] for (uint w = 0; w < DECAL_TILE_WORDS; ++w) tiles.Store(base + 4u * w, 0u);
#elif STEP == 1
    if (id.x >= P[0].w) return;
    StructuredBuffer<DecalRecord> records = ResourceDescriptorHeap[P[0].x];
    RWStructuredBuffer<DecalFrame> frames = ResourceDescriptorHeap[P[0].y];
    const DecalRecord r = records[id.x];
    float3 ax = float3(r.box[0].x, r.box[1].x, r.box[2].x), ay = float3(r.box[0].y, r.box[1].y, r.box[2].y);
    float3 az = float3(r.box[0].z, r.box[1].z, r.box[2].z), c = float3(r.box[0].w, r.box[1].w, r.box[2].w);
    if (r.instance != DECAL_NONE)
    {
        const GpuInstance inst = loadInstance(r.instance);
        const float3x3 R = float3x3(inst.objectToWorld[0].xyz, inst.objectToWorld[1].xyz, inst.objectToWorld[2].xyz);
        const float3 t = float3(inst.objectToWorld[0].w, inst.objectToWorld[1].w, inst.objectToWorld[2].w);
        ax = mul(R, ax); ay = mul(R, ay); az = mul(R, az);
        c = mul(R, c) + t;
    }
    c -= g_cameraPosition;
    // inverse of [ax ay az]: rows cross(ay, az), cross(az, ax), cross(ax, ay) over the determinant
    const float3 r0 = cross(ay, az), r1 = cross(az, ax), r2 = cross(ax, ay);
    const float det = dot(ax, r0);
    DecalFrame f;
    const float inv = abs(det) > 1e-30f ? 1.0f / det : 0.0f;
    f.toDecal[0] = float4(r0 * inv, -dot(r0, c) * inv);
    f.toDecal[1] = float4(r1 * inv, -dot(r1, c) * inv);
    f.toDecal[2] = float4(r2 * inv, -dot(r2, c) * inv);
    f.centre = c; f.material = r.material;
    f.axisX = ax; f.instance = r.instance;
    f.axisY = ay; f.priority = r.priority;
    f.axisZ = az; f.order = r.order;
    float opacity = det != 0 ? r.opacity : 0.0f;
    float life = 1;
    if (r.fadeInDuration > 0) life = min(life, (g_time - r.fadeInStart) / r.fadeInDuration);
    if (r.fadeOutDuration > 0) life = min(life, 1 - (g_time - r.fadeOutStart) / r.fadeOutDuration);
    opacity *= saturate(life);
    if (r.fadeScreenSize > 0)
    {
        const float screen = max(length(ax), max(length(ay), length(az))) / max(length(c), 1e-6f);
        const float k = r.fadeScreenSize * (2 / g_proj[0][0]) / g_viewWidth * 600;
        opacity *= saturate((screen - k) / (k * 0.5f));
    }
    f.opacity = opacity; f.cosFadeStart = r.cosFadeStart; f.cosFadeEnd = r.cosFadeEnd; f.edge = r.edge;
    f.color = r.color; f.channels = r.channels;
    f.emissive = r.emissive; f.pad = 0;
    frames[id.x] = f;
#else
    const uint decal = gid.x;
    if (decal >= P[0].w) return;
    StructuredBuffer<DecalFrame> frames = ResourceDescriptorHeap[P[0].y];
    StructuredBuffer<float2> tileDepth = ResourceDescriptorHeap[P[1].y];
    const DecalFrame d = frames[decal];
    if (!(d.opacity > 0)) return;
    // screen rectangle of the 8 corners (a corner at or behind the near plane: the whole view)
    float2 lo = float2(1e30f, 1e30f), hi = float2(-1e30f, -1e30f);
    bool crosses = false;
    [unroll] for (uint k = 0; k < 8; ++k)
    {
        const float3 p = d.centre + d.axisX * ((k & 1) ? 1 : -1) + d.axisY * ((k & 2) ? 1 : -1) + d.axisZ * ((k & 4) ? 1 : -1);
        const float4 clip = mul(g_viewProj, float4(p + g_cameraPosition, 1));
        if (clip.w <= g_nearPlane) { crosses = true; continue; }
        const float2 ndc = clip.xy / clip.w;
        const float2 px = float2((ndc.x * 0.5f + 0.5f) * g_viewWidth, (0.5f - 0.5f * ndc.y) * g_viewHeight);
        lo = min(lo, px);
        hi = max(hi, px);
    }
    int2 t0 = crosses ? int2(0, 0) : int2(floor(lo / DECAL_TILE_PX));
    int2 t1 = crosses ? int2(tilesX, tilesY) - 1 : int2(floor(hi / DECAL_TILE_PX));
    t0 = max(t0, int2(0, 0));
    t1 = min(t1, int2(tilesX, tilesY) - 1);
    if (any(t1 < t0)) return;
    const uint w = uint(t1.x - t0.x + 1), h = uint(t1.y - t0.y + 1);
    // view axis and the box's extent along it
    const float3 forward = normalize(-float3(g_view[2].xyz));  // camera looks down -Z of view space
    const float rz = abs(dot(forward, d.axisX)) + abs(dot(forward, d.axisY)) + abs(dot(forward, d.axisZ));
    const float dz = dot(forward, d.centre);
    for (uint i = gi; i < w * h; i += 64u)
    {
        const uint2 tile = uint2(t0) + uint2(i % w, i / w);
        const float2 zr = tileDepth[tile.y * tilesX + tile.x];
        if (zr.y == 0) continue;  // no geometry in the tile
        // view-depth slab of the tile's geometry: depth = near / z (reversed Z)
        const float zNear = g_nearPlane / zr.y, zFar = g_nearPlane / zr.x;
        if (dz + rz < zNear || dz - rz > zFar) continue;
        // side planes through the camera and the tile's corner rays, normals inward
        const float2 p0 = float2(tile * DECAL_TILE_PX), p1 = min(p0 + DECAL_TILE_PX, float2(g_viewWidth, g_viewHeight));
        const float3 c00 = decalRayAt(p0), c10 = decalRayAt(float2(p1.x, p0.y)), c11 = decalRayAt(p1), c01 = decalRayAt(float2(p0.x, p1.y));
        const float3 planes[4] = { cross(c00, c10), cross(c10, c11), cross(c11, c01), cross(c01, c00) };
        const float3 inside = c00 + c10 + c11 + c01;
        bool separated = false;
        [unroll] for (uint q = 0; q < 4; ++q)
        {
            float3 n = planes[q];
            if (dot(n, inside) < 0) n = -n;
            const float r = abs(dot(n, d.axisX)) + abs(dot(n, d.axisY)) + abs(dot(n, d.axisZ));
            if (dot(n, d.centre) < -r) separated = true;
        }
        if (separated) continue;
        const uint base = DECAL_TILES_HEADER_BYTES + (tile.y * tilesX + tile.x) * DECAL_TILE_WORDS * 4u;
        uint slot;
        tiles.InterlockedAdd(base, 1u, slot);
        if (slot < DECAL_PER_TILE) tiles.InterlockedOr(base + 4u + (slot / 2u) * 4u, (decal & 0xFFFFu) << (16u * (slot & 1u)));
        else if (slot == DECAL_PER_TILE)
        {
            tiles.InterlockedOr(8u, DECAL_STATUS_TILE_FULL);
            tiles.InterlockedAdd(12u, 1u);
        }
    }
#endif
}
#endif
