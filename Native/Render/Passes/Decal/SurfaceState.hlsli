// Surface state field (FEATURES_GAME 5.2, WORLD_VFX 10.4; A7). Owner: E. Readers: M's material resolve (wet, frost,
// scorch, dust and blood layers, snow height), through surfaceStateAt.
//
// The GPU copy of the VFX context's surface state (NativeVfx nv_surface_delta -> unx::surface::SurfaceField): 1 m
// bricks of 4^3 voxels (0.25 m), each 464 B: int32 key[3], float t0 (seconds after the field's time base), unorm8 x 5
// per voxel (wet, scorch, frost, dust, blood; byte 16 + 5 v + c), int16 snow height change per voxel (1/2048 m; byte
// 336 + 2 v), voxel v = (i * 4 + j) * 4 + k for x, y, z steps i, j, k. A channel with half-life h decays analytically:
// value(t) = value(t0) 2^-((t - t0) / h). Bricks are found through an open-addressing hash table of 16 B entries
// (int32 key[3], slot; slot 0xFFFFFFFF = empty), linear probing, at most maxProbe + 1 probes (a structural bound the
// CPU keeps). Constants (raw, 48 B): tableMask, maxProbe, bricks, 0, now (seconds after the time base), 1 / half-life
// per channel (0 = no decay), 0.
#ifndef UNX_SURFACE_STATE_HLSLI
#define UNX_SURFACE_STATE_HLSLI
#include "Bindless.hlsli"

#define SURFACE_NONE 0xFFFFFFFFu
#define SURFACE_BRICK_BYTES 464u
#define SURFACE_VOXEL 0.25f

struct SurfaceContext
{
    uint constants, table, pool;  // SURFACE_NONE: no field
};
struct SurfaceSample
{
    float wet, scorch, frost, dust, blood;  // [0, 1]
    float snow;                             // height change (m)
};

uint surfaceHash(int3 key)  // same on the CPU (SurfaceField)
{
    uint h = uint(key.z) * 747796405u + 2891336453u;
    h = ((h >> ((h >> 28u) + 4u)) ^ h) * 277803737u;
    h ^= uint(key.y) * 2654435761u;
    h = ((h >> ((h >> 28u) + 4u)) ^ h) * 277803737u;
    h ^= uint(key.x) * 2246822519u;
    h = ((h >> ((h >> 28u) + 4u)) ^ h) * 277803737u;
    return (h >> 22u) ^ h;
}
uint surfaceFind(SurfaceContext c, int3 key)
{
    ByteAddressBuffer constants = ResourceDescriptorHeap[c.constants];
    ByteAddressBuffer table = ResourceDescriptorHeap[c.table];
    const uint mask = constants.Load(0), maxProbe = constants.Load(4);
    uint i = surfaceHash(key) & mask;
    for (uint p = 0; p <= maxProbe; ++p)
    {
        const int4 e = asint(table.Load4(i * 16u));
        if (uint(e.w) == SURFACE_NONE) return SURFACE_NONE;
        if (all(e.xyz == key)) return uint(e.w);
        i = (i + 1u) & mask;
    }
    return SURFACE_NONE;
}
int surfaceFloorDiv4(int v) { return v >= 0 ? v / 4 : -((-v + 3) / 4); }

// Trilinear over the 8 voxel centres around the point (0 where a brick is absent), each brick decayed to now.
SurfaceSample surfaceStateAt(SurfaceContext c, float3 worldPos)
{
    SurfaceSample s = (SurfaceSample)0;
    if (c.constants == SURFACE_NONE) return s;
    ByteAddressBuffer constants = ResourceDescriptorHeap[c.constants];
    ByteAddressBuffer pool = ResourceDescriptorHeap[c.pool];
    const float now = asfloat(constants.Load(16));
    float inv[6];
    [unroll] for (uint k = 0; k < 6; ++k) inv[k] = asfloat(constants.Load(20 + 4 * k));
    const float3 q = worldPos / SURFACE_VOXEL - 0.5f;
    const float3 b = floor(q), f = q - b;
    int3 cachedKey = int3(0x7FFFFFFF, 0, 0);
    uint cachedSlot = SURFACE_NONE;
    float decay[6] = { 1, 1, 1, 1, 1, 1 };
    [unroll] for (uint corner = 0; corner < 8; ++corner)
    {
        const int3 o = int3(corner & 1, (corner >> 1) & 1, corner >> 2);
        const int3 voxel = int3(b) + o;
        const float w = (o.x ? f.x : 1 - f.x) * (o.y ? f.y : 1 - f.y) * (o.z ? f.z : 1 - f.z);
        const int3 key = int3(surfaceFloorDiv4(voxel.x), surfaceFloorDiv4(voxel.y), surfaceFloorDiv4(voxel.z));
        if (any(key != cachedKey))
        {
            cachedKey = key;
            cachedSlot = surfaceFind(c, key);
            if (cachedSlot != SURFACE_NONE)
            {
                const float t0 = asfloat(pool.Load(cachedSlot * SURFACE_BRICK_BYTES + 12u));
                [unroll] for (uint k = 0; k < 6; ++k) decay[k] = inv[k] > 0 ? exp2(-(now - t0) * inv[k]) : 1.0f;
            }
        }
        if (cachedSlot == SURFACE_NONE || !(w > 0)) continue;
        const int3 local = voxel - key * 4;
        const uint v = uint((local.x * 4 + local.y) * 4 + local.z);
        const uint base = cachedSlot * SURFACE_BRICK_BYTES;
        const uint byte0 = 16u + 5u * v;
        const uint2 words = pool.Load2(base + (byte0 & ~3u));
        const uint shift = (byte0 & 3u) * 8u;
        // 5 bytes starting at byte0 inside the two words
        const uint lo = words.x >> shift, hi = shift ? (words.y << (32u - shift)) : 0u;
        const uint packed4 = lo | hi;
        const uint fifth = shift ? (words.y >> shift) >> 0u : words.y;
        const float wet = (packed4 & 0xFFu) / 255.0f, scorch = ((packed4 >> 8) & 0xFFu) / 255.0f;
        const float frost = ((packed4 >> 16) & 0xFFu) / 255.0f, dust = ((packed4 >> 24) & 0xFFu) / 255.0f;
        const float blood = (fifth & 0xFFu) / 255.0f;
        const uint snowByte = 336u + 2u * v;
        const uint snowWord = pool.Load(base + (snowByte & ~3u));
        const int snow16 = int(snowWord << (16u - (snowByte & 2u) * 8u)) >> 16;
        s.wet += w * wet * decay[0];
        s.scorch += w * scorch * decay[1];
        s.frost += w * frost * decay[2];
        s.dust += w * dust * decay[3];
        s.blood += w * blood * decay[4];
        s.snow += w * (snow16 / 2048.0f) * decay[5];
    }
    return s;
}
#endif
