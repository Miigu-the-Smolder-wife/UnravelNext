// Fluid surface reconstruction (track W, B8; FEATURES_GAME 0.B): the 0.5 level set of the particles' quadratic
// B-spline density on a sparse node grid of spacing h (the particle spacing), extracted by marching cubes over the
// active blocks. For particles on a regular lattice of spacing h the density is exactly 1 inside (partition of unity)
// and exactly 0.5 midway past the last particle row, so the surface of a lattice block is its box of N h^3 volume: the
// simulation's own surface definition. Deterministic: the density sums are fixed-point integers (order independent),
// slots and triangles are ordered by block index, cell and case table order.
// Root constants (Bindless.hlsli P[8]):
//   P[0] particles SRV (raw), previous particles SRV (raw; 0xFFFFFFFF = none), particle count, stride (bytes; the
//        position is the first float3)
//   P[1] alpha (frame time between previous and current), scale (particle units -> node units), h (m), table size
//   P[2] node blocks per axis xyz, block pool capacity
//   P[3] origin xyz (world position of node 0), triangle capacity
//   P[4] table UAV, scan UAV, density UAV, counters UAV
//   P[5] cell info UAV, block triangles UAV, vertices UAV, case table SRV
//   P[6] dispatch arguments UAV, draw arguments UAV, velocities UAV, axis signs (bit a: axis a of the particles' space is
//        negated in the output's; an odd count also reverses each triangle's winding: CCW outside stays CCW outside)
//   P[7] particle velocity offset (bytes; 0xFFFFFFFF = none), velocity scale (particle units -> m/s), previous slot
//        offset (bytes to the uint index of the particle in the previous buffer; 0xFFFFFFFF = the same index), first
//        record (1: every triangle past the drawn ones is retired, FluidTail.hlsl)
// Nodes hold 4 uints: density (fixed point 2^20) and the density-weighted velocity (signed, m/s x 2^16) for the surface
// velocities (motion vectors: V's triangle stream, request 20260926_W_gpu_triangle_stream.md).
#ifndef UNX_WATER_FLUID_SURFACE_HLSLI
#define UNX_WATER_FLUID_SURFACE_HLSLI
#include "Bindless.hlsli"

#define FS_BLOCK 8u                 // nodes (and marching cubes cells) per block axis
#define FS_BLOCK_NODES 512u
#define FS_SCALE 1048576.0          // 2^20 density fixed point
#define FS_MOMENTUM 65536.0         // 2^16 weighted velocity fixed point (m/s)
#define FS_ISO 524288u              // 0.5 in fixed point
#define FS_WINDOW 11u               // count/emit window: nodes [8b - 1, 8b + 9] (cell corners 0..8, gradients -1..9)
#define FS_CASE_STRIDE 25u          // case table: triangle count, then up to 8 triangles x 3 edges
#define FS_COUNTER_ACTIVE 0u
#define FS_COUNTER_TRIANGLES 1u
#define FS_COUNTER_OVERFLOW 2u      // pool or triangle capacity exceeded (counted, reported)
#define FS_COUNTER_TAIL_FROM 3u     // triangles [from, to) to retire this record (drawn now, drawn before: FluidTail.hlsl)
#define FS_COUNTER_TAIL_TO 4u
#define FS_COUNTER_DRAWN 15u        // triangles drawn by the previous record (kept across records; FluidClear skips it)
#define FS_SUMS(tableSize) (tableSize)
#define FS_SLOTS(tableSize) (tableSize + 2048u)

uint fsCount() { return P[0].z; }
uint fsStride() { return P[0].w; }
float fsAlpha() { return asfloat(P[1].x); }
float fsScale() { return asfloat(P[1].y); }
float fsH() { return asfloat(P[1].z); }
uint fsTableSize() { return P[1].w; }
uint3 fsBlocks() { return P[2].xyz; }
uint fsMaxBlocks() { return P[2].w; }
float3 fsOrigin() { return asfloat(P[3].xyz); }
uint fsMaxTriangles() { return P[3].w; }
bool fsFirstRecord() { return P[7].w != 0; }
float3 fsAxes() { return float3((P[6].w & 1u) ? -1.0 : 1.0, (P[6].w & 2u) ? -1.0 : 1.0, (P[6].w & 4u) ? -1.0 : 1.0); }
bool fsMirrored() { return (countbits(P[6].w & 7u) & 1u) != 0; }

// ---- W3 seam (engine 2): basins whose water the fluid enters --------------------------------------------------------
// Inside a closed basin the fluid and the bath are one medium: the fluid surface below the bath surface is no boundary
// and is cut away exactly, triangle by triangle, at y = level + eta(x, z) (the pool's field, W2), in the renderer's
// axes. The basins are a table (FluidBasin.hlsl writes it: P[8] = (table SRV, basin count)), eta at sample (lx, lz) /
// (L / 256) of each basin's local frame (Pool.cpp axes(): local x = dx cos - dz sin + Lx / 2, local z = dx sin + dz cos +
// Lz / 2). Only cells in the band |y - level| <= band + 2 h over a basin take two triangle slots per case triangle (a
// triangle cut to a quad is two); cells wholly below it are under the water (zero area); every other cell is emitted
// exactly as before. The band is this frame's max |eta| of the basin's field (FluidBasin.hlsl): measured, so no part of
// the surface leaves it and the out-of-band decisions are exact.
#define FS_CLIP_BASINS_MAX 64u
#define FS_CLIP_FAR 1e30f
struct FsBasin { float3 centre; uint field; float c, s, sizeX, sizeZ, band; };
uint fsBasinCount() { return min(P[8].y, FS_CLIP_BASINS_MAX); }
FsBasin fsBasin(uint b)
{
    ByteAddressBuffer table = ResourceDescriptorHeap[P[8].x];  // 48 B records (FluidBasin.hlsl)
    const uint4 a = table.Load4(b * 48), q = table.Load4(b * 48 + 16);
    FsBasin r;
    r.centre = asfloat(a.xyz); r.field = a.w; r.c = asfloat(q.x); r.s = asfloat(q.y); r.sizeX = asfloat(q.z); r.sizeZ = asfloat(q.w);
    r.band = asfloat(table.Load(b * 48 + 32));
    return r;
}
float2 fsBasinLocal(FsBasin b, float3 p)
{
    const float dx = p.x - b.centre.x, dz = p.z - b.centre.z;
    return float2(dx * b.c - dz * b.s + 0.5f * b.sizeX, dx * b.s + dz * b.c + 0.5f * b.sizeZ);
}
// Bilinear eta of the pool field (RGBA32F 257^2, .x) at local (lx, lz).
float fsEta(FsBasin b, float2 l)
{
    Texture2D<float4> field = ResourceDescriptorHeap[b.field];
    const float2 u = clamp(l / float2(b.sizeX, b.sizeZ) * 256.0f, 0.0f, 256.0f);
    const uint2 i = min((uint2)u, 255u);
    const float2 f = u - (float2)i;
    const float e00 = field.Load(int3(i, 0)).x, e10 = field.Load(int3(i + uint2(1, 0), 0)).x;
    const float e01 = field.Load(int3(i + uint2(0, 1), 0)).x, e11 = field.Load(int3(i + uint2(1, 1), 0)).x;
    return lerp(lerp(e00, e10, f.x), lerp(e01, e11, f.x), f.y);
}
// Signed height above the water of the basin over p (renderer axes): < 0 is under a bath surface; FS_CLIP_FAR outside
// every basin.
float fsAboveWater(float3 p)
{
    [loop] for (uint k = 0; k < fsBasinCount(); ++k)
    {
        const FsBasin b = fsBasin(k);
        const float2 l = fsBasinLocal(b, p);
        if (l.x < 0 || l.y < 0 || l.x > b.sizeX || l.y > b.sizeZ) continue;
        return p.y - (b.centre.y + fsEta(b, l));
    }
    return FS_CLIP_FAR;
}
// The centre of marching cubes cell `cell` (node coordinates of its lowest corner) in the renderer's axes: one
// expression for the count and the emit, so both passes decide the band identically.
float3 fsCellCentre(int3 cell) { return fsAxes() * (fsOrigin() + ((float3)cell + 0.5f) * fsH()); }
// A cell wholly under a basin's water: over the basin and below its band (the field's measured max |eta|, so the whole
// surface is above the cell): its triangles are all under the surface and are emitted with zero area (one slot each).
bool fsUnderBand(float3 centre)
{
    const float reach = 2.0f * fsH();
    [loop] for (uint k = 0; k < fsBasinCount(); ++k)
    {
        const FsBasin b = fsBasin(k);
        const float2 l = fsBasinLocal(b, centre);
        if (l.x < reach || l.y < reach || l.x > b.sizeX - reach || l.y > b.sizeZ - reach) continue;
        if (centre.y < b.centre.y - b.band - reach) return true;
    }
    return false;
}
// A marching cubes cell (its centre, renderer axes) in the waterline band of a basin: it takes two slots per triangle.
bool fsInBand(float3 centre)
{
    const float reach = 2.0f * fsH();
    [loop] for (uint k = 0; k < fsBasinCount(); ++k)
    {
        const FsBasin b = fsBasin(k);
        const float2 l = fsBasinLocal(b, centre);
        if (l.x < -reach || l.y < -reach || l.x > b.sizeX + reach || l.y > b.sizeZ + reach) continue;
        if (abs(centre.y - b.centre.y) <= b.band + reach) return true;
    }
    return false;
}

uint fsBlockIndex(int3 b) { uint3 n = fsBlocks(); return ((uint)b.z * n.y + (uint)b.y) * n.x + (uint)b.x; }
bool fsInside(int3 b) { uint3 n = fsBlocks(); return all(b >= 0) && all(b < (int3)n); }
int3 fsBlockCoord(uint i) { uint3 n = fsBlocks(); return int3(i % n.x, (i / n.x) % n.y, i / (n.x * n.y)); }

bool fsHasVelocity() { return P[7].x != 0xFFFFFFFFu; }
// Particle i's element in the previous tick's buffer (the physics fluid re-sorts at the tick start and keeps each
// particle's tick-start slot: FluidGpu.hlsl Particle.origin).
uint fsPrevious(uint i)
{
    if (P[7].z == 0xFFFFFFFFu) return i;
    ByteAddressBuffer cur = ResourceDescriptorHeap[P[0].x];
    return cur.Load(i * fsStride() + P[7].z);
}
// Particle i's velocity (m/s) at the frame's time.
float3 fsVelocity(uint i)
{
    ByteAddressBuffer cur = ResourceDescriptorHeap[P[0].x];
    float3 v = asfloat(cur.Load3(i * fsStride() + P[7].x));
    if (P[0].y != 0xFFFFFFFFu)
    {
        ByteAddressBuffer prev = ResourceDescriptorHeap[P[0].y];
        v = lerp(asfloat(prev.Load3(fsPrevious(i) * fsStride() + P[7].x)), v, fsAlpha());
    }
    return v * asfloat(P[7].y);
}
// Particle i in node units at the frame's time (current, or blended with the previous tick's position).
float3 fsParticle(uint i)
{
    ByteAddressBuffer cur = ResourceDescriptorHeap[P[0].x];
    float3 x = asfloat(cur.Load3(i * fsStride()));
    if (P[0].y != 0xFFFFFFFFu)
    {
        ByteAddressBuffer prev = ResourceDescriptorHeap[P[0].y];
        x = lerp(asfloat(prev.Load3(fsPrevious(i) * fsStride())), x, fsAlpha());
    }
    return x * fsScale();
}
void fsWeights(float f, out float3 w) { w = float3(0.5 * (1.5 - f) * (1.5 - f), 0.75 - (f - 1.0) * (f - 1.0), 0.5 * (f - 0.5) * (f - 0.5)); }

// Node j: density and weighted velocity (0 outside the active blocks).
uint4 fsNode(int3 j)
{
    int3 b = j >> 3; if (!fsInside(b)) return 0;
    RWByteAddressBuffer table = ResourceDescriptorHeap[P[4].x];
    uint slot = table.Load(4 * fsBlockIndex(b)); if (slot == 0) return 0;
    RWByteAddressBuffer density = ResourceDescriptorHeap[P[4].z];
    int3 l = j - b * 8; return density.Load4(16 * ((slot - 1) * FS_BLOCK_NODES + ((uint)l.z * 8 + (uint)l.y) * 8 + (uint)l.x));
}
uint fsDensity(int3 j) { return fsNode(j).x; }
// Cube corner c (bit 0 = x, 1 = y, 2 = z) and edge e (4 along x, 4 along y, 4 along z; FluidSurface.cpp kEdges).
static const uint2 kFsEdges[12] = { uint2(0, 1), uint2(2, 3), uint2(4, 5), uint2(6, 7), uint2(0, 2), uint2(1, 3), uint2(4, 6), uint2(5, 7), uint2(0, 4), uint2(1, 5), uint2(2, 6), uint2(3, 7) };
int3 fsCorner(uint c) { return int3(c & 1, (c >> 1) & 1, c >> 2); }
#endif
