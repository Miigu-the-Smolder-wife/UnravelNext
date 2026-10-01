// Tile lights (RENDERER_REDESIGN_V2 14.1 "타일 광원 목록", 14.2; L2; owner A): per 8 x 8 shading tile, the lights of the
// tile's froxel lists classified NEAR / FAR (Passes/Common/LightNearFar.hlsli) and the FAR lights' vector irradiance at
// the tile's four corner surface points, written by TileLights.hlsl and read by ShadeOpaque: a pixel skips the diffuse
// term of its FAR lights in the per-light loop (their specular stays per pixel until D-23) and adds
// front x max(0, n . bilerp(E_corners)) once. Every shadow caster is NEAR until 14.3's classification exists.
// Record, 96 B per tile (tile index = y x tilesX + x):
//   0..47   E at the corners: pixels (0,0), (7,0), (0,7), (7,7) of the tile (float3 x 4, camera-relative positions)
//   48      flags: bit 0 = the record applies (else every light is NEAR for the tile: an edge tile, a sky corner, more
//           than 4 slices or more than 64 entries in a slice), bits 8..15 first slice, bits 16..23 slice count (<= 4)
//   52      counters: NEAR entries | FAR lights << 16 (statistics)
//   56..87  NEAR mask per slice (4 x uint2; bit = entry position in that slice's froxel list; a clear bit = FAR)
//   88..95  pad
#ifndef UNX_TILE_LIGHTS_HLSLI
#define UNX_TILE_LIGHTS_HLSLI

#define TILE_LIGHTS_RECORD_BYTES 96u
#define TILE_LIGHTS_SLICES 4u

struct TileLightRecord
{
    float3 e0, e1, e2, e3;
    uint flags, counts;
    uint2 mask[4];
};

TileLightRecord tileLightsRecord(uint srv, uint tileIndex)
{
    ByteAddressBuffer b = ResourceDescriptorHeap[srv];
    const uint o = tileIndex * TILE_LIGHTS_RECORD_BYTES;
    TileLightRecord r;
    r.e0 = asfloat(b.Load3(o));
    r.e1 = asfloat(b.Load3(o + 12));
    r.e2 = asfloat(b.Load3(o + 24));
    r.e3 = asfloat(b.Load3(o + 36));
    const uint2 fc = b.Load2(o + 48);
    r.flags = fc.x;
    r.counts = fc.y;
    r.mask[0] = b.Load2(o + 56);
    r.mask[1] = b.Load2(o + 64);
    r.mask[2] = b.Load2(o + 72);
    r.mask[3] = b.Load2(o + 80);
    return r;
}
bool tileLightsValid(TileLightRecord r) { return (r.flags & 1u) != 0; }
uint tileLightsFirstSlice(TileLightRecord r) { return (r.flags >> 8) & 0xFFu; }
uint tileLightsSliceCount(TileLightRecord r) { return (r.flags >> 16) & 0xFFu; }
// Bilinear E at pixel offset (tx, ty) in 0..7 of the tile.
float3 tileLightsIrradiance(TileLightRecord r, uint2 t)
{
    const float fx = t.x / 7.0, fy = t.y / 7.0;
    return lerp(lerp(r.e0, r.e1, fx), lerp(r.e2, r.e3, fx), fy);
}

#endif
