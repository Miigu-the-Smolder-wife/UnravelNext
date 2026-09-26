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
//   P[6] dispatch arguments UAV, draw arguments UAV, velocities UAV, 0
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
